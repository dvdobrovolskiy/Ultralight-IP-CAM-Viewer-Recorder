#include "cam/warm.h"

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "app/config.h"
#include "cam/cam_cache.h"
#include "core/seh.h"

#define WARM_OPEN_DEADLINE_MS 15000

// See the identical helper in tile_stream.c: sliced sleep so a pending stop
// from the UI thread never waits out a multi-second backoff.
static void sleep_quit_aware(SDL_atomic_t *quit, int ms) {
  while (ms > 0) {
    if (quit && SDL_AtomicGet(quit)) return;
    int step = (ms > 100) ? 100 : ms;
    SDL_Delay((Uint32)step);
    ms -= step;
  }
}

static int warm_ffmpeg_interrupt_cb(void *opaque) {
  WarmCamArgs *a = (WarmCamArgs *)opaque;
  if (!a) return 0;
  if (a->quit && SDL_AtomicGet(a->quit)) return 1;
  if (a->open_deadline_ticks != 0 &&
      (Sint32)(SDL_GetTicks() - a->open_deadline_ticks) > 0) {
    return 1;
  }
  return 0;
}

int warm_cam_thread_fn(void *userdata) {
  WarmCamArgs *a = (WarmCamArgs *)userdata;
  if (!a) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  int backoff_ms = 500;
  int last_keyframe_delta_ms = 0;
  for (;;) {
    if (SDL_AtomicGet(a->quit)) break;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      av_dict_free(&opts);
      continue;
    }
    fmt->interrupt_callback.callback = warm_ffmpeg_interrupt_cb;
    fmt->interrupt_callback.opaque = a;

    a->open_deadline_ticks = SDL_GetTicks() + WARM_OPEN_DEADLINE_MS;
    int err = avformat_open_input(&fmt, a->url, NULL, &opts);
    av_dict_free(&opts);
    if (err < 0 || !fmt) {
      if (fmt) avformat_free_context(fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0) {
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }
    a->open_deadline_ticks = 0;  // handshake done; reads are watchdogged

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
    avcodec_parameters_to_context(dec_ctx, fmt->streams[vstream]->codecpar);
    dec_ctx->thread_count = 1;
    if ((err = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
      avcodec_free_context(&dec_ctx);
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    AVFrame *frame = av_frame_alloc();
    AVFrame *yuv = av_frame_alloc();
    struct SwsContext *sws = NULL;
    int sws_w = 0;
    int sws_h = 0;
    enum AVPixelFormat sws_fmt = AV_PIX_FMT_NONE;
    uint8_t *yuv_buf = NULL;
    int yuv_buf_size = 0;

    Uint32 last_decode = 0;
    int64_t last_kf_ms = 0;
    backoff_ms = 500;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
      SDL_free(yuv_buf);
      if (sws) sws_freeContext(sws);
      av_frame_free(&yuv);
      av_frame_free(&frame);
      avcodec_free_context(&dec_ctx);
      avformat_close_input(&fmt);
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    while (!SDL_AtomicGet(a->quit)) {
      err = av_read_frame(fmt, pkt);
      if (err < 0) break;

      if (pkt->stream_index != vstream) {
        av_packet_unref(pkt);
        continue;
      }

      Uint32 now = SDL_GetTicks();
      if (!(pkt->flags & AV_PKT_FLAG_KEY) || (last_decode && (Uint32)(now - last_decode) < 1000)) {
        av_packet_unref(pkt);
        continue;
      }
      {
        int64_t now_ms = av_gettime_relative() / 1000;
        if (last_kf_ms) {
          int delta = (int)(now_ms - last_kf_ms);
          if (delta > 200 && delta < 60000) last_keyframe_delta_ms = delta;
        }
        last_kf_ms = now_ms;
      }
      last_decode = now;

      if ((err = avcodec_send_packet(dec_ctx, pkt)) < 0) {
        av_packet_unref(pkt);
        continue;
      }
      av_packet_unref(pkt);

      while (!SDL_AtomicGet(a->quit) && (err = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
        int w = frame->width;
        int h = frame->height;
        if (w <= 0 || h <= 0) {
          av_frame_unref(frame);
          continue;
        }

        int needed = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
        if (needed <= 0) {
          av_frame_unref(frame);
          continue;
        }
        if (needed != yuv_buf_size) {
          SDL_free(yuv_buf);
          yuv_buf = (uint8_t *)SDL_malloc((size_t)needed);
          if (!yuv_buf) {
            yuv_buf_size = 0;
            av_frame_unref(frame);
            break;
          }
          yuv_buf_size = needed;
        }

        yuv->format = AV_PIX_FMT_YUV420P;
        yuv->width = w;
        yuv->height = h;
        if ((err = av_image_fill_arrays(yuv->data, yuv->linesize, yuv_buf, AV_PIX_FMT_YUV420P, w, h, 1)) <
            0) {
          av_frame_unref(frame);
          continue;
        }

        if (!sws || sws_w != w || sws_h != h || sws_fmt != (enum AVPixelFormat)frame->format) {
          if (sws) sws_freeContext(sws);
          sws = sws_getContext(w, h, (enum AVPixelFormat)frame->format, w, h, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, NULL, NULL, NULL);
          sws_w = w;
          sws_h = h;
          sws_fmt = (enum AVPixelFormat)frame->format;
        }
        if (sws) {
          sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, h, yuv->data,
                    yuv->linesize);
        }

        if (a->cams && a->index >= 0 && a->index < a->cam_count) {
          SDL_mutex *lk = a->cams[a->index].lock;
          if (lk) SDL_LockMutex(lk);
          camera_cache_update(&a->cams[a->index], yuv_buf, yuv_buf_size, w, h);
          if (last_keyframe_delta_ms > 0) {
            int prev = a->cams[a->index].keyint_ms;
            if (prev <= 0) prev = last_keyframe_delta_ms;
            a->cams[a->index].keyint_ms = (prev * 7 + last_keyframe_delta_ms) / 8;
          }
          if (lk) SDL_UnlockMutex(lk);
        }

        av_frame_unref(frame);
        break;
      }

      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        continue;
      }
    }

    av_packet_free(&pkt);
    SDL_free(yuv_buf);
    if (sws) sws_freeContext(sws);
    av_frame_free(&yuv);
    av_frame_free(&frame);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt);

    if (!SDL_AtomicGet(a->quit)) {
      sleep_quit_aware(a->quit, backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
    }
  }
  return 0;
}

void warm_cams_stop(SDL_Thread **ths, WarmCamArgs *args, int count, SDL_atomic_t *quit) {
  if (!quit) return;
  if (ths && count > 0) SDL_AtomicSet(quit, 1);
  for (int i = 0; ths && i < count; i++) {
    if (ths[i]) SDL_WaitThread(ths[i], NULL);
  }
  if (ths) SDL_free(ths);
  if (args) SDL_free(args);
  SDL_AtomicSet(quit, 0);
}

int warm_cams_prepare(SDL_Thread ***out_ths, WarmCamArgs **out_args, SDL_atomic_t *quit,
                      CameraConfig *cams, int cam_count) {
  if (!out_ths || !out_args || !quit) return -1;
  *out_ths = NULL;
  *out_args = NULL;
  if (!cams || cam_count <= 0) return 0;

  SDL_Thread **ths = (SDL_Thread **)SDL_calloc((size_t)cam_count, sizeof(SDL_Thread *));
  WarmCamArgs *args = (WarmCamArgs *)SDL_calloc((size_t)cam_count, sizeof(WarmCamArgs));
  if (!ths || !args) {
    SDL_free(ths);
    SDL_free(args);
    return -1;
  }

  SDL_AtomicSet(quit, 0);
  for (int i = 0; i < cam_count; i++) {
    args[i].index = i;
    args[i].cams = cams;
    args[i].cam_count = cam_count;
    args[i].quit = quit;
    SDL_AtomicSet(&args[i].alive, 0);
    args[i].spawn_ticks = 0;
    args[i].next_spawn_ticks = 0;
    args[i].backoff_ms = 500;
    args[i].restart_count = 0;

    const char *src = (cams[i].preview_url && *cams[i].preview_url) ? cams[i].preview_url : cams[i].url;
    if (!cams[i].enabled) src = NULL;  // disabled: never connect (see tile_stream.c)
    args[i].url[0] = 0;
    if (src && *src) {
      build_camera_url(cams[i].type, src, args[i].url, sizeof(args[i].url));
    }

    ths[i] = NULL;
  }

  *out_ths = ths;
  *out_args = args;
  return 0;
}

void warm_cams_spawn_step(SDL_Thread **ths, WarmCamArgs *args, int count, int *io_next,
                          int max_spawn) {
  if (!ths || !args || count <= 0 || !io_next || max_spawn <= 0) return;

  Uint32 now = SDL_GetTicks();

  // Pass 1: reap any thread whose `alive` flag is 0. That happens on either a clean
  // exit (the SEH wrapper clears the flag on normal return) or a caught crash (the
  // vectored handler clears it via TLS before ExitThread). SDL_WaitThread on an
  // already-exited thread is non-blocking, so this never stalls the UI.
  for (int i = 0; i < count; i++) {
    if (!ths[i]) continue;
    if (SDL_AtomicGet(&args[i].alive)) continue;

    SDL_WaitThread(ths[i], NULL);
    ths[i] = NULL;

    if (args[i].quit && SDL_AtomicGet(args[i].quit)) {
      // App is shutting down — don't schedule a respawn.
      continue;
    }

    args[i].restart_count++;
    Uint32 lifetime = (args[i].spawn_ticks > 0) ? (Uint32)(now - args[i].spawn_ticks) : 0u;
    if (lifetime >= 60000u) {
      // Thread ran healthily for >=60s — reset backoff so a one-off failure doesn't
      // permanently penalize this slot.
      args[i].backoff_ms = 500;
    } else {
      args[i].backoff_ms = (args[i].backoff_ms < 500) ? 500 : args[i].backoff_ms * 2;
      if (args[i].backoff_ms > 30000) args[i].backoff_ms = 30000;
    }
    args[i].next_spawn_ticks = now + (Uint32)args[i].backoff_ms;
  }

  // Pass 2: spawn new / respawn dead. `*io_next` is the round-robin cursor for the
  // initial fan-out at startup so we don't open all N RTSP streams in the same frame.
  int spawned = 0;
  for (int step = 0; step < count && spawned < max_spawn; step++) {
    int i = *io_next;
    if (i >= count) i = 0;
    *io_next = i + 1;

    if (ths[i]) continue;
    if (args[i].cams && args[i].cams[i].type != CAMERA_TYPE_RTSP) continue;
    if (!args[i].url[0]) continue;
    if (args[i].next_spawn_ticks != 0 && (Sint32)(now - args[i].next_spawn_ticks) < 0) continue;
    if (args[i].quit && SDL_AtomicGet(args[i].quit)) continue;

    char name[64];
    snprintf(name, sizeof(name), "warmcam-%d", i);
    args[i].spawn_ticks = now;
    ths[i] = seh_create_thread(warm_cam_thread_fn, name, &args[i], &args[i].alive);
    if (!ths[i]) {
      // Couldn't create — try again next tick after a short delay.
      args[i].next_spawn_ticks = now + 1000;
      continue;
    }
    spawned++;
  }
}
