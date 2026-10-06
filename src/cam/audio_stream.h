#pragma once

#include <SDL.h>

#include "app/camera.h"
#include "audio/audio.h"
#include "plugin_host.h"

// Per-camera audio stream worker. One instance runs at a time, for whichever camera
// is currently "audio-selected" (the fullscreen / expanded one). The thread opens its
// own RTSP connection, decodes the audio stream only, resamples to the shared SDL
// audio device's format, and queues samples. Reconnects with exponential backoff.
//
// `playing` is set to 1 while audio samples are being actively queued, and cleared
// after a brief idle window or on shutdown — the UI renders the green speaker icon
// based on this flag.
typedef struct AudioStreamArgs {
  char url[2048];
  CameraType type;
  AudioState *audio;
  PluginHost *plugins;
  int cam_idx;            // informational; -1 when idle

  SDL_atomic_t quit;
  SDL_atomic_t alive;       // managed by the SEH wrapper
  SDL_atomic_t playing;     // 1 = sound is being queued right now
  // 0 = idle / not running
  // 1 = connecting (RTSP open in progress)
  // 2 = ready (audio stream confirmed; decoding)
  // 3 = no_audio (open succeeded but stream has no audio track)
  SDL_atomic_t state;
  SDL_atomic_t volume_pct;  // 0..200; 100 = unity. >100 boosts in software with clamp.
} AudioStreamArgs;

#define AUDIO_STREAM_IDLE       0
#define AUDIO_STREAM_CONNECTING 1
#define AUDIO_STREAM_READY      2
#define AUDIO_STREAM_NO_AUDIO   3

int audio_stream_thread_fn(void *userdata);
