#include "cam/time_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "core/util.h"

#define TM_MAX_SEG_SECONDS 7200.0  // cap a single file's drawn span (crash safety)
#define TM_GAP_SLACK 8.0           // segments closer than this count as contiguous

struct TimeMachine {
  char base[1024];

  SDL_Thread *thread;
  SDL_mutex *mutex;

  SDL_atomic_t quit;
  SDL_atomic_t seek_gen;  // bumped on every seek/stop request

  // --- guarded by mutex ---
  char safe_title[160];
  int want_rescan;
  int playing;
  double req_seek;  // target unix time for the latest seek; <0 = stop/close
  double playhead;  // current playback unix time (thread-updated)
  int has_video;

  TmSeg *segs;
  int seg_count;
  int seg_cap;
  double span_min, span_max;

  // latest decoded frame (YUV420P, planes packed)
  uint8_t *yuv;
  int yuv_cap;
  int fw, fh;
  uint32_t frame_serial;
};

// ---------------------------------------------------------------- scanning

static int tm_seg_cmp(const void *a, const void *b) {
  double da = ((const TmSeg *)a)->start, db = ((const TmSeg *)b)->start;
  return (da < db) ? -1 : (da > db) ? 1 : 0;
}

#ifdef _WIN32
static double tm_filetime_to_unix(FILETIME ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  // 100ns ticks since 1601 -> seconds since 1970.
  return (double)((long long)(u.QuadPart / 10000000ULL) - 11644473600LL);
}

static double tm_local_to_unix(int yyyy, int mon, int day, int hh, int mm, int ss) {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = yyyy - 1900;
  t.tm_mon = mon - 1;
  t.tm_mday = day;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  t.tm_isdst = -1;
  time_t u = mktime(&t);
  return (u == (time_t)-1) ? 0.0 : (double)u;
}

// Windows file APIs must use the wide (UTF-16) variants: camera folders carry the
// camera title verbatim (e.g. Cyrillic "Новая 1"), and the ANSI FindFirstFileA
// cannot match non-codepage names — that made the strip read "no recordings"
// even when .ts files existed. Paths are converted UTF-8 <-> UTF-16 at the API
// boundary; TmSeg.path stays UTF-8 (FFmpeg's avformat_open_input wants UTF-8 on
// Windows and converts to wide internally).
static int tm_u8_to_w(const char *s, wchar_t *w, int wcap) {
  return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wcap);
}
static int tm_w_to_u8(const wchar_t *w, char *s, int scap) {
  return WideCharToMultiByte(CP_UTF8, 0, w, -1, s, scap, NULL, NULL);
}

