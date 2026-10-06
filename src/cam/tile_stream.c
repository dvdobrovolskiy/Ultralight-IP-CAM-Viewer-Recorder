#include "cam/tile_stream.h"

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include "app/config.h"
#include "audio/audio_pipe.h"
#include "cam/async_recorder.h"
#include "cam/cam_cache.h"
#include "core/log.h"
#include "core/seh.h"
#include "core/util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _WIN32
void startup_trace_tile_sources(const char *base_dir, const TileStreamArgs *args, const CameraConfig *cams,
                                int cam_count) {
  if (!base_dir || !*base_dir || !args || !cams || cam_count <= 0) return;

  int used_main = 0;
  int used_preview = 0;
  int used_other = 0;
  int missing_main = 0;
  int missing_preview = 0;

  for (int i = 0; i < cam_count; i++) {
    const char *main_in = (cams[i].url && *cams[i].url) ? cams[i].url : NULL;
    const char *prev_in = (cams[i].preview_url && *cams[i].preview_url) ? cams[i].preview_url : NULL;
    if (!main_in) missing_main++;
    if (!prev_in) missing_preview++;

    char main_url[2048] = {0};
    char prev_url[2048] = {0};
    if (main_in) build_camera_url(cams[i].type, main_in, main_url, sizeof(main_url));
    if (prev_in) build_camera_url(cams[i].type, prev_in, prev_url, sizeof(prev_url));

    if (!args[i].url[0]) continue;
    if (main_url[0] && strcmp(args[i].url, main_url) == 0) {
      used_main++;
    } else if (prev_url[0] && strcmp(args[i].url, prev_url) == 0) {
      used_preview++;
    } else {
      used_other++;
    }
  }

  startup_tracef(base_dir,
                 "tiles_stream_sources used_main=%d used_preview=%d used_other=%d missing_main=%d missing_preview=%d",
                 used_main, used_preview, used_other, missing_main, missing_preview);
}
#endif

// Sleep in short slices, returning as soon as quit is set. Workers back off up
// to 8s between attempts; the UI thread joins them when the camera list is
// edited, and a plain SDL_Delay there would freeze the interface for the whole
// backoff. With sliced sleeps a stop completes in ~100ms.
static void sleep_quit_aware(SDL_atomic_t *quit, int ms) {
  while (ms > 0) {
    if (quit && SDL_AtomicGet(quit)) return;
    int step = (ms > 100) ? 100 : ms;
    SDL_Delay((Uint32)step);
    ms -= step;
  }
}

// Aborts the open/handshake once its deadline passes, no matter which phase
// FFmpeg is stuck in. Max time any camera can occupy a connection slot.
#define TILE_OPEN_DEADLINE_MS 15000

static int tile_stream_ffmpeg_interrupt_cb(void *opaque) {
  TileStreamArgs *a = (TileStreamArgs *)opaque;
  if (!a) return 0;
  if (a->quit && SDL_AtomicGet(a->quit)) return 1;
  if (a->open_deadline_ticks != 0 &&
      (Sint32)(SDL_GetTicks() - a->open_deadline_ticks) > 0) {
    return 1;
  }
  return 0;
}

// Global gate limiting concurrent avformat_open_input calls (see header).
static SDL_sem *g_open_gate = NULL;
static int g_open_gate_max = 0;

void tile_stream_init_open_gate(int max_concurrent) {
  if (!g_open_gate && max_concurrent > 0) {
    g_open_gate = SDL_CreateSemaphore((Uint32)max_concurrent);
    g_open_gate_max = max_concurrent;
  }
}

// Restore the gate to full capacity.
//
// A worker holds a slot only across avformat_open_input, and releases it on
// every normal path — but the SEH handler can terminate a worker mid-open (a
// crash inside FFmpeg, say), and that slot is then lost for good. Losing all of
// them is catastrophic and silent: every worker blocks forever waiting for a
// slot that will never be posted, so *every* camera sits on a spinner with no
// error anywhere. Recreating the semaphore each time the tiles are (re)prepared
// makes that unrecoverable state merely temporary.
//
// Only safe because prepare always runs after tile_streams_stop has joined
// every worker, so nothing can be waiting on the old semaphore.
static void open_gate_reset(void) {
  if (!g_open_gate || g_open_gate_max <= 0) return;
  SDL_DestroySemaphore(g_open_gate);
  g_open_gate = SDL_CreateSemaphore((Uint32)g_open_gate_max);
}

