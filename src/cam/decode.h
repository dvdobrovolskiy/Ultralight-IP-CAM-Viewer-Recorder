#pragma once

#include <stdint.h>

#include <SDL.h>

#include "audio/audio.h"
#include "audio/audio_pipe.h"
#include "plugin_host.h"

typedef struct VideoShared {
  SDL_mutex *mutex;
  SDL_cond *cond;

  uint8_t *yuv;
  int yuv_size;
  int w;
  int h;

  int has_frame;
  int decode_err; // 0 ok, <0 AVERROR on fatal
  int keyint_ms_est; // estimated keyframe interval (ms), best-effort
  SDL_atomic_t quit;

  // Phase 2: set to 1 by the SEH thread wrapper while the decode thread is running;
  // cleared on normal return or on caught crash. The main loop polls this so a
  // silent crash triggers the same retry path as an explicit decode_err.
  SDL_atomic_t alive;

  // Hard open/handshake deadline (absolute SDL_GetTicks(); 0 = none) enforced
  // by the FFmpeg interrupt callback — see TileStreamArgs.open_deadline_ticks
  // for the rationale. Written only by the decode thread.
  Uint32 open_deadline_ticks;
} VideoShared;

typedef struct DecodeArgs {
  const char *url;
  VideoShared *shared;
  AudioState *audio;
  int audio_enabled;
  PluginHost *plugins;
  int camera_index;
  const char *camera_title_utf8;
  const char *recordings_base_dir;
  SDL_mutex *rec_mutex;
  int *rec_want;
  int rec_segment_seconds;
  int initial_keyint_ms;
  // Audio: like tile_stream.c, decode.c is just a packet producer. When this
  // camera is the active audio source (audio_active_idx == -1, meaning no
  // tile_stream is owning audio), it announces codec params to the pipe and
  // clones audio packets there. The dedicated worker thread does the decode/
  // resample/queue. NULL audio_pipe means no audio handoff.
  AudioPipe *audio_pipe;
  SDL_atomic_t *audio_active_idx;  // -1 = decode.c is the audio source
} DecodeArgs;

int ffmpeg_interrupt_cb(void *opaque);
void shared_set_error(VideoShared *shared, int err);

int decode_thread_fn(void *userdata);

void clear_shared(VideoShared *shared);
void stop_decode_thread(VideoShared *shared, SDL_Thread **th);

void request_stop_decode_thread_async(VideoShared *shared, SDL_Thread **th,
                                      SDL_Thread **join_th, SDL_atomic_t *join_done,
                                      int *join_rc);