static void tm_scan_dir(const char *base, const char *safe_title, TmSeg **out, int *out_count,
                        double *omin, double *omax) {
  *out = NULL;
  *out_count = 0;
  *omin = 0;
  *omax = 0;
  if (!safe_title[0]) return;

  int cap = 0, n = 0;
  TmSeg *arr = NULL;
  double mn = 0, mx = 0;

  char cam_glob[1300];
  snprintf(cam_glob, sizeof(cam_glob), "%srecordings\\%s\\*", base, safe_title);
  wchar_t cam_glob_w[1400];
  if (tm_u8_to_w(cam_glob, cam_glob_w, 1400) <= 0) return;

  WIN32_FIND_DATAW fd;
  HANDLE hd = FindFirstFileW(cam_glob_w, &fd);
  if (hd == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    char dayname[260];
    if (tm_w_to_u8(fd.cFileName, dayname, sizeof(dayname)) <= 0) continue;
    int dd = 0, MM = 0, yyyy = 0;
    if (sscanf(dayname, "%d.%d.%d", &dd, &MM, &yyyy) != 3) continue;
    if (yyyy < 1970 || MM < 1 || MM > 12 || dd < 1 || dd > 31) continue;

    char day_glob[1500];
    snprintf(day_glob, sizeof(day_glob), "%srecordings\\%s\\%s\\*.ts", base, safe_title, dayname);
    wchar_t day_glob_w[1600];
    if (tm_u8_to_w(day_glob, day_glob_w, 1600) <= 0) continue;
    WIN32_FIND_DATAW ff;
    HANDLE hf = FindFirstFileW(day_glob_w, &ff);
    if (hf == INVALID_HANDLE_VALUE) continue;
    do {
      if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      char fname[260];
      if (tm_w_to_u8(ff.cFileName, fname, sizeof(fname)) <= 0) continue;
      int hh = 0, mm = 0, ss = 0, eh = 0, em = 0, es = 0;
      double start = 0, end = 0;
      if (sscanf(fname, "%d-%d-%d__%d-%d-%d", &hh, &mm, &ss, &eh, &em, &es) == 6) {
        // Self-describing closed segment: exact span from the filename.
        start = tm_local_to_unix(yyyy, MM, dd, hh, mm, ss);
        end = tm_local_to_unix(yyyy, MM, dd, eh, em, es);
        if (start <= 0) continue;
        if (end < start) end += 86400.0;  // segment crossed local midnight
      } else if (sscanf(fname, "%d-%d-%d", &hh, &mm, &ss) == 3) {
        // Still-open (or crash-truncated) segment: end estimated from mtime.
        start = tm_local_to_unix(yyyy, MM, dd, hh, mm, ss);
        if (start <= 0) continue;
        end = tm_filetime_to_unix(ff.ftLastWriteTime);
      } else {
        continue;
      }
      if (end <= start) end = start + 1.0;
      if (end - start > TM_MAX_SEG_SECONDS) end = start + TM_MAX_SEG_SECONDS;

      if (n >= cap) {
        int ncap = cap ? cap * 2 : 64;
        TmSeg *na = (TmSeg *)SDL_realloc(arr, (size_t)ncap * sizeof(TmSeg));
        if (!na) break;
        arr = na;
        cap = ncap;
      }
      arr[n].start = start;
      arr[n].end = end;
      snprintf(arr[n].path, sizeof(arr[n].path), "%srecordings\\%s\\%s\\%s", base, safe_title,
               dayname, fname);
      if (n == 0 || start < mn) mn = start;
      if (n == 0 || end > mx) mx = end;
      n++;
    } while (FindNextFileW(hf, &ff));
    FindClose(hf);
  } while (FindNextFileW(hd, &fd));
  FindClose(hd);

  if (arr && n > 1) qsort(arr, (size_t)n, sizeof(TmSeg), tm_seg_cmp);
  *out = arr;
  *out_count = n;
  *omin = mn;
  *omax = mx;
}
#else
static void tm_scan_dir(const char *base, const char *safe_title, TmSeg **out, int *out_count,
                        double *omin, double *omax) {
  (void)base;
  (void)safe_title;
  *out = NULL;
  *out_count = 0;
  *omin = 0;
  *omax = 0;
}
#endif

static void tm_rescan(TimeMachine *tm) {
  char base[1024], title[160];
  SDL_LockMutex(tm->mutex);
  snprintf(base, sizeof(base), "%s", tm->base);
  snprintf(title, sizeof(title), "%s", tm->safe_title);
  SDL_UnlockMutex(tm->mutex);

  TmSeg *arr = NULL;
  int n = 0;
  double mn = 0, mx = 0;
  tm_scan_dir(base, title, &arr, &n, &mn, &mx);

  SDL_LockMutex(tm->mutex);
  // Only adopt if the camera hasn't changed under us mid-scan.
  if (strcmp(title, tm->safe_title) == 0) {
    SDL_free(tm->segs);
    tm->segs = arr;
    tm->seg_count = n;
    tm->seg_cap = n;
    tm->span_min = mn;
    tm->span_max = mx;
    arr = NULL;
  }
  SDL_UnlockMutex(tm->mutex);
  if (arr) SDL_free(arr);
}