int tile_stream_thread_fn(void *userdata) {
  TileStreamArgs *a = (TileStreamArgs *)userdata;
  if (!a) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

#ifdef _WIN32
  if (a->base_dir[0]) startup_tracef(a->base_dir, "tile_thread_start idx=%d url_set=%d", a->index, a->url[0] ? 1 : 0);
#endif

  int backoff_ms = 500;
  int open_attempts = 0;
  Uint32 last_thumb_update = 0;
  Uint32 last_main_update = 0;
  Uint32 last_feed_update = 0;
  struct SwsContext *sws_thumb = NULL;
  struct SwsContext *sws_main = NULL;
  struct SwsContext *sws_feed = NULL;
  int sws_feed_src_w = 0, sws_feed_src_h = 0, sws_feed_dst_w = 0, sws_feed_dst_h = 0;
  enum AVPixelFormat sws_feed_src_fmt = AV_PIX_FMT_NONE;
  // Per-tile recorder: its own thread + bounded queue. This worker only clones
  // and enqueues packets; all disk I/O (open/roll/write/flush/close) runs on the
  // recorder thread so recording can never stall frame production or the UI.
  Uint32 rec_t0 = SDL_GetTicks();
  AsyncRecorder *ar = async_recorder_create(a->index, a->recordings_base_dir);
#ifdef _WIN32
  if (a->base_dir[0]) {
    startup_tracef(a->base_dir, "tile_rec_create idx=%d ok=%d dt_ms=%u", a->index, ar ? 1 : 0,
                   (unsigned)(SDL_GetTicks() - rec_t0));
  }
#endif

  AVFrame *frame = av_frame_alloc();
  AVFrame *yuv = av_frame_alloc();
  AVPacket *pkt = av_packet_alloc();
  if (!frame || !yuv || !pkt) {
    if (pkt) av_packet_free(&pkt);
    if (yuv) av_frame_free(&yuv);
    if (frame) av_frame_free(&frame);
    if (ar) async_recorder_free(ar);
    return 0;
  }

  uint8_t *yuv_thumb_buf = NULL;
  int yuv_thumb_buf_size = 0;
  int sws_thumb_src_w = 0, sws_thumb_src_h = 0;
  enum AVPixelFormat sws_thumb_src_fmt = AV_PIX_FMT_NONE;
  int sws_thumb_dst_w = 0, sws_thumb_dst_h = 0;

  uint8_t *yuv_main_buf = NULL;
  int yuv_main_buf_size = 0;
  uint8_t *yuv_feed_buf = NULL;
  int yuv_feed_buf_size = 0;
  int sws_main_src_w = 0, sws_main_src_h = 0;
  enum AVPixelFormat sws_main_src_fmt = AV_PIX_FMT_NONE;

  int logged_open_ok = 0;
  int logged_first_thumb = 0;
  int logged_first_main = 0;

  for (;;) {
    if (SDL_AtomicGet(a->quit)) break;
    if (!a->url[0]) {
      sleep_quit_aware(a->quit, 250);
      continue;
    }

    AVDictionary *opts = NULL;
    if (a->type == CAMERA_TYPE_RTSP) av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      av_dict_free(&opts);
      continue;
    }
    fmt->interrupt_callback.callback = tile_stream_ffmpeg_interrupt_cb;
    fmt->interrupt_callback.opaque = a;

    // Throttle simultaneous connection handshakes (see tile_stream_init_open_gate).
    Uint32 gate_t0 = SDL_GetTicks();
    int gated = 0;
    if (g_open_gate) {
      while (!SDL_AtomicGet(a->quit)) {
        if (SDL_SemWaitTimeout(g_open_gate, 100) == 0) {
          gated = 1;
          break;
        }
      }
      if (SDL_AtomicGet(a->quit)) {
        // The slot may already have been acquired just before quit was set —
        // leaving without releasing it would shrink the gate permanently.
        if (gated) SDL_SemPost(g_open_gate);
        avformat_free_context(fmt);
        av_dict_free(&opts);
        break;
      }
    }
    Uint32 open_t0 = SDL_GetTicks();
    // Deadline covers open + stream-info: a camera that can't complete its
    // handshake in this window fails fast and frees its slot instead of
    // starving the cameras that do answer.
    a->open_deadline_ticks = open_t0 + TILE_OPEN_DEADLINE_MS;
#ifdef _WIN32
    if (a->base_dir[0]) {
      startup_tracef(a->base_dir, "tile_open_begin idx=%d attempt=%d gate_wait_ms=%u", a->index,
                     open_attempts + 1, (unsigned)(open_t0 - gate_t0));
    }
#endif
    int err = avformat_open_input(&fmt, a->url, NULL, &opts);
    if (gated) SDL_SemPost(g_open_gate);
    av_dict_free(&opts);
    if (err < 0 || !fmt) {
      if (fmt) avformat_free_context(fmt);
#ifdef _WIN32
      open_attempts++;
      if (a->base_dir[0]) {
        char ebuf[160] = {0};
        av_strerror(err, ebuf, sizeof(ebuf));
        startup_tracef(a->base_dir, "tile_open_FAIL idx=%d attempt=%d dt_ms=%u err=%d (%s)", a->index,
                       open_attempts, (unsigned)(SDL_GetTicks() - open_t0), err, ebuf);
      }
#endif
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

#ifdef _WIN32
    if (!logged_open_ok && a->base_dir[0]) {
      startup_tracef(a->base_dir, "tile_open_ok idx=%d", a->index);
      logged_open_ok = 1;
    }
#endif

    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0) {
#ifdef _WIN32
      if (a->base_dir[0]) {
        char ebuf[160] = {0};
        av_strerror(err, ebuf, sizeof(ebuf));
        startup_tracef(a->base_dir, "tile_find_stream_info_FAIL idx=%d err=%d (%s)", a->index, err,
                       ebuf);
      }
#endif
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    // Handshake done: from here on reads are bounded by rw_timeout and the
    // stall watchdogs, so the hard deadline comes off.
    a->open_deadline_ticks = 0;

    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
      if (fmt->streams[i] && fmt->streams[i]->codecpar &&
          fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        vstream = (int)i;
        break;
      }
    }
    if (vstream < 0) {
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    const AVCodec *dec = avcodec_find_decoder(fmt->streams[vstream]->codecpar->codec_id);
    if (!dec) {
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    err = avcodec_parameters_to_context(dec_ctx, fmt->streams[vstream]->codecpar);
    if (err < 0) {
      avcodec_free_context(&dec_ctx);
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    dec_ctx->thread_count = 1;
    dec_ctx->thread_type = 0;

    err = avcodec_open2(dec_ctx, dec, NULL);
    if (err < 0) {
      avcodec_free_context(&dec_ctx);
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    backoff_ms = 500;

    // Hand the recorder a fresh snapshot of the source video stream so it can
    // open files independently of this worker's context. Rolls a new file on
    // each (re)connect to keep per-file timestamps continuous.
    async_recorder_set_source(ar, fmt->streams[vstream]->codecpar, fmt->streams[vstream]->time_base);

    // Audio: this tile_stream is just a packet producer. When this tile is
    // selected as the audio source, announce our codec params (once) and
    // forward audio packets to the dedicated audio worker via audio_pipe.
    // The worker handles decode/resample/SDL queue, decoupled from video.
    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (astream < 0) astream = -1;
    int announced_for_token = 0;

    for (;;) {
      if (SDL_AtomicGet(a->quit)) break;
      err = av_read_frame(fmt, pkt);
      if (err < 0) break;

      // Audio packets: forward to the worker if we're the active source.
      if (astream >= 0 && pkt->stream_index == astream) {
        int is_active = (a->audio_active_idx &&
                         SDL_AtomicGet(a->audio_active_idx) == a->index);
        if (is_active && a->audio_pipe) {
          if (!announced_for_token) {
            audio_pipe_announce_source(a->audio_pipe, a->index,
                                       fmt->streams[astream]->codecpar);
            announced_for_token = 1;
          }
          audio_pipe_push(a->audio_pipe, a->index, pkt);
        } else {
          // Source switched away — make sure we re-announce on switch-back.
          announced_for_token = 0;
        }
        av_packet_unref(pkt);
        continue;
      }

      if (pkt->stream_index != vstream) {
        av_packet_unref(pkt);
        continue;
      }

      // Per-tile recording. Read record_enabled + title under rec_mutex (the F5
      // handler writes them from the UI thread), then hand the video packet to
      // this camera's dedicated recorder thread. The hand-off is a cheap clone +
      // enqueue; every blocking disk operation happens off this worker thread.
      {
        int want_rec = 0;
        char cam_title[256] = {0};
        if (a->rec_mutex) SDL_LockMutex(a->rec_mutex);
        if (a->cams && a->index >= 0 && a->index < a->cam_count) {
          want_rec = a->cams[a->index].record_enabled ? 1 : 0;
          if (a->cams[a->index].title_utf8) {
            snprintf(cam_title, sizeof(cam_title), "%s", a->cams[a->index].title_utf8);
          }
        }
        if (a->rec_mutex) SDL_UnlockMutex(a->rec_mutex);
        async_recorder_feed(ar, want_rec, cam_title, pkt);
      }

      err = avcodec_send_packet(dec_ctx, pkt);
      av_packet_unref(pkt);
      if (err < 0) continue;

      for (;;) {
        if (SDL_AtomicGet(a->quit)) break;
        err = avcodec_receive_frame(dec_ctx, frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) break;

        Uint32 now = SDL_GetTicks();
        int hires = (a->hires_index && SDL_AtomicGet(a->hires_index) == a->index) ? 1 : 0;
        int do_main = 0;
        int do_thumb = 0;
        if (a->min_update_ms > 0) {
          do_thumb = (!last_thumb_update || (Uint32)(now - last_thumb_update) >= (Uint32)a->min_update_ms) ? 1 : 0;
        } else {
          do_thumb = 1;
        }
        if (hires) {
          const int main_min_ms = 33;
          do_main = (!last_main_update || (Uint32)(now - last_main_update) >= (Uint32)main_min_ms) ? 1 : 0;
        }
        // Plugin feed: ~5 fps of (near) full-res frames while any plugin is enabled.
        // Independent of do_main, which only runs for the on-screen camera.
        int do_feed = 0;
        if (camera_plugin_feed_wanted()) {
          const int feed_min_ms = 200;
          do_feed = (!last_feed_update || (Uint32)(now - last_feed_update) >= (Uint32)feed_min_ms) ? 1 : 0;
        }
        if (!do_main && !do_thumb && !do_feed) {
          av_frame_unref(frame);
          continue;
        }

        int src_w = frame->width & ~1;
        int src_h = frame->height & ~1;
        if (src_w < 2 || src_h < 2) {
          av_frame_unref(frame);
          continue;
        }

        if (do_thumb) {
#ifdef _WIN32
          int tw = 0, th = 0;
          camera_thumb_dims(src_w, src_h, &tw, &th);
#else
          int tw = src_w / 4 & ~1;
          int th = src_h / 4 & ~1;
          if (tw < 2) tw = src_w;
          if (th < 2) th = src_h;
#endif
          if (tw > 0 && th > 0) {
            if (!sws_thumb || sws_thumb_src_w != src_w || sws_thumb_src_h != src_h ||
                sws_thumb_src_fmt != (enum AVPixelFormat)frame->format || sws_thumb_dst_w != tw ||
                sws_thumb_dst_h != th) {
              if (sws_thumb) sws_freeContext(sws_thumb);
              sws_thumb = sws_getContext(src_w, src_h, (enum AVPixelFormat)frame->format, tw, th,
                                         AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
              sws_thumb_src_w = src_w;
              sws_thumb_src_h = src_h;
              sws_thumb_src_fmt = (enum AVPixelFormat)frame->format;
              sws_thumb_dst_w = tw;
              sws_thumb_dst_h = th;
            }
            if (sws_thumb) {
              int need = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, tw, th, 1);
              if (need > 0) {
                if (!yuv_thumb_buf || yuv_thumb_buf_size != need) {
                  SDL_free(yuv_thumb_buf);
                  yuv_thumb_buf = (uint8_t *)SDL_malloc((size_t)need);
                  if (!yuv_thumb_buf) {
                    yuv_thumb_buf_size = 0;
                  } else {
                    yuv_thumb_buf_size = need;
                  }
                }
                if (yuv_thumb_buf && yuv_thumb_buf_size == need) {
                  yuv->format = AV_PIX_FMT_YUV420P;
                  yuv->width = tw;
                  yuv->height = th;
                  int rc = av_image_fill_arrays(yuv->data, yuv->linesize, yuv_thumb_buf, AV_PIX_FMT_YUV420P, tw, th,
                                                1);
                  if (rc >= 0) {
                    sws_scale(sws_thumb, (const uint8_t *const *)frame->data, frame->linesize, 0, src_h, yuv->data,
                              yuv->linesize);
                    if (a->cams && a->index >= 0 && a->index < a->cam_count) {
                      SDL_mutex *lk = a->cams[a->index].lock;
                      if (lk) SDL_LockMutex(lk);
                      camera_thumb_set_yuv420p(&a->cams[a->index], yuv_thumb_buf, need, tw, th, now);
                      if (lk) SDL_UnlockMutex(lk);
                    }
                    last_thumb_update = now;

#ifdef _WIN32
                    if (!logged_first_thumb && a->base_dir[0]) {
                      startup_tracef(a->base_dir, "tile_first_thumb idx=%d w=%d h=%d", a->index, tw, th);
                      logged_first_thumb = 1;
                    }
#endif
                  }
                }
              }
            }
          }
        }

        if (do_main) {
          int mw = src_w;
          int mh = src_h;
          if (!sws_main || sws_main_src_w != src_w || sws_main_src_h != src_h ||
              sws_main_src_fmt != (enum AVPixelFormat)frame->format) {
            if (sws_main) sws_freeContext(sws_main);
            sws_main = sws_getContext(src_w, src_h, (enum AVPixelFormat)frame->format, mw, mh, AV_PIX_FMT_YUV420P,
                                      SWS_BILINEAR, NULL, NULL, NULL);
            sws_main_src_w = src_w;
            sws_main_src_h = src_h;
            sws_main_src_fmt = (enum AVPixelFormat)frame->format;
          }
          if (sws_main) {
            int need = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, mw, mh, 1);
            if (need > 0) {
              if (!yuv_main_buf || yuv_main_buf_size != need) {
                SDL_free(yuv_main_buf);
  SDL_free(yuv_feed_buf);
  if (sws_feed) sws_freeContext(sws_feed);
                yuv_main_buf = (uint8_t *)SDL_malloc((size_t)need);
                if (!yuv_main_buf) {
                  yuv_main_buf_size = 0;
                } else {
                  yuv_main_buf_size = need;
                }
              }
              if (yuv_main_buf && yuv_main_buf_size == need) {
                yuv->format = AV_PIX_FMT_YUV420P;
                yuv->width = mw;
                yuv->height = mh;
                int rc = av_image_fill_arrays(yuv->data, yuv->linesize, yuv_main_buf, AV_PIX_FMT_YUV420P, mw, mh, 1);
                if (rc >= 0) {
                  sws_scale(sws_main, (const uint8_t *const *)frame->data, frame->linesize, 0, src_h, yuv->data,
                            yuv->linesize);
                  if (a->cams && a->index >= 0 && a->index < a->cam_count) {
                    SDL_mutex *lk = a->cams[a->index].lock;
                    if (lk) SDL_LockMutex(lk);
                    camera_main_set_yuv420p(&a->cams[a->index], yuv_main_buf, need, mw, mh, now);
                    if (lk) SDL_UnlockMutex(lk);
                  }
                  last_main_update = now;

#ifdef _WIN32
                  if (!logged_first_main && a->base_dir[0]) {
                    startup_tracef(a->base_dir, "tile_first_main idx=%d w=%d h=%d", a->index, mw, mh);
                    logged_first_main = 1;
                  }
#endif
                }
              }
            }
          }
        }

        if (do_feed) {
          int fw = src_w, fh = src_h;
          if (fw > CAMERA_PLUGIN_FEED_MAX_W) {
            fw = CAMERA_PLUGIN_FEED_MAX_W;
            fh = (int)((int64_t)src_h * fw / src_w) & ~1;
            if (fh < 2) fh = 2;
          }
          if (!sws_feed || sws_feed_src_w != src_w || sws_feed_src_h != src_h ||
              sws_feed_src_fmt != (enum AVPixelFormat)frame->format || sws_feed_dst_w != fw || sws_feed_dst_h != fh) {
            if (sws_feed) sws_freeContext(sws_feed);
            sws_feed = sws_getContext(src_w, src_h, (enum AVPixelFormat)frame->format, fw, fh, AV_PIX_FMT_YUV420P,
                                      SWS_BILINEAR, NULL, NULL, NULL);
            sws_feed_src_w = src_w;
            sws_feed_src_h = src_h;
            sws_feed_src_fmt = (enum AVPixelFormat)frame->format;
            sws_feed_dst_w = fw;
            sws_feed_dst_h = fh;
          }
          if (sws_feed) {
            int need = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, fw, fh, 1);
            if (need > 0) {
              if (!yuv_feed_buf || yuv_feed_buf_size != need) {
                SDL_free(yuv_feed_buf);
                yuv_feed_buf = (uint8_t *)SDL_malloc((size_t)need);
                yuv_feed_buf_size = yuv_feed_buf ? need : 0;
              }
              if (yuv_feed_buf && yuv_feed_buf_size == need) {
                yuv->format = AV_PIX_FMT_YUV420P;
                yuv->width = fw;
                yuv->height = fh;
                if (av_image_fill_arrays(yuv->data, yuv->linesize, yuv_feed_buf, AV_PIX_FMT_YUV420P, fw, fh, 1) >= 0) {
                  sws_scale(sws_feed, (const uint8_t *const *)frame->data, frame->linesize, 0, src_h, yuv->data,
                            yuv->linesize);
                  if (a->cams && a->index >= 0 && a->index < a->cam_count) {
                    SDL_mutex *lk = a->cams[a->index].lock;
                    if (lk) SDL_LockMutex(lk);
                    camera_feed_set_yuv420p(&a->cams[a->index], yuv_feed_buf, need, fw, fh, now);
                    if (lk) SDL_UnlockMutex(lk);
                  }
                  last_feed_update = now;
                }
              }
            }
          }
        }

        av_frame_unref(frame);
      }
    }

    // This RTSP session is ending; tell the recorder so it closes its current
    // file (it keeps its own param snapshot and never touched this fmt).
    async_recorder_invalidate_source(ar);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt);

    if (!SDL_AtomicGet(a->quit)) {
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
    }
  }

  if (ar) async_recorder_free(ar);
  SDL_free(yuv_thumb_buf);
  SDL_free(yuv_main_buf);
  if (sws_thumb) sws_freeContext(sws_thumb);
  if (sws_main) sws_freeContext(sws_main);
  av_packet_free(&pkt);
  av_frame_free(&yuv);
  av_frame_free(&frame);
  return 0;
}

