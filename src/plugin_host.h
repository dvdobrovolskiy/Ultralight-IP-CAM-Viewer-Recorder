#pragma once

#include <SDL.h>

#include "plugin_api.h"

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct PluginHost PluginHost;

typedef struct PluginRow {
  char filename_utf8[260];
  char name_utf8[128];
  char version_utf8[64];
  int enabled;
  int is_valid;
  int has_audio;
  int last_err;

#ifdef _WIN32
  HMODULE lib;
#endif
  const RtspPluginApiV1 *api;
  void *state;

  RtspOverlayRect overlays[64];
  int overlay_count;
} PluginRow;

struct PluginHost {
  char plugins_dir[1024];
  char settings_ini[1024];

  SDL_mutex *mutex;

  PluginRow *rows;
  int row_count;
};

int plugin_host_init(PluginHost *host, const char *settings_ini_path);
void plugin_host_shutdown(PluginHost *host);

// Scan plugins_dir and load any DLLs not yet loaded. Also prunes removed entries.
int plugin_host_rescan(PluginHost *host);

// Enable/disable a plugin (persisted to INI). Does not unload DLL.
int plugin_host_set_enabled(PluginHost *host, int index, int enabled);

// Video hook (called on main thread). Populates overlays for enabled plugins.
void plugin_host_on_video_frame(PluginHost *host, const RtspVideoFrameYuv420p *frame, int zoomed_in);

// Audio hook (called from decode thread). Best-effort, no allocations.
void plugin_host_on_audio(PluginHost *host, const RtspAudioFrameS16 *audio);

// Draw overlays for current frame.
void plugin_host_draw_overlays(SDL_Renderer *renderer, PluginHost *host, int win_w, int win_h,
                               int tex_w, int tex_h, double zoom, double view_x, double view_y);

// 1 if any plugin is enabled and successfully loaded (cheap gate for frame feeding).
int plugin_host_any_enabled(PluginHost *host);

// 1 if an enabled, successfully loaded plugin has `name_substr` in its name (e.g. "Face").
int plugin_host_is_enabled_named(PluginHost *host, const char *name_substr);

// Number of overlay rects the enabled plugin(s) matching `name_substr` reported for the
// current frame (0 if none / not enabled). For "Face" this is the number of tracked faces.
int plugin_host_overlay_count_named(PluginHost *host, const char *name_substr);
