#pragma once

#include <SDL.h>

#ifdef _WIN32
typedef struct UiHangWatchdogArgs {
  char base_dir[1024];
  SDL_atomic_t *quit;
  SDL_atomic_t *main_heartbeat; // SDL_GetTicks()
} UiHangWatchdogArgs;

int ui_hang_watchdog_thread_fn(void *userdata);
#endif