void tile_streams_stop(SDL_Thread **ths, TileStreamArgs *args, int count, SDL_atomic_t *quit) {
  if (!quit) return;
  if (ths && count > 0) SDL_AtomicSet(quit, 1);
  for (int i = 0; ths && i < count; i++) {
    if (ths[i]) SDL_WaitThread(ths[i], NULL);
  }
  if (ths) SDL_free(ths);
  if (args) SDL_free(args);
  SDL_AtomicSet(quit, 0);
}

int tile_streams_prepare(SDL_Thread ***out_ths, TileStreamArgs **out_args, SDL_atomic_t *quit,
                         CameraConfig *cams, int cam_count,
                         int use_preview_url, SDL_atomic_t *hires_index, const char *base_dir,
                         const char *recordings_base_dir, SDL_mutex *rec_mutex,
                         AudioPipe *audio_pipe, SDL_atomic_t *audio_active_idx) {
  if (!out_ths || !out_args || !quit) return -1;
  *out_ths = NULL;
  *out_args = NULL;
  if (!cams || cam_count <= 0) return 0;

  // All workers have been joined by now; reclaim any gate slot lost to a
  // worker that was killed mid-open.
  open_gate_reset();

  SDL_Thread **ths = (SDL_Thread **)SDL_calloc((size_t)cam_count, sizeof(SDL_Thread *));
  TileStreamArgs *args = (TileStreamArgs *)SDL_calloc((size_t)cam_count, sizeof(TileStreamArgs));
  if (!ths || !args) {
    SDL_free(ths);
    SDL_free(args);
    return -1;
  }

  SDL_AtomicSet(quit, 0);
  for (int i = 0; i < cam_count; i++) {
    args[i].index = i;
    args[i].type = cams[i].type;
    args[i].base_dir[0] = 0;
    if (base_dir && *base_dir) snprintf(args[i].base_dir, sizeof(args[i].base_dir), "%s", base_dir);
    args[i].cams = cams;
    args[i].cam_count = cam_count;
    args[i].quit = quit;
    args[i].hires_index = hires_index;
    args[i].min_update_ms = 100;
    SDL_AtomicSet(&args[i].alive, 0);
    args[i].spawn_ticks = 0;
    args[i].next_spawn_ticks = 0;
    args[i].backoff_ms = 500;
    args[i].restart_count = 0;
    args[i].recordings_base_dir[0] = 0;
    if (recordings_base_dir && *recordings_base_dir) {
      snprintf(args[i].recordings_base_dir, sizeof(args[i].recordings_base_dir), "%s", recordings_base_dir);
    }
    args[i].rec_mutex = rec_mutex;
    args[i].audio_pipe = audio_pipe;
    args[i].audio_active_idx = audio_active_idx;

    const char *src = NULL;
    if (use_preview_url) {
      src = (cams[i].preview_url && *cams[i].preview_url) ? cams[i].preview_url : cams[i].url;
    } else {
      src = (cams[i].url && *cams[i].url) ? cams[i].url : cams[i].preview_url;
    }
    // A disabled camera is left with an empty URL, which the spawn step treats
    // as "nothing to do" — no worker, no connection, no restart backoff.
    if (!cams[i].enabled) src = NULL;
    args[i].url[0] = 0;
    if (src && *src) {
      build_camera_url(cams[i].type, src, args[i].url, sizeof(args[i].url));
    }
    if (!args[i].url[0]) {
      ths[i] = NULL;
      continue;
    }

    ths[i] = NULL;
  }

  *out_ths = ths;
  *out_args = args;
  return 0;
}

