#include "audio/audio.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"

int audio_state_snapshot(AudioState *st, SDL_AudioDeviceID *out_dev, SDL_AudioSpec *out_have) {
  if (!st) return 0;
  if (!SDL_AtomicGet(&st->ready)) return 0;
  if (st->mutex) SDL_LockMutex(st->mutex);
  if (out_dev) *out_dev = st->dev;
  if (out_have) *out_have = st->have;
  if (st->mutex) SDL_UnlockMutex(st->mutex);
  return 1;
}

void audio_state_clear_queue(AudioState *st) {
  SDL_AudioDeviceID dev = 0;
  if (!audio_state_snapshot(st, &dev, NULL) || dev == 0) return;
  SDL_ClearQueuedAudio(dev);
}

int audio_init_thread_fn(void *userdata) {
  AudioInitArgs *a = (AudioInitArgs *)userdata;
  if (!a || !a->st) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  const char *base_use = (a->base_dir[0] ? a->base_dir : ".\\");
#ifdef _WIN32
  startup_tracef(base_use, "audio_open_begin");
#else
  (void)base_use;
#endif

  SDL_AudioSpec want = {0}, have = {0};
  want.freq = 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  // 1024 frames @ 48 kHz stereo ≈ 21 ms per callback. Short enough for low
  // latency, long enough to amortize the lock+memcpy per callback.
  want.samples = 1024;
  want.callback = a->callback;
  want.userdata = a->callback_userdata;

  Uint32 t0 = SDL_GetTicks();
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  Uint32 dt = (Uint32)(SDL_GetTicks() - t0);

  if (SDL_AtomicGet(&a->st->quit)) {
    if (dev != 0) SDL_CloseAudioDevice(dev);
#ifdef _WIN32
    startup_tracef(base_use, "audio_open_aborted dt_ms=%u", (unsigned)dt);
#endif
    SDL_free(a);
    return 0;
  }

  if (dev != 0) {
    SDL_PauseAudioDevice(dev, 0);
    if (a->st->mutex) SDL_LockMutex(a->st->mutex);
    a->st->dev = dev;
    a->st->have = have;
    a->st->last_err[0] = 0;
    if (a->st->mutex) SDL_UnlockMutex(a->st->mutex);
    SDL_AtomicSet(&a->st->ready, 1);
#ifdef _WIN32
    startup_tracef(base_use, "audio_open_ok dt_ms=%u freq=%d ch=%d samples=%u", (unsigned)dt,
                   have.freq, have.channels, (unsigned)have.samples);
#endif
  } else {
    const char *err = SDL_GetError();
    if (!err) err = "";
    if (a->st->mutex) SDL_LockMutex(a->st->mutex);
    snprintf(a->st->last_err, sizeof(a->st->last_err), "%s", err);
    a->st->dev = 0;
    memset(&a->st->have, 0, sizeof(a->st->have));
    if (a->st->mutex) SDL_UnlockMutex(a->st->mutex);
#ifdef _WIN32
    startup_tracef(base_use, "audio_open_fail dt_ms=%u err=%s", (unsigned)dt, err);
#endif
  }

  SDL_free(a);
  (void)dt;
  return 0;
}
