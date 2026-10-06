#include "plugin_host.h"

#include <stdio.h>
#include <string.h>

#include "app/config.h"

#ifdef _WIN32
#include <windows.h>
#endif

static void host_log_utf8(int level, const char *msg_utf8) {
  const char *pfx = "INFO";
  if (level == 1) pfx = "WARN";
  if (level == 2) pfx = "ERR";
  fprintf(stderr, "[plugin][%s] %s\n", pfx, msg_utf8 ? msg_utf8 : "");
}

static RtspPluginHostApi make_host_api(void) {
  RtspPluginHostApi api;
  memset(&api, 0, sizeof(api));
  api.size = (uint32_t)sizeof(api);
  api.log.size = (uint32_t)sizeof(api.log);
  api.log.log_utf8 = host_log_utf8;
  return api;
}

static int str_ends_with_i(const char *s, const char *suffix) {
  if (!s || !suffix) return 0;
  size_t n = strlen(s), m = strlen(suffix);
  if (m > n) return 0;
  return SDL_strcasecmp(s + (n - m), suffix) == 0;
}

static void safe_copy(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  if (!src) src = "";
  snprintf(dst, cap, "%s", src);
}

static int get_base_dir(char *out, size_t out_cap) {
  if (!out || out_cap == 0) return -1;
  char *base = SDL_GetBasePath();
  if (base && *base) {
    snprintf(out, out_cap, "%s", base);
    SDL_free(base);
    return 0;
  }
  if (base) SDL_free(base);
  snprintf(out, out_cap, ".\\");
  return 0;
}

static int ensure_dir(const char *path) {
#ifdef _WIN32
  if (!path || !*path) return -1;
  WCHAR wpath[1024] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof(wpath) / sizeof(WCHAR))) <= 0) {
    return -1;
  }
  DWORD attr = GetFileAttributesW(wpath);
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
  if (CreateDirectoryW(wpath, NULL)) return 0;
  return 0; // best-effort
#else
  (void)path;
  return 0;
#endif
}

static int ini_get_int_enabled(const char *ini, const char *filename, int def) {
#ifdef _WIN32
  if (!ini || !*ini || !filename) return def;
  return ini_get_int(ini, "Plugins", filename, def);
#else
  (void)ini;
  (void)filename;
  return def;
#endif
}

static void ini_set_int_enabled(const char *ini, const char *filename, int v) {
#ifdef _WIN32
  if (!ini || !*ini || !filename) return;
  ini_set_int(ini, "Plugins", filename, v ? 1 : 0);
#else
  (void)ini;
  (void)filename;
  (void)v;
#endif
}

static int find_row_by_filename(PluginHost *host, const char *filename) {
  if (!host || !filename) return -1;
  for (int i = 0; i < host->row_count; i++) {
    if (SDL_strcasecmp(host->rows[i].filename_utf8, filename) == 0) return i;
  }
  return -1;
}

static void free_row(PluginRow *r) {
  if (!r) return;
#ifdef _WIN32
  if (r->api && r->state && r->api->shutdown) {
    r->api->shutdown(r->state);
  }
  r->state = NULL;
  r->api = NULL;
  if (r->lib) {
    FreeLibrary(r->lib);
    r->lib = NULL;
  }
#endif
  r->overlay_count = 0;
}