void tile_streams_spawn_step(SDL_Thread **ths, TileStreamArgs *args, int count, int *io_next,
                             int max_spawn) {
  if (!ths || !args || count <= 0 || !io_next || max_spawn <= 0) return;

  Uint32 now = SDL_GetTicks();

  // Pass 1: reap dead threads (clean exit or caught crash). Non-blocking because the
  // thread already finished by the time alive==0.
  for (int i = 0; i < count; i++) {
    if (!ths[i]) continue;
    if (SDL_AtomicGet(&args[i].alive)) continue;

    SDL_WaitThread(ths[i], NULL);
    ths[i] = NULL;

    if (args[i].quit && SDL_AtomicGet(args[i].quit)) continue;

    args[i].restart_count++;
    Uint32 lifetime = (args[i].spawn_ticks > 0) ? (Uint32)(now - args[i].spawn_ticks) : 0u;
    if (lifetime >= 60000u) {
      args[i].backoff_ms = 500;
    } else {
      args[i].backoff_ms = (args[i].backoff_ms < 500) ? 500 : args[i].backoff_ms * 2;
      if (args[i].backoff_ms > 30000) args[i].backoff_ms = 30000;
    }
    args[i].next_spawn_ticks = now + (Uint32)args[i].backoff_ms;

#ifdef _WIN32
    if (args[i].base_dir[0]) {
      startup_tracef(args[i].base_dir, "tile_thread_died idx=%d lifetime_ms=%u backoff_ms=%d restarts=%d",
                     i, (unsigned)lifetime, args[i].backoff_ms, args[i].restart_count);
    }
#endif
  }

  // Pass 2: spawn / respawn.
  int spawned = 0;
  for (int step = 0; step < count && spawned < max_spawn; step++) {
    int i = *io_next;
    if (i >= count) i = 0;
    *io_next = i + 1;

    if (ths[i]) continue;
    if (!args[i].url[0]) continue;
    if (args[i].next_spawn_ticks != 0 && (Sint32)(now - args[i].next_spawn_ticks) < 0) continue;
    if (args[i].quit && SDL_AtomicGet(args[i].quit)) continue;

    char name[64];
    snprintf(name, sizeof(name), "tilecam-%d", i);
#ifdef _WIN32
    if (args[i].base_dir[0]) startup_tracef(args[i].base_dir, "tile_spawn idx=%d", i);
#endif
    args[i].spawn_ticks = now;
    ths[i] = seh_create_thread(tile_stream_thread_fn, name, &args[i], &args[i].alive);
    if (!ths[i]) {
      args[i].next_spawn_ticks = now + 1000;
      continue;
    }
    spawned++;
  }
}
