#pragma once

#include <SDL.h>

typedef struct AudioState {
  SDL_mutex *mutex;
  SDL_atomic_t ready; // 0 not ready, 1 ready
  SDL_atomic_t quit;
  SDL_AudioDeviceID dev;
  SDL_AudioSpec have;
  char last_err[256];
} AudioState;

typedef struct AudioInitArgs {
  AudioState *st;
  char base_dir[1024];
  // SDL audio callback (pull model) and its userdata pointer. When set, the
  // device is opened with this callback instead of in queue mode; the caller
  // (audio_pipe) is responsible for filling samples on demand.
  SDL_AudioCallback callback;
  void *callback_userdata;
} AudioInitArgs;

int audio_state_snapshot(AudioState *st, SDL_AudioDeviceID *out_dev, SDL_AudioSpec *out_have);
void audio_state_clear_queue(AudioState *st);

int audio_init_thread_fn(void *userdata);
