#pragma once

#include <stddef.h>

#include "app/camera.h"

typedef enum ViewMode {
  VIEW_MODE_TILES = 0,
  VIEW_MODE_MATRIX = 1,
} ViewMode;

typedef enum PreviewMode {
  PREVIEW_MODE_STATIC = 0,
  PREVIEW_MODE_WARM = 1,
  PREVIEW_MODE_STREAM = 2,
} PreviewMode;

const char *view_mode_to_string(ViewMode m);
ViewMode view_mode_from_string(const char *s);

const char *preview_mode_to_string(PreviewMode m);
PreviewMode preview_mode_from_string(const char *s);

const char *camera_type_to_string(CameraType t);
CameraType camera_type_from_string(const char *s);

int stream_tiles_use_main_mode(int argc, PreviewMode preview_mode, int tiles_use_preview_url,
                               int camera_count);

const char *normalize_scale_quality(const char *s);

int build_effective_url(const char *input, char *out, size_t out_size);
int build_camera_url(CameraType type, const char *input, char *out, size_t out_size);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// All path/section/key/value strings are UTF-8. Internally these route through
// the Win32 W APIs so non-ASCII characters (Cyrillic, accents, etc.) in any of
// them work regardless of the system ANSI codepage.
int ini_get_string(const char *path, const char *section, const char *key, const char *def,
                   char *out, DWORD out_chars);
int ini_get_int(const char *path, const char *section, const char *key, int def);
int ini_set_string(const char *path, const char *section, const char *key, const char *val);
int ini_set_int(const char *path, const char *section, const char *key, int v);
int ini_delete_section(const char *path, const char *section);

int load_cameras_from_ini(const char *path, CameraConfig **out_cams, int *out_count,
                          int *out_active_camera);
int save_cameras_to_ini(const char *path, const CameraConfig *cams, int count, int active_camera);
#endif