static int load_dll_plugin(PluginHost *host, PluginRow *row, const char *full_path) {
#ifdef _WIN32
  if (!host || !row || !full_path) return -1;

  WCHAR wpath[1024] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, full_path, -1, wpath,
                          (int)(sizeof(wpath) / sizeof(WCHAR))) <= 0) {
    return -1;
  }
  row->lib = LoadLibraryW(wpath);
  if (!row->lib) {
    row->last_err = (int)GetLastError();
    return -1;
  }

  typedef const RtspPluginApiV1 *(*GetApiFn)(void);
  GetApiFn get_api = (GetApiFn)GetProcAddress(row->lib, "rtsp_plugin_get_api");
  if (!get_api) {
    row->is_valid = 0;
    row->last_err = (int)GetLastError();
    return -1;
  }

  const RtspPluginApiV1 *api = get_api();
  if (!api || api->magic != RTSP_PLUGIN_MAGIC || api->abi_version != RTSP_PLUGIN_ABI_VERSION ||
      api->size < sizeof(RtspPluginApiV1) || !api->get_name_utf8 || !api->init || !api->shutdown ||
      !api->on_video_frame_yuv420p || !api->get_overlays) {
    row->is_valid = 0;
    return -1;
  }

  RtspPluginHostApi host_api = make_host_api();
  void *state = NULL;
  int rc = api->init(&host_api, &state);
  if (rc != 0 || !state) {
    row->is_valid = 0;
    return -1;
  }

  row->api = api;
  row->state = state;
  row->is_valid = 1;
  row->has_audio = (api->on_audio_s16 != NULL) ? 1 : 0;

  const char *nm = api->get_name_utf8();
  const char *ver = api->get_version_utf8 ? api->get_version_utf8() : NULL;
  safe_copy(row->name_utf8, sizeof(row->name_utf8), nm ? nm : "");
  safe_copy(row->version_utf8, sizeof(row->version_utf8), ver ? ver : "");
  return 0;
#else
  (void)host;
  (void)row;
  (void)full_path;
  return -1;
#endif
}

int plugin_host_init(PluginHost *host, const char *settings_ini_path) {
  if (!host) return -1;
  memset(host, 0, sizeof(*host));

  safe_copy(host->settings_ini, sizeof(host->settings_ini), settings_ini_path ? settings_ini_path : "");

  host->mutex = SDL_CreateMutex();
  if (!host->mutex) return -1;

  char base[1024];
  get_base_dir(base, sizeof(base));
  snprintf(host->plugins_dir, sizeof(host->plugins_dir), "%splugins\\", base);
  ensure_dir(host->plugins_dir);
  return 0;
}

void plugin_host_shutdown(PluginHost *host) {
  if (!host) return;
  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) free_row(&host->rows[i]);
  SDL_free(host->rows);
  host->rows = NULL;
  host->row_count = 0;
  if (host->mutex) {
    SDL_UnlockMutex(host->mutex);
    SDL_DestroyMutex(host->mutex);
    host->mutex = NULL;
  }
}

int plugin_host_rescan(PluginHost *host) {
#ifdef _WIN32
  if (!host) return -1;
  if (host->mutex) SDL_LockMutex(host->mutex);

  // Mark existing rows as unseen.
  uint8_t *seen = NULL;
  if (host->row_count > 0) {
    seen = (uint8_t *)SDL_calloc((size_t)host->row_count, 1);
    if (!seen) {
      if (host->mutex) SDL_UnlockMutex(host->mutex);
      return -1;
    }
  }

  char pattern[1024];
  snprintf(pattern, sizeof(pattern), "%s*.dll", host->plugins_dir);
  WCHAR wpattern[1024] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern,
                          (int)(sizeof(wpattern) / sizeof(WCHAR))) <= 0) {
    if (seen) SDL_free(seen);
    if (host->mutex) SDL_UnlockMutex(host->mutex);
    return 0;
  }
  WIN32_FIND_DATAW ffd;
  HANDLE h = FindFirstFileW(wpattern, &ffd);
  if (h == INVALID_HANDLE_VALUE) {
    if (seen) SDL_free(seen);
    if (host->mutex) SDL_UnlockMutex(host->mutex);
    return 0;
  }

  do {
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    char fn[260] = {0};
    if (WideCharToMultiByte(CP_UTF8, 0, ffd.cFileName, -1, fn, (int)sizeof(fn),
                            NULL, NULL) <= 0) continue;
    if (!fn[0]) continue;
    if (!str_ends_with_i(fn, ".dll")) continue;

    int idx = find_row_by_filename(host, fn);
    if (idx >= 0) {
      if (seen) seen[idx] = 1;
      continue;
    }

    // Add new row.
    int new_count = host->row_count + 1;
    PluginRow *next = (PluginRow *)SDL_realloc(host->rows, (size_t)new_count * sizeof(PluginRow));
    if (!next) break;
    host->rows = next;
    PluginRow *r = &host->rows[new_count - 1];
    memset(r, 0, sizeof(*r));
    safe_copy(r->filename_utf8, sizeof(r->filename_utf8), fn);
    r->enabled = ini_get_int_enabled(host->settings_ini, fn, 1);
    r->is_valid = 0;

    char full[1024];
    snprintf(full, sizeof(full), "%s%s", host->plugins_dir, fn);
    if (load_dll_plugin(host, r, full) == 0) {
      // ok
    } else {
      // invalid or failed; keep row for visibility
      r->enabled = 0;
    }

    host->row_count = new_count;
    if (seen) {
      uint8_t *next_seen = (uint8_t *)SDL_realloc(seen, (size_t)new_count);
      if (next_seen) {
        seen = next_seen;
        seen[new_count - 1] = 1;
      }
    }
  } while (FindNextFileW(h, &ffd));
  FindClose(h);

  // Remove rows not seen anymore.
  if (seen && host->row_count > 0) {
    for (int i = host->row_count - 1; i >= 0; i--) {
      if (seen[i]) continue;
      free_row(&host->rows[i]);
      if (i < host->row_count - 1) {
        memmove(&host->rows[i], &host->rows[i + 1],
                (size_t)(host->row_count - i - 1) * sizeof(PluginRow));
      }
      host->row_count--;
    }
    if (host->row_count == 0) {
      SDL_free(host->rows);
      host->rows = NULL;
    } else {
      PluginRow *shrunk = (PluginRow *)SDL_realloc(host->rows, (size_t)host->row_count * sizeof(PluginRow));
      if (shrunk) host->rows = shrunk;
    }
  }
  if (seen) SDL_free(seen);
  if (host->mutex) SDL_UnlockMutex(host->mutex);
  return 0;
