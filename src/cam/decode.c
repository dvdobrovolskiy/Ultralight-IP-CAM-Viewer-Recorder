#include "cam/decode.h"

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#include "audio/audio_pipe.h"
#include "cam/async_recorder.h"
#include "core/log.h"
#include "core/seh.h"
#include "core/util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct AsyncJoinArgs {
  SDL_Thread *target;
  SDL_atomic_t *done;
  int *out_rc;
} AsyncJoinArgs;

void shared_set_error(VideoShared *shared, int err) {
  SDL_LockMutex(shared->mutex);
  shared->decode_err = err;
  SDL_CondBroadcast(shared->cond);
  SDL_UnlockMutex(shared->mutex);
}

#define DECODE_OPEN_DEADLINE_MS 15000

int ffmpeg_interrupt_cb(void *opaque) {
  VideoShared *shared = (VideoShared *)opaque;
  if (SDL_AtomicGet(&shared->quit)) return 1;
  if (shared->open_deadline_ticks != 0 &&
      (Sint32)(SDL_GetTicks() - shared->open_deadline_ticks) > 0) {
    return 1;
  }
  return 0;
}

int decode_thread_fn(void *userdata) {
  DecodeArgs *args = (DecodeArgs *)userdata;
  VideoShared *shared = args->shared;

  AVFormatContext *fmt = NULL;
  AVCodecContext *dec = NULL;
  struct SwsContext *sws = NULL;
  AVFrame *frame = NULL;
  AVFrame *frame_yuv = NULL;
  AVPacket *pkt = NULL;
  uint8_t *yuv_buf = NULL;
  int yuv_buf_size = 0;
  int cur_w = 0, cur_h = 0;
  enum AVPixelFormat cur_src_fmt = AV_PIX_FMT_NONE;
  int video_stream = -1;
  int audio_stream = -1;
  int64_t last_kf_ms = 0;

  // Recording runs on a dedicated thread + bounded queue (async_recorder.h):
  // this worker only clones+enqueues video packets, never blocks on disk.
  AsyncRecorder *ar = NULL;

  int err = 0;

  AVDictionary *open_opts = NULL;
  av_dict_set(&open_opts, "rtsp_transport", "tcp", 0);
  av_dict_set(&open_opts, "stimeout", "3000000", 0);
  av_dict_set(&open_opts, "rw_timeout", "3000000", 0);
  av_dict_set(&open_opts, "timeout", "3000000", 0);
  av_dict_set(&open_opts, "fflags", "nobuffer", 0);
  av_dict_set(&open_opts, "flags", "low_delay", 0);

  fmt = avformat_alloc_context();
  if (!fmt) {
    err = AVERROR(ENOMEM);
    av_dict_free(&open_opts);
    goto done;
  }
  fmt->interrupt_callback.callback = ffmpeg_interrupt_cb;
  fmt->interrupt_callback.opaque = shared;

  shared->open_deadline_ticks = SDL_GetTicks() + DECODE_OPEN_DEADLINE_MS;
  err = avformat_open_input(&fmt, args->url, NULL, &open_opts);
  av_dict_free(&open_opts);
  if (err < 0) goto done;

  err = avformat_find_stream_info(fmt, NULL);
  if (err < 0) goto done;
  shared->open_deadline_ticks = 0;  // handshake done; reads are watchdogged

  video_stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (video_stream < 0) {
    err = video_stream;
    goto done;
  }

  audio_stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (audio_stream < 0 || !args->audio_enabled) {
    audio_stream = -1;
  }

  AVStream *st = fmt->streams[video_stream];
  const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) {
    err = AVERROR_DECODER_NOT_FOUND;
    goto done;
  }

  dec = avcodec_alloc_context3(codec);
  if (!dec) {
    err = AVERROR(ENOMEM);
    goto done;
  }
  err = avcodec_parameters_to_context(dec, st->codecpar);
  if (err < 0) goto done;

  AVDictionary *dec_opts = NULL;
  av_dict_set(&dec_opts, "threads", "0", 0);
  err = avcodec_open2(dec, codec, &dec_opts);
  av_dict_free(&dec_opts);
  if (err < 0) goto done;

  frame = av_frame_alloc();
  frame_yuv = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!frame || !frame_yuv || !pkt) {
    err = AVERROR(ENOMEM);
    goto done;
  }

  // Start this camera's recorder and snapshot the source video stream for it.
  ar = async_recorder_create(args ? args->camera_index : 0,
                             (args && args->recordings_base_dir[0]) ? args->recordings_base_dir
                                                                    : NULL);
  async_recorder_set_source(ar, st->codecpar, st->time_base);

  int audio_announced = 0;
  while (!SDL_AtomicGet(&shared->quit)) {
    // decode.c is the audio source only when no tile_stream owns it (matrix
    // grid sets audio_active_idx to the selected tile; here we want the opposite).
    int decode_owns_audio = (audio_stream >= 0 && args && args->audio_enabled &&
                             args->audio_pipe &&
                             (!args->audio_active_idx ||
                              SDL_AtomicGet(args->audio_active_idx) == -1));
    if (decode_owns_audio && !audio_announced) {
      AVStream *ast = fmt->streams[audio_stream];
      audio_pipe_announce_source(args->audio_pipe, args->camera_index, ast->codecpar);
      audio_announced = 1;
#ifdef _WIN32
      startup_tracef(NULL, "decode_audio_announce cam=%d codec=%s sr=%d ch=%d",
                     args->camera_index, avcodec_get_name(ast->codecpar->codec_id),
                     ast->codecpar->sample_rate, ast->codecpar->ch_layout.nb_channels);
#endif
    } else if (!decode_owns_audio && audio_announced) {
      // Matrix grid took over; stop pushing so we don't race tile_stream.
      audio_announced = 0;
    }

    err = av_read_frame(fmt, pkt);
    if (err < 0) {
      if (err == AVERROR_EOF) {
        err = 0;
        break;
      }
      if (err == AVERROR(EAGAIN)) {
        SDL_Delay(1);
        continue;
      }
      goto done;
    }

    // Recording: read desired state + title under rec_mutex (the UI thread
    // writes them), then hand the video packet to this camera's recorder thread.
    // The hand-off is a cheap clone + enqueue; all disk I/O is off this thread.
    {
      int want_rec = 0;
      char cam_title[256] = {0};
      if (args && args->rec_mutex) SDL_LockMutex(args->rec_mutex);
      if (args && args->rec_want) want_rec = (*args->rec_want) ? 1 : 0;
      if (args && args->camera_title_utf8 && *args->camera_title_utf8) {
        snprintf(cam_title, sizeof(cam_title), "%s", args->camera_title_utf8);
      }
      if (args && args->rec_mutex) SDL_UnlockMutex(args->rec_mutex);
      if (pkt->stream_index == video_stream) {
        async_recorder_feed(ar, want_rec, cam_title, pkt);
      }
    }

    int is_video = (pkt->stream_index == video_stream);
    int is_audio = (audio_stream >= 0 && pkt->stream_index == audio_stream);
    if (!is_video && !is_audio) {
      av_packet_unref(pkt);
      continue;
    }
    // Hand audio packets off to the dedicated worker via the pipe — but only
    // when we currently own audio (no tile_stream is the source). Otherwise
    // we'd race the tile_stream producer for the same camera.
    if (is_audio) {
      if (decode_owns_audio && audio_announced) {
        audio_pipe_push(args->audio_pipe, args->camera_index, pkt);
      }
      av_packet_unref(pkt);
      continue;
    }

    if (is_video && (pkt->flags & AV_PKT_FLAG_KEY)) {
      int64_t now_ms = av_gettime_relative() / 1000;
      if (last_kf_ms) {
        int delta = (int)(now_ms - last_kf_ms);
        if (delta > 200 && delta < 60000) {
          SDL_LockMutex(shared->mutex);
          int prev = shared->keyint_ms_est;
          if (prev <= 0) prev = (args && args->initial_keyint_ms > 0) ? args->initial_keyint_ms : delta;
          shared->keyint_ms_est = (prev * 7 + delta) / 8;
          SDL_UnlockMutex(shared->mutex);
        }
      } else if (args && args->initial_keyint_ms > 0) {
        SDL_LockMutex(shared->mutex);
        if (shared->keyint_ms_est <= 0) shared->keyint_ms_est = args->initial_keyint_ms;
        SDL_UnlockMutex(shared->mutex);
      }
      last_kf_ms = now_ms;
    }

    err = avcodec_send_packet(dec, pkt);
    av_packet_unref(pkt);
    if (err < 0) {
      if (err == AVERROR(EAGAIN)) continue;
      goto done;
    }

    if (is_video) {
      while (!SDL_AtomicGet(&shared->quit) && (err = avcodec_receive_frame(dec, frame)) >= 0) {
        int w = frame->width;
        int h = frame->height;
        enum AVPixelFormat src_fmt = (enum AVPixelFormat)frame->format;
        if (w <= 0 || h <= 0) continue;

        if (w != cur_w || h != cur_h || src_fmt != cur_src_fmt) {
          if (sws) sws_freeContext(sws);
          sws = sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, NULL, NULL, NULL);
          if (!sws) {
            err = AVERROR(EINVAL);
            goto done;
          }

          int needed = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
          if (needed < 0) {
            err = needed;
            goto done;
          }
          if (needed != yuv_buf_size) {
            av_freep(&yuv_buf);
            yuv_buf = (uint8_t *)av_malloc((size_t)needed);
            if (!yuv_buf) {
              err = AVERROR(ENOMEM);
              goto done;
            }
            yuv_buf_size = needed;
          }

          frame_yuv->format = AV_PIX_FMT_YUV420P;
          frame_yuv->width = w;
          frame_yuv->height = h;
          err = av_image_fill_arrays(frame_yuv->data, frame_yuv->linesize, yuv_buf,
                                     AV_PIX_FMT_YUV420P, w, h, 1);
          if (err < 0) goto done;

          cur_w = w;
          cur_h = h;
          cur_src_fmt = src_fmt;

          SDL_LockMutex(shared->mutex);
          if (needed != shared->yuv_size) {
            uint8_t *new_buf = (uint8_t *)SDL_realloc(shared->yuv, (size_t)needed);
            if (!new_buf) {
              SDL_UnlockMutex(shared->mutex);
              err = AVERROR(ENOMEM);
              goto done;
            }
            shared->yuv = new_buf;
            shared->yuv_size = needed;
          }
          shared->w = w;
          shared->h = h;
          SDL_UnlockMutex(shared->mutex);
        }

        sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, h,
                  frame_yuv->data, frame_yuv->linesize);

        SDL_LockMutex(shared->mutex);
        if (shared->yuv && shared->yuv_size == yuv_buf_size) {
          memcpy(shared->yuv, yuv_buf, (size_t)yuv_buf_size);
          shared->has_frame = 1;
          SDL_CondSignal(shared->cond);
        }
        SDL_UnlockMutex(shared->mutex);
      }
    }

    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
      continue;
    }
    if (err < 0) goto done;
  }