// Returns index of the segment covering `t`, else the earliest segment starting
// at/after `t` (snap forward), else -1. Caller holds the mutex.
static int tm_find_seg_locked(TimeMachine *tm, double t) {
  int best_after = -1;
  for (int i = 0; i < tm->seg_count; i++) {
    if (t >= tm->segs[i].start && t <= tm->segs[i].end) return i;
    if (tm->segs[i].start >= t) {
      if (best_after < 0 || tm->segs[i].start < tm->segs[best_after].start) best_after = i;
    }
  }
  return best_after;
}

// ---------------------------------------------------------------- playback

typedef struct TmPlay {
  AVFormatContext *fmt;
  AVCodecContext *dec;
  int vstream;
  AVRational tb;
  int64_t st_start;  // stream start_time (pts base), 0 if none
} TmPlay;

static void tm_play_close(TmPlay *p) {
  if (p->dec) avcodec_free_context(&p->dec);
  if (p->fmt) avformat_close_input(&p->fmt);
  p->fmt = NULL;
  p->dec = NULL;
  p->vstream = -1;
  p->tb = (AVRational){0, 1};
  p->st_start = 0;
}

static int tm_play_open(TmPlay *p, const char *path) {
  tm_play_close(p);
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return -1;
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    avformat_close_input(&fmt);
    return -1;
  }
  int vs = -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      vs = (int)i;
      break;
    }
  }
  if (vs < 0) {
    avformat_close_input(&fmt);
    return -1;
  }
  const AVCodec *codec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
  AVCodecContext *dec = codec ? avcodec_alloc_context3(codec) : NULL;
  if (!dec) {
    avformat_close_input(&fmt);
    return -1;
  }
  avcodec_parameters_to_context(dec, fmt->streams[vs]->codecpar);
  if (avcodec_open2(dec, codec, NULL) < 0) {
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return -1;
  }
  p->fmt = fmt;
  p->dec = dec;
  p->vstream = vs;
  p->tb = fmt->streams[vs]->time_base;
  int64_t st = fmt->streams[vs]->start_time;
  p->st_start = (st == AV_NOPTS_VALUE) ? 0 : st;
  return 0;
}

