#include "core/watchdog.h"

#include <stdio.h>

#include "core/build_id.h"
#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int ui_hang_watchdog_thread_fn(void *userdata) {
  UiHangWatchdogArgs *a = (UiHangWatchdogArgs *)userdata;
  if (!a || !a->quit || !a->main_heartbeat) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  const char *base_use = (a->base_dir[0] ? a->base_dir : ".\\");
  char path[1024] = {0};
  snprintf(path, sizeof(path), "%scache\\hang_watchdog.log", base_use);

  {
    char line[256];
    snprintf(line, sizeof(line), "t=%lu watchdog_started build=%s", (unsigned long)GetTickCount(), BUILD_ID);
    append_log_line_utf8(path, line);
  }

  Uint32 last_report = 0;
  int was_stalled = 0;
  for (;;) {
    if (SDL_AtomicGet(a->quit)) break;
    SDL_Delay(200);

    Uint32 now = SDL_GetTicks();
    Uint32 hb = (Uint32)SDL_AtomicGet(a->main_heartbeat);
    Uint32 dt = (Uint32)(now - hb);

    int stalled = (dt > 800u) ? 1 : 0;
    if (stalled) {
      if (!was_stalled || (Uint32)(now - last_report) > 2000u) {
        char line[256];
        snprintf(line, sizeof(line), "t=%lu main_stall_ms=%lu", (unsigned long)GetTickCount(),
                 (unsigned long)dt);
        append_log_line_utf8(path, line);
        last_report = now;
      }
    } else {
      if (was_stalled) {
        char line[256];
        snprintf(line, sizeof(line), "t=%lu main_recovered", (unsigned long)GetTickCount());
        append_log_line_utf8(path, line);
      }
    }
    was_stalled = stalled;
  }
  return 0;
}
#endif