done:
  if (err == AVERROR_EXIT && SDL_AtomicGet(&shared->quit)) {
    err = 0;
  }
  if (err < 0 && !SDL_AtomicGet(&shared->quit)) {
    shared_set_error(shared, err);
  }

  if (ar) async_recorder_free(ar);
  av_freep(&yuv_buf);
  av_packet_free(&pkt);
  av_frame_free(&frame_yuv);
  av_frame_free(&frame);
  if (sws) sws_freeContext(sws);
  avcodec_free_context(&dec);
  if (fmt) avformat_close_input(&fmt);

  return 0;
}

void clear_shared(VideoShared *shared) {
  SDL_LockMutex(shared->mutex);
  shared->has_frame = 0;
  shared->decode_err = 0;
  shared->w = 0;
  shared->h = 0;
  shared->keyint_ms_est = 0;
  SDL_UnlockMutex(shared->mutex);
}

void stop_decode_thread(VideoShared *shared, SDL_Thread **th) {
  if (!th || !*th) return;
  SDL_AtomicSet(&shared->quit, 1);
  SDL_WaitThread(*th, NULL);
  *th = NULL;
  SDL_AtomicSet(&shared->quit, 0);
  clear_shared(shared);
}

static int async_join_thread_fn(void *userdata) {
  AsyncJoinArgs *a = (AsyncJoinArgs *)userdata;
  if (!a) return 0;
  int rc = 0;
  if (a->target) SDL_WaitThread(a->target, &rc);
  if (a->out_rc) *a->out_rc = rc;
  if (a->done) SDL_AtomicSet(a->done, 1);
  SDL_free(a);
  return 0;
}

void request_stop_decode_thread_async(VideoShared *shared, SDL_Thread **th,
                                      SDL_Thread **join_th, SDL_atomic_t *join_done,
                                      int *join_rc) {
  if (!shared || !th || !*th) return;
  if (join_th && *join_th) return; // already joining

  AsyncJoinArgs *a = (AsyncJoinArgs *)SDL_malloc(sizeof(AsyncJoinArgs));
  if (!a) return;
  a->target = *th;
  a->done = join_done;
  a->out_rc = join_rc;
  if (join_done) SDL_AtomicSet(join_done, 0);

  SDL_AtomicSet(&shared->quit, 1);
  SDL_Thread *jt = seh_create_thread(async_join_thread_fn, "ffmpeg-join", a, NULL);
  if (!jt) {
    SDL_free(a);
    stop_decode_thread(shared, th);
    if (join_done) SDL_AtomicSet(join_done, 0);
    return;
  }
  if (join_th) *join_th = jt;
  *th = NULL;
}
