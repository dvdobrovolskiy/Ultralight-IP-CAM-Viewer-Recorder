#pragma once
// System-tray behaviour (Windows; no-ops elsewhere), modelled on Hypermnesia's client:
//   - the window's X button and the minimize button hide the window to the tray
//   - left-click on the tray icon restores the window
//   - right-click opens a menu: Show / Exit; Exit is the only way to quit
//   - the icon is re-added when Explorer restarts (TaskbarCreated)
//   - Windows shutdown / logoff still terminates the app normally
#include <SDL.h>

// Call once after SDL_CreateWindow (subclasses the native window, adds the icon).
void tray_init(SDL_Window *win);
// Call before SDL_DestroyWindow.
void tray_shutdown(void);
