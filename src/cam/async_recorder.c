#include "cam/async_recorder.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <libavutil/error.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "cam/record.h"
#include "core/log.h"
#include "core/util.h"

// Queue caps. At a few Mbps these allow ~minute(s) of slack before the disk
// must catch up; if it cannot, the oldest packets are dropped so the worker is
// never blocked and memory stays bounded.
#define ASYNC_REC_MAX_PKTS 600
#define ASYNC_REC_MAX_BYTES (64 * 1024 * 1024)

typedef struct PktNode {
  AVPacket *pkt;
  int bytes;
  struct PktNode *next;
} PktNode;

struct AsyncRecorder {
  int cam_index;
  char base[1024];

  SDL_Thread *thread;
  SDL_mutex *mutex;
  SDL_cond *cond;

  // --- guarded by mutex ---
  int quit;
  int wake;          // set on any state change so the thread re-evaluates
  int want;          // desired recording state
  char title[256];   // camera display name (folder)

  AVCodecParameters *src_par;  // owned snapshot, NULL when no/invalidated source
  AVRational src_tb;
  int src_serial;    // bumped on set_source/invalidate to trigger a roll

  PktNode *head, *tail;
  int count;
  long long bytes;
  long long dropped;  // packets dropped due to a full queue (disk too slow)
};

static void async_recorder_log(AsyncRecorder *ar, const char *line) {
  if (!ar || !ar->base[0]) return;
  char log_path[1100];
  snprintf(log_path, sizeof(log_path), "%srecordings/recording.log", ar->base);
  append_log_line_utf8(log_path, line);
}

// Closes the active recorder and renames the just-finished segment from its
// open-time-only name (…\HH-MM-SS.ts) to a self-describing from→to name
// (…\HH-MM-SS__HH-MM-SS.ts), so the time-machine index gets exact spans straight
// from the filename instead of relying on file mtime. cur_path is cleared.
#ifdef _WIN32
static void ar_close_segment(Recorder *rec, char *cur_path) {
  if (rec->active) recorder_close(rec);
  if (!cur_path[0]) return;
  size_t len = strlen(cur_path);
  const char *bn = cur_path;
  for (const char *p = cur_path; *p; p++) {
    if (*p == '\\' || *p == '/') bn = p + 1;
  }
  if (len >= 4 && !strstr(bn, "__")) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char dst[1300];
    snprintf(dst, sizeof(dst), "%.*s__%02d-%02d-%02d.ts", (int)(len - 3), cur_path, (int)st.wHour,
             (int)st.wMinute, (int)st.wSecond);
    wchar_t ws[1400], wd[1400];
    if (MultiByteToWideChar(CP_UTF8, 0, cur_path, -1, ws, 1400) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, dst, -1, wd, 1400) > 0) {
      MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING);
    }
  }
  cur_path[0] = 0;
}
#else
static void ar_close_segment(Recorder *rec, char *cur_path) {
  if (rec->active) recorder_close(rec);
  cur_path[0] = 0;
}
#endif