static void tm_play_seek_offset(TmPlay *p, double offset_s) {
  if (!p->fmt || p->vstream < 0 || offset_s <= 0.5) return;
  double tbd = av_q2d(p->tb);
  if (tbd <= 0) return;
  int64_t ts = p->st_start + (int64_t)(offset_s / tbd);
  avformat_seek_file(p->fmt, p->vstream, INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
  if (p->dec) avcodec_flush_buffers(p->dec);
}

static void tm_publish_frame(TimeMachine *tm, struct SwsContext **psws, int *sw, int *sh,
                             enum AVPixelFormat *sfmt, uint8_t **conv, int *ccap, AVFrame *frame) {
  int w = frame->width & ~1;
  int h = frame->height & ~1;
  if (w < 2 || h < 2) return;
  if (!*psws || *sw != w || *sh != h || *sfmt != (enum AVPixelFormat)frame->format) {
    if (*psws) sws_freeContext(*psws);
    *psws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format, w, h,
                           AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    *sw = w;
    *sh = h;
    *sfmt = (enum AVPixelFormat)frame->format;
  }
  if (!*psws) return;
  int need = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
  if (need <= 0) return;
  if (*ccap < need) {
    SDL_free(*conv);
    *conv = (uint8_t *)SDL_malloc((size_t)need);
    *ccap = *conv ? need : 0;
    if (!*conv) return;
  }
  uint8_t *dst[4];
  int ls[4];
  if (av_image_fill_arrays(dst, ls, *conv, AV_PIX_FMT_YUV420P, w, h, 1) < 0) return;
  sws_scale(*psws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst, ls);

  SDL_LockMutex(tm->mutex);
  if (tm->yuv_cap < need) {
    SDL_free(tm->yuv);
    tm->yuv = (uint8_t *)SDL_malloc((size_t)need);
    tm->yuv_cap = tm->yuv ? need : 0;
  }
  if (tm->yuv && tm->yuv_cap >= need) {
    memcpy(tm->yuv, *conv, (size_t)need);
    tm->fw = w;
    tm->fh = h;
    tm->frame_serial++;
  }
  SDL_UnlockMutex(tm->mutex);
}

static int tm_thread_fn(void *userdata) {
  TimeMachine *tm = (TimeMachine *)userdata;
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  TmPlay play = {0};
  play.vstream = -1;
  double cur_start = 0, cur_end = 0;
  int cur_idx = -1;

  AVFrame *frame = av_frame_alloc();
  AVPacket *pkt = av_packet_alloc();
  struct SwsContext *sws = NULL;
  int sw = 0, sh = 0;
  enum AVPixelFormat sfmt = AV_PIX_FMT_NONE;
  uint8_t *conv = NULL;
  int conv_cap = 0;

  int paced = 0;
  double anchor_disp = 0;
  Uint32 anchor_ticks = 0;
  int my_seek_gen = SDL_AtomicGet(&tm->seek_gen);

  while (!SDL_AtomicGet(&tm->quit)) {
    if (!frame || !pkt) break;

    SDL_LockMutex(tm->mutex);
    int rescan = tm->want_rescan;
    tm->want_rescan = 0;
    int playing = tm->playing;
    double seekto = tm->req_seek;
    // Read the generation under the same lock as req_seek so a new generation is
    // never paired with a stale target.
    int gen = SDL_AtomicGet(&tm->seek_gen);
    SDL_UnlockMutex(tm->mutex);

    if (rescan) tm_rescan(tm);

    if (gen != my_seek_gen) {
      my_seek_gen = gen;
      tm_play_close(&play);
      cur_idx = -1;
      paced = 0;
      if (seekto < 0) {
        SDL_LockMutex(tm->mutex);
        tm->has_video = 0;
        SDL_UnlockMutex(tm->mutex);
        continue;
      }
      SDL_LockMutex(tm->mutex);
      int idx = tm_find_seg_locked(tm, seekto);
      TmSeg s;
      if (idx >= 0) s = tm->segs[idx];
      SDL_UnlockMutex(tm->mutex);

      if (idx >= 0 && tm_play_open(&play, s.path) == 0) {
        cur_idx = idx;
        cur_start = s.start;
        cur_end = s.end;
        double offset = seekto - s.start;
        if (offset < 0) offset = 0;
        tm_play_seek_offset(&play, offset);
        SDL_LockMutex(tm->mutex);
        tm->playhead = (seekto > s.start) ? seekto : s.start;
        tm->has_video = 1;
        SDL_UnlockMutex(tm->mutex);
      } else {
        SDL_LockMutex(tm->mutex);
        tm->playhead = seekto;
        tm->has_video = 0;
        SDL_UnlockMutex(tm->mutex);
      }
      continue;
    }

    if (!playing || !play.fmt) {
      paced = 0;
      SDL_Delay(15);
      continue;
    }

    int r = av_read_frame(play.fmt, pkt);
    if (r < 0) {
      av_packet_unref(pkt);
      tm_play_close(&play);
      // Advance to the next segment in time order (skips gaps automatically).
      SDL_LockMutex(tm->mutex);
      int ni = cur_idx + 1;
      int have = (ni >= 0 && ni < tm->seg_count);
      TmSeg s;
      if (have) s = tm->segs[ni];
      if (!have) tm->has_video = 0;
      SDL_UnlockMutex(tm->mutex);
      if (have && tm_play_open(&play, s.path) == 0) {
        cur_idx = ni;
        cur_start = s.start;
        cur_end = s.end;
        paced = 0;
      } else {
        SDL_Delay(40);
      }
      continue;
    }
    if (pkt->stream_index != play.vstream) {
      av_packet_unref(pkt);
      continue;
    }

    if (avcodec_send_packet(play.dec, pkt) < 0) {
      av_packet_unref(pkt);
      continue;
    }
    av_packet_unref(pkt);

    for (;;) {
      int rr = avcodec_receive_frame(play.dec, frame);
      if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
      if (rr < 0) break;

      int64_t pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE) pts = frame->pts;
      double pts_s =
          (pts == AV_NOPTS_VALUE) ? 0.0 : (double)(pts - play.st_start) * av_q2d(play.tb);
      if (pts_s < 0) pts_s = 0;
      double disp = cur_start + pts_s;

      // Pace to real time (1x). Bail out promptly on quit / new seek / pause.
      if (!paced) {
        anchor_disp = disp;
        anchor_ticks = SDL_GetTicks();
        paced = 1;
      }
      double target = (disp - anchor_disp) * 1000.0;
      int bail = 0;
      for (;;) {
        if (SDL_AtomicGet(&tm->quit) || SDL_AtomicGet(&tm->seek_gen) != my_seek_gen) {
          bail = 1;
          break;
        }
        SDL_LockMutex(tm->mutex);
        int pl = tm->playing;
        SDL_UnlockMutex(tm->mutex);
        if (!pl) {
          bail = 1;
          break;
        }  // present-then-pause handled by re-anchor on resume
        double el = (double)(SDL_GetTicks() - anchor_ticks);
        if (el >= target) break;
        double rem = target - el;
        SDL_Delay(rem > 10 ? 10 : (Uint32)rem);
      }
      if (bail && SDL_AtomicGet(&tm->seek_gen) != my_seek_gen) {
        av_frame_unref(frame);
        break;
      }

      tm_publish_frame(tm, &sws, &sw, &sh, &sfmt, &conv, &conv_cap, frame);
      SDL_LockMutex(tm->mutex);
      tm->playhead = disp;
      tm->has_video = 1;
      SDL_UnlockMutex(tm->mutex);
      av_frame_unref(frame);

      SDL_LockMutex(tm->mutex);
      int still_playing = tm->playing;
      SDL_UnlockMutex(tm->mutex);
      if (!still_playing) break;
    }
  }

  tm_play_close(&play);
  if (sws) sws_freeContext(sws);
  SDL_free(conv);
  if (frame) av_frame_free(&frame);
  if (pkt) av_packet_free(&pkt);
  return 0;
}

