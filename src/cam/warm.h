#pragma once

#include <SDL.h>

#include "app/camera.h"

typedef struct WarmCamArgs {
  int index;
  char url[2048];
  CameraConfig *cams;
  int cam_count;
  SDL_atomic_t *quit;

  // Phase 2: per-thread liveness + restart bookkeeping. Set to 1 by the SEH thread
  // wrapper at entry and cleared on normal return or on caught crash via TLS hook.
  SDL_atomic_t alive;
  Uint32 spawn_ticks;
  Uint32 next_spawn_ticks;
  int backoff_ms;
  int restart_count;

  // Hard open/handshake deadline (absolute SDL_GetTicks(); 0 = none) enforced
  // by the FFmpeg interrupt callback — see the identical field in
  // TileStreamArgs for the full rationale.
  Uint32 open_deadline_ticks;
} WarmCamArgs;

int warm_cam_thread_fn(void *userdata);

void warm_cams_stop(SDL_Thread **ths, WarmCamArgs *args, int count, SDL_atomic_t *quit);
int warm_cams_prepare(SDL_Thread ***out_ths, WarmCamArgs **out_args, SDL_atomic_t *quit,
                      CameraConfig *cams, int cam_count);

// Phase 2: combined spawner+supervisor. Call every UI tick. Spawns up to `max_spawn`
// new threads, joins any that have died (clean or crash), and respawns dead per-camera
// threads after exponential backoff so a faulty stream can't pin a slot. `*io_next` is
// the round-robin spawn cursor used during initial start-up.
void warm_cams_spawn_step(SDL_Thread **ths, WarmCamArgs *args, int count, int *io_next,
                          int max_spawn);