static int async_recorder_thread_fn(void *userdata) {
  AsyncRecorder *ar = (AsyncRecorder *)userdata;
  Recorder rec = {0};

  AVCodecParameters *open_par = NULL;  // params for the currently-open file
  AVRational open_tb = {0, 0};
  int open_serial = -1;
  Uint32 seg_start = 0;
  Uint32 last_flush = 0;
  Uint32 retry_at = 0;
  char cur_path[1200] = {0};  // path of the currently-open segment (for finalize)

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  for (;;) {
    SDL_LockMutex(ar->mutex);
    while (!ar->quit && ar->head == NULL && !ar->wake) {
      SDL_CondWait(ar->cond, ar->mutex);
    }
    ar->wake = 0;
    int quit = ar->quit;
    int want = ar->want;
    int serial = ar->src_serial;
    char title[256];
    snprintf(title, sizeof(title), "%s", ar->title);

    PktNode *node = ar->head;
    if (node) {
      ar->head = node->next;
      if (!ar->head) ar->tail = NULL;
      ar->count--;
      ar->bytes -= node->bytes;
    }

    // When the source changed, snapshot the new params (deep copy) so we can
    // open the next file without holding the lock during disk I/O.
    AVCodecParameters *new_par = NULL;
    AVRational new_tb = {0, 0};
    int serial_changed = (serial != open_serial);
    if (serial_changed && ar->src_par) {
      new_par = avcodec_parameters_alloc();
      if (new_par) avcodec_parameters_copy(new_par, ar->src_par);
      new_tb = ar->src_tb;
    }
    SDL_UnlockMutex(ar->mutex);

    // ---------- unlocked: all disk work happens here ----------
    if (serial_changed) {
      // Source opened/reconnected/invalidated: roll the file.
      ar_close_segment(&rec, cur_path);
      if (open_par) avcodec_parameters_free(&open_par);
      open_par = new_par;  // may be NULL (invalidated)
      open_tb = new_tb;
      open_serial = serial;
      retry_at = 0;
    } else if (new_par) {
      avcodec_parameters_free(&new_par);  // defensive; shouldn't occur
    }

    if (!want) {
      if (rec.active) {
        ar_close_segment(&rec, cur_path);
        async_recorder_log(ar, "stopped");
      }
    }

    if (node) {
      if (want) {
        AVPacket *pkt = node->pkt;
        Uint32 now = SDL_GetTicks();
        int is_key = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
        int need_open = (!rec.active) || ((Uint32)(now - seg_start) >= RECORDER_SEGMENT_MS);
        if (need_open && is_key && open_par && now >= retry_at) {
          char safe[128] = {0};
          sanitize_folder_name_utf8(title, safe, sizeof(safe));
          if (!safe[0]) snprintf(safe, sizeof(safe), "Camera_%d", ar->cam_index);
          ar_close_segment(&rec, cur_path);
          char out_path[1200] = {0};
          int orc =
              recorder_open_dated_segment(&rec, open_par, open_tb, ar->base, safe, out_path,
                                          sizeof(out_path));
          if (orc != 0) {
            char line[1400];
            char ebuf[256] = {0};
            av_strerror(orc, ebuf, (int)sizeof(ebuf));
            snprintf(line, sizeof(line), "open failed (%d) err=%s", orc, ebuf);
            async_recorder_log(ar, line);
            if (orc == AVERROR_MUXER_NOT_FOUND) {
              async_recorder_log(ar, "hint: FFmpeg build missing mpegts muxer");
            }
            retry_at = now + 2000;
          } else {
            char line[1400];
            snprintf(line, sizeof(line), "started out=%s", out_path);
            async_recorder_log(ar, line);
            snprintf(cur_path, sizeof(cur_path), "%s", out_path);
            seg_start = now;
            last_flush = 0;
          }
        }
        if (rec.active) {
          recorder_write_video(&rec, pkt);
          if (!last_flush || (Uint32)(now - last_flush) >= RECORDER_FLUSH_MS) {
            recorder_flush(&rec);
            last_flush = now;
          }
        }
      }
      av_packet_free(&node->pkt);
      SDL_free(node);
    }

    // Exit only once the queue is fully drained, so a graceful stop keeps the
    // last buffered footage instead of dropping it.
    if (quit && node == NULL) break;
  }

  ar_close_segment(&rec, cur_path);
  if (open_par) avcodec_parameters_free(&open_par);
  return 0;
}

AsyncRecorder *async_recorder_create(int cam_index, const char *recordings_base_dir) {
  AsyncRecorder *ar = (AsyncRecorder *)SDL_calloc(1, sizeof(AsyncRecorder));
  if (!ar) return NULL;
  ar->cam_index = cam_index;
  ar->src_serial = 0;
  ar->src_tb = (AVRational){0, 0};

  if (recordings_base_dir && recordings_base_dir[0]) {
    snprintf(ar->base, sizeof(ar->base), "%s", recordings_base_dir);
  } else {
    char *base = SDL_GetBasePath();
    snprintf(ar->base, sizeof(ar->base), "%s", (base && *base) ? base : ".\\");
    if (base) SDL_free(base);
  }
  // NOTE: do NOT create the recordings directory here. async_recorder_create()
  // runs at the *start* of every camera worker, before it opens its stream. On a
  // cloud-synced tree (e.g. MEGA) N concurrent CreateDirectory calls can each
  // block for hundreds of ms, starving the RTSP opens (cameras stuck spinning).
  // The directory is created lazily, only when a segment is actually opened
  // (recorder_open_dated_segment -> ensure_dir_recursive_win32).

  ar->mutex = SDL_CreateMutex();
  ar->cond = SDL_CreateCond();
  if (!ar->mutex || !ar->cond) {
    if (ar->cond) SDL_DestroyCond(ar->cond);
    if (ar->mutex) SDL_DestroyMutex(ar->mutex);
    SDL_free(ar);
    return NULL;
  }

  ar->thread = SDL_CreateThread(async_recorder_thread_fn, "cam-recorder", ar);
  if (!ar->thread) {
    SDL_DestroyCond(ar->cond);
    SDL_DestroyMutex(ar->mutex);
    SDL_free(ar);
    return NULL;
  }
  return ar;
}