// ---------------------------------------------------------------- public API

TimeMachine *time_machine_create(const char *recordings_base_dir) {
  TimeMachine *tm = (TimeMachine *)SDL_calloc(1, sizeof(TimeMachine));
  if (!tm) return NULL;
  if (recordings_base_dir && recordings_base_dir[0]) {
    snprintf(tm->base, sizeof(tm->base), "%s", recordings_base_dir);
  } else {
    char *b = SDL_GetBasePath();
    snprintf(tm->base, sizeof(tm->base), "%s", (b && *b) ? b : ".\\");
    if (b) SDL_free(b);
  }
  tm->req_seek = -1;
  tm->mutex = SDL_CreateMutex();
  if (!tm->mutex) {
    SDL_free(tm);
    return NULL;
  }
  SDL_AtomicSet(&tm->quit, 0);
  SDL_AtomicSet(&tm->seek_gen, 0);
  tm->thread = SDL_CreateThread(tm_thread_fn, "time-machine", tm);
  if (!tm->thread) {
    SDL_DestroyMutex(tm->mutex);
    SDL_free(tm);
    return NULL;
  }
  return tm;
}

void time_machine_destroy(TimeMachine *tm) {
  if (!tm) return;
  SDL_AtomicSet(&tm->quit, 1);
  SDL_AtomicAdd(&tm->seek_gen, 1);  // unblock any pacing wait
  SDL_WaitThread(tm->thread, NULL);
  SDL_free(tm->segs);
  SDL_free(tm->yuv);
  SDL_DestroyMutex(tm->mutex);
  SDL_free(tm);
}