#else
  (void)host;
  return 0;
#endif
}

int plugin_host_set_enabled(PluginHost *host, int index, int enabled) {
  if (!host || index < 0 || index >= host->row_count) return -1;
  if (host->mutex) SDL_LockMutex(host->mutex);
  PluginRow *r = &host->rows[index];
  r->enabled = enabled ? 1 : 0;
  ini_set_int_enabled(host->settings_ini, r->filename_utf8, r->enabled);
  if (host->mutex) SDL_UnlockMutex(host->mutex);
  return 0;
}

void plugin_host_on_video_frame(PluginHost *host, const RtspVideoFrameYuv420p *frame, int zoomed_in) {
  if (!host || !frame) return;
  // Background feeds (a non-displayed camera's frame, on_screen == 0) must not clobber
  // the overlay rects that belong to the camera currently on screen.
  int refresh_overlays = 1;
  if (frame->size >= sizeof(RtspVideoFrameYuv420p) && !frame->on_screen) refresh_overlays = 0;
  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) {
    PluginRow *r = &host->rows[i];
    if (refresh_overlays) r->overlay_count = 0;
    if (!r->enabled || !r->is_valid || !r->api || !r->state) continue;
    if (r->api->on_video_frame_yuv420p) {
      r->api->on_video_frame_yuv420p(r->state, frame, zoomed_in);
    }
    if (refresh_overlays && r->api->get_overlays) {
      int n = r->api->get_overlays(r->state, r->overlays, (int)(sizeof(r->overlays) / sizeof(r->overlays[0])));
      if (n < 0) n = 0;
      if (n > (int)(sizeof(r->overlays) / sizeof(r->overlays[0]))) n = (int)(sizeof(r->overlays) / sizeof(r->overlays[0]));
      r->overlay_count = n;
    }
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);
}

int plugin_host_any_enabled(PluginHost *host) {
  if (!host) return 0;
  int any = 0;
  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) {
    PluginRow *r = &host->rows[i];
    if (r->enabled && r->is_valid && r->api && r->state) {
      any = 1;
      break;
    }
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);
  return any;
}