void async_recorder_free(AsyncRecorder *ar) {
  if (!ar) return;
  SDL_LockMutex(ar->mutex);
  ar->quit = 1;
  ar->wake = 1;
  SDL_CondSignal(ar->cond);
  SDL_UnlockMutex(ar->mutex);

  SDL_WaitThread(ar->thread, NULL);

  // Drain any remaining queue nodes (the thread drains on graceful stop, but be
  // defensive about anything left).
  PktNode *n = ar->head;
  while (n) {
    PktNode *next = n->next;
    av_packet_free(&n->pkt);
    SDL_free(n);
    n = next;
  }
  if (ar->src_par) avcodec_parameters_free(&ar->src_par);
  SDL_DestroyCond(ar->cond);
  SDL_DestroyMutex(ar->mutex);
  SDL_free(ar);
}

void async_recorder_set_source(AsyncRecorder *ar, const AVCodecParameters *par, AVRational tb) {
  if (!ar || !par) return;
  SDL_LockMutex(ar->mutex);
  if (ar->src_par) avcodec_parameters_free(&ar->src_par);
  ar->src_par = avcodec_parameters_alloc();
  if (ar->src_par) avcodec_parameters_copy(ar->src_par, par);
  ar->src_tb = tb;
  ar->src_serial++;
  ar->wake = 1;
  SDL_CondSignal(ar->cond);
  SDL_UnlockMutex(ar->mutex);
}

void async_recorder_invalidate_source(AsyncRecorder *ar) {
  if (!ar) return;
  SDL_LockMutex(ar->mutex);
  if (ar->src_par) avcodec_parameters_free(&ar->src_par);
  ar->src_par = NULL;
  ar->src_serial++;
  ar->wake = 1;
  SDL_CondSignal(ar->cond);
  SDL_UnlockMutex(ar->mutex);
}

void async_recorder_feed(AsyncRecorder *ar, int want, const char *title, const AVPacket *pkt) {
  if (!ar) return;
  SDL_LockMutex(ar->mutex);
  ar->want = want ? 1 : 0;
  if (title && *title) snprintf(ar->title, sizeof(ar->title), "%s", title);

  if (ar->want && pkt && pkt->size >= 0) {
    // Bound the queue: drop oldest until the new packet fits.
    while (ar->count >= ASYNC_REC_MAX_PKTS ||
           (ar->bytes + (long long)pkt->size) > (long long)ASYNC_REC_MAX_BYTES) {
      PktNode *old = ar->head;
      if (!old) break;
      ar->head = old->next;
      if (!ar->head) ar->tail = NULL;
      ar->count--;
      ar->bytes -= old->bytes;
      av_packet_free(&old->pkt);
      SDL_free(old);
      ar->dropped++;
    }
    AVPacket *clone = av_packet_alloc();
    if (clone && av_packet_ref(clone, pkt) == 0) {
      PktNode *n = (PktNode *)SDL_malloc(sizeof(PktNode));
      if (n) {
        n->pkt = clone;
        n->bytes = pkt->size;
        n->next = NULL;
        if (ar->tail) {
          ar->tail->next = n;
        } else {
          ar->head = n;
        }
        ar->tail = n;
        ar->count++;
        ar->bytes += pkt->size;
      } else {
        av_packet_free(&clone);
      }
    } else if (clone) {
      av_packet_free(&clone);
    }
  }

  ar->wake = 1;
  SDL_CondSignal(ar->cond);
  SDL_UnlockMutex(ar->mutex);
}