void time_machine_set_camera(TimeMachine *tm, const char *title_utf8) {
  if (!tm) return;
  char safe[160] = {0};
  sanitize_folder_name_utf8(title_utf8 ? title_utf8 : "", safe, sizeof(safe));
  SDL_LockMutex(tm->mutex);
  if (strcmp(safe, tm->safe_title) != 0) {
    snprintf(tm->safe_title, sizeof(tm->safe_title), "%s", safe);
    tm->want_rescan = 1;
    tm->playing = 0;
    tm->req_seek = -1;  // stop/close current playback
    tm->has_video = 0;
    SDL_AtomicAdd(&tm->seek_gen, 1);
    SDL_UnlockMutex(tm->mutex);
    return;
  }
  SDL_UnlockMutex(tm->mutex);
}

void time_machine_request_rescan(TimeMachine *tm) {
  if (!tm) return;
  SDL_LockMutex(tm->mutex);
  tm->want_rescan = 1;
  SDL_UnlockMutex(tm->mutex);
}

void time_machine_seek(TimeMachine *tm, double unix_time) {
  if (!tm) return;
  SDL_LockMutex(tm->mutex);
  tm->req_seek = unix_time;
  tm->playing = 1;
  SDL_AtomicAdd(&tm->seek_gen, 1);
  SDL_UnlockMutex(tm->mutex);
}

void time_machine_set_playing(TimeMachine *tm, int playing) {
  if (!tm) return;
  SDL_LockMutex(tm->mutex);
  tm->playing = playing ? 1 : 0;
  SDL_UnlockMutex(tm->mutex);
}

int time_machine_is_playing(TimeMachine *tm) {
  if (!tm) return 0;
  SDL_LockMutex(tm->mutex);
  int p = tm->playing;
  SDL_UnlockMutex(tm->mutex);
  return p;
}

double time_machine_playhead(TimeMachine *tm) {
  if (!tm) return 0;
  SDL_LockMutex(tm->mutex);
  double p = tm->playhead;
  SDL_UnlockMutex(tm->mutex);
  return p;
}

int time_machine_has_video(TimeMachine *tm) {
  if (!tm) return 0;
  SDL_LockMutex(tm->mutex);
  int h = tm->has_video;
  SDL_UnlockMutex(tm->mutex);
  return h;
}

int time_machine_copy_segments(TimeMachine *tm, TmSeg *out, int max, double *out_min,
                               double *out_max) {
  if (!tm) return 0;
  SDL_LockMutex(tm->mutex);
  int n = tm->seg_count < max ? tm->seg_count : max;
  if (out && n > 0) memcpy(out, tm->segs, (size_t)n * sizeof(TmSeg));
  if (out_min) *out_min = tm->span_min;
  if (out_max) *out_max = tm->span_max;
  SDL_UnlockMutex(tm->mutex);
  return n;
}

int time_machine_fetch_frame(TimeMachine *tm, uint8_t **buf, int *cap, int *w, int *h,
                             uint32_t *serial) {
  if (!tm) return 0;
  int got = 0;
  SDL_LockMutex(tm->mutex);
  if (tm->frame_serial != *serial && tm->yuv && tm->fw > 0 && tm->fh > 0) {
    int need = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, tm->fw, tm->fh, 1);
    if (need > 0) {
      if (*cap < need) {
        uint8_t *nb = (uint8_t *)SDL_realloc(*buf, (size_t)need);
        if (nb) {
          *buf = nb;
          *cap = need;
        }
      }
      if (*cap >= need && *buf) {
        memcpy(*buf, tm->yuv, (size_t)need);
        *w = tm->fw;
        *h = tm->fh;
        *serial = tm->frame_serial;
        got = 1;
      }
    }
  }
  SDL_UnlockMutex(tm->mutex);
  return got;
}