void plugin_host_on_audio(PluginHost *host, const RtspAudioFrameS16 *audio) {
  if (!host || !audio) return;
  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) {
    PluginRow *r = &host->rows[i];
    if (!r->enabled || !r->is_valid || !r->api || !r->state) continue;
    if (r->api->on_audio_s16) r->api->on_audio_s16(r->state, audio);
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);
}

static void draw_rect_thick(SDL_Renderer *renderer, SDL_Rect r, int thickness) {
  if (thickness < 1) thickness = 1;
  for (int i = 0; i < thickness; i++) {
    SDL_Rect rr = {r.x - i, r.y - i, r.w + 2 * i, r.h + 2 * i};
    SDL_RenderDrawRect(renderer, &rr);
  }
}

void plugin_host_draw_overlays(SDL_Renderer *renderer, PluginHost *host, int win_w, int win_h,
                               int tex_w, int tex_h, double zoom, double view_x, double view_y) {
  if (!renderer || !host) return;
  if (win_w <= 0 || win_h <= 0 || tex_w <= 0 || tex_h <= 0) return;

  SDL_Rect src = {0, 0, tex_w, tex_h};
  if (zoom > 1.0) {
    src.w = (int)((double)tex_w / zoom + 0.5);
    src.h = (int)((double)tex_h / zoom + 0.5);
    if (src.w < 1) src.w = 1;
    if (src.h < 1) src.h = 1;
    src.x = (int)(view_x + 0.5);
    src.y = (int)(view_y + 0.5);
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) {
    PluginRow *r = &host->rows[i];
    if (!r->enabled || !r->is_valid || r->overlay_count <= 0) continue;
    for (int j = 0; j < r->overlay_count; j++) {
      RtspOverlayRect o = r->overlays[j];
      if (o.w <= 0 || o.h <= 0) continue;

      // Map frame coords -> window coords.
      int x0 = o.x;
      int y0 = o.y;
      int x1 = o.x + o.w;
      int y1 = o.y + o.h;

      if (zoom > 1.0) {
        // Only draw visible portion.
        if (x1 < src.x || y1 < src.y || x0 > src.x + src.w || y0 > src.y + src.h) continue;
      }

      double fx0 = (double)(x0 - src.x) / (double)src.w;
      double fy0 = (double)(y0 - src.y) / (double)src.h;
      double fx1 = (double)(x1 - src.x) / (double)src.w;
      double fy1 = (double)(y1 - src.y) / (double)src.h;
      if (fx0 < 0.0) fx0 = 0.0;
      if (fy0 < 0.0) fy0 = 0.0;
      if (fx1 > 1.0) fx1 = 1.0;
      if (fy1 > 1.0) fy1 = 1.0;

      SDL_Rect rr;
      rr.x = (int)(fx0 * (double)win_w + 0.5);
      rr.y = (int)(fy0 * (double)win_h + 0.5);
      rr.w = (int)((fx1 - fx0) * (double)win_w + 0.5);
      rr.h = (int)((fy1 - fy0) * (double)win_h + 0.5);
      if (rr.w < 1) rr.w = 1;
      if (rr.h < 1) rr.h = 1;

      SDL_SetRenderDrawColor(renderer, o.r ? o.r : 255, o.g, o.b, o.a ? o.a : 220);
      draw_rect_thick(renderer, rr, o.thickness > 0 ? o.thickness : 2);
    }
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);
}

int plugin_host_overlay_count_named(PluginHost *host, const char *name_substr) {
  if (!host || !name_substr) return 0;
  int n = 0;
  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) {
    PluginRow *r = &host->rows[i];
    if (r->enabled && r->is_valid && r->state && strstr(r->name_utf8, name_substr)) n += r->overlay_count;
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);
  return n;
}

int plugin_host_is_enabled_named(PluginHost *host, const char *name_substr) {
  if (!host || !name_substr) return 0;
  int found = 0;
  if (host->mutex) SDL_LockMutex(host->mutex);
  for (int i = 0; i < host->row_count; i++) {
    PluginRow *r = &host->rows[i];
    if (r->enabled && r->is_valid && r->state && strstr(r->name_utf8, name_substr)) {
      found = 1;
      break;
    }
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);
  return found;
}
