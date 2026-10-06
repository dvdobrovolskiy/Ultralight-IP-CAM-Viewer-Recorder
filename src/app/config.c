#include "app/config.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "core/util.h"

const char *view_mode_to_string(ViewMode m) {
  switch (m) {
    case VIEW_MODE_MATRIX:
      return "Matrix";
    case VIEW_MODE_TILES:
    default:
      return "Tiles";
  }
}

ViewMode view_mode_from_string(const char *s) {
  if (!s || !*s) return VIEW_MODE_TILES;
  if (SDL_strcasecmp(s, "matrix") == 0) return VIEW_MODE_MATRIX;
  if (SDL_strcasecmp(s, "tiles") == 0) return VIEW_MODE_TILES;
  return VIEW_MODE_TILES;
}

const char *preview_mode_to_string(PreviewMode m) {
  switch (m) {
    case PREVIEW_MODE_WARM:
      return "Warm";
    case PREVIEW_MODE_STREAM:
      return "Stream";
    case PREVIEW_MODE_STATIC:
    default:
      return "Static";
  }
}

PreviewMode preview_mode_from_string(const char *s) {
  if (!s || !*s) return PREVIEW_MODE_STREAM;
  if (SDL_strcasecmp(s, "static") == 0) return PREVIEW_MODE_STATIC;
  if (SDL_strcasecmp(s, "warm") == 0) return PREVIEW_MODE_WARM;
  if (SDL_strcasecmp(s, "stream") == 0) return PREVIEW_MODE_STREAM;
  return PREVIEW_MODE_STREAM;
}

const char *camera_type_to_string(CameraType t) {
  switch (t) {
    case CAMERA_TYPE_RTSP:
      return "RTSP";
    case CAMERA_TYPE_ONVIF:
      return "ONVIF";
    case CAMERA_TYPE_MJPEG:
    default:
      return "MJPEG";
  }
}

CameraType camera_type_from_string(const char *s) {
  if (!s) return CAMERA_TYPE_MJPEG;
  if (SDL_strcasecmp(s, "rtsp") == 0) return CAMERA_TYPE_RTSP;
  if (SDL_strcasecmp(s, "onvif") == 0) return CAMERA_TYPE_ONVIF;
  if (SDL_strcasecmp(s, "mjpeg") == 0) return CAMERA_TYPE_MJPEG;
  return CAMERA_TYPE_MJPEG;
}

int stream_tiles_use_main_mode(int argc, PreviewMode preview_mode, int tiles_use_preview_url,
                               int camera_count) {
  return (argc < 2 && preview_mode == PREVIEW_MODE_STREAM && tiles_use_preview_url == 0 && camera_count > 0) ? 1 : 0;
}

const char *normalize_scale_quality(const char *s) {
  if (!s || !*s) return "linear";
  if (SDL_strcasecmp(s, "nearest") == 0) return "nearest";
  if (SDL_strcasecmp(s, "linear") == 0) return "linear";
  if (SDL_strcasecmp(s, "best") == 0) return "best";
  return "linear";
}

int build_effective_url(const char *input, char *out, size_t out_size) {
  if (!input || !*input || !out || out_size == 0) return -1;

  if (!starts_with(input, "http://") && !starts_with(input, "https://")) {
    snprintf(out, out_size, "%s", input);
    return 0;
  }

  const char *scheme_end = strstr(input, "://");
  if (!scheme_end) {
    snprintf(out, out_size, "%s", input);
    return 0;
  }
  const char *after_scheme = scheme_end + 3;
  const char *path = strchr(after_scheme, '/');
  const char *host_end = path ? path : input + strlen(input);

  const char *default_path = "/video1.mjpg";
  const char *use_path = path ? path : default_path;

  snprintf(out, out_size, "%.*s://%.*s%s",
           (int)(scheme_end - input), input,
           (int)(host_end - after_scheme), after_scheme,
           use_path);
  return 0;
}

int build_camera_url(CameraType type, const char *input, char *out, size_t out_size) {
  if (!input || !*input) return -1;

  // ONVIF cameras stream via the RTSP URI discovered from the device — use it
  // verbatim (before the probe completes this may still be the http device URL,
  // which simply fails to open and is retried).
  if (type == CAMERA_TYPE_RTSP || type == CAMERA_TYPE_ONVIF) {
    snprintf(out, out_size, "%s", input);
    return 0;
  }

  if (starts_with(input, "http://") || starts_with(input, "https://")) {
    return build_effective_url(input, out, out_size);
  }
  if (starts_with(input, "rtsp://")) {
    snprintf(out, out_size, "%s", input);
    return 0;
  }

  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "http://%s", input);
  return build_effective_url(tmp, out, out_size);
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Encoding strategy:
//
//   Values (camera titles, URLs, etc.) are stored on disk as UTF-8 bytes — but
//   pre-existing INI files were written by the old ANSI code paths, which
//   pass-through arbitrary bytes via the system codepage round-trip. To keep
//   those files readable byte-for-byte we deliberately use the A variants of
//   the INI APIs for values: they preserve the on-disk bytes unchanged.
//
//   Paths are kept in UTF-8 throughout the codebase (so SDL_RWFromFile and the
//   wide-char CreateDirectoryW path work). For the A INI APIs we convert the
//   UTF-8 path to the system ANSI codepage just before the call. On systems
//   where the path is pure ASCII the conversion is a no-op; on a Russian
//   system with Cyrillic in the path the CP1251 conversion succeeds and the
//   file is found.
static void utf8_path_to_acp(const char *path_u8, char *out_acp, int out_size) {
  if (!out_acp || out_size <= 0) return;
  out_acp[0] = 0;
  if (!path_u8 || !*path_u8) return;
  WCHAR wpath[1024] = {0};
  if (MultiByteToWideChar(CP_UTF8, 0, path_u8, -1, wpath,
                          (int)(sizeof(wpath) / sizeof(WCHAR))) <= 0) {
    snprintf(out_acp, (size_t)out_size, "%s", path_u8);
    return;
  }
  if (WideCharToMultiByte(CP_ACP, 0, wpath, -1, out_acp, out_size, NULL, NULL) <= 0) {
    snprintf(out_acp, (size_t)out_size, "%s", path_u8);
  }
}

int ini_get_string(const char *path, const char *section, const char *key, const char *def,
                   char *out, DWORD out_chars) {
  if (!out || out_chars == 0) return -1;
  char apath[1024] = {0};
  utf8_path_to_acp(path, apath, (int)sizeof(apath));
  DWORD n = GetPrivateProfileStringA(section, key, def ? def : "", out, out_chars, apath);
  (void)n;
  return 0;
}

int ini_get_int(const char *path, const char *section, const char *key, int def) {
  char apath[1024] = {0};
  utf8_path_to_acp(path, apath, (int)sizeof(apath));
  return (int)GetPrivateProfileIntA(section, key, def, apath);
}

int ini_set_string(const char *path, const char *section, const char *key, const char *val) {
  char apath[1024] = {0};
  utf8_path_to_acp(path, apath, (int)sizeof(apath));
  return WritePrivateProfileStringA(section, key, val ? val : "", apath) ? 0 : -1;
}

int ini_delete_section(const char *path, const char *section) {
  char apath[1024] = {0};
  utf8_path_to_acp(path, apath, (int)sizeof(apath));
  return WritePrivateProfileStringA(section, NULL, NULL, apath) ? 0 : -1;
}

int ini_set_int(const char *path, const char *section, const char *key, int v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%d", v);
  return ini_set_string(path, section, key, buf);
}

static void ini_delete_key(const char *path, const char *section, const char *key) {
  char apath[1024] = {0};
  utf8_path_to_acp(path, apath, (int)sizeof(apath));
  WritePrivateProfileStringA(section, key, NULL, apath);
}

// Persisted ONVIF metadata keys of a CameraN section (OnvifPtz is an int and
// handled separately).
static const char *const kOnvifStringKeys[] = {
    "OnvifDeviceUrl", "OnvifName", "OnvifManufacturer", "OnvifModel",
    "OnvifFirmware",  "OnvifSerial", "OnvifIp",          "OnvifMac",
};

// Persisted PTZ keys of a CameraN section (PtzEnabled is an int).
static const char *const kPtzStringKeys[] = {
    "PtzUrl", "PtzUser", "PtzPass", "PtzProfile",
};

int load_cameras_from_ini(const char *path, CameraConfig **out_cams, int *out_count,
                          int *out_active_camera) {
  if (!path || !out_cams || !out_count) return -1;

  int count = ini_get_int(path, "Cameras", "Count", 0);
  int active = ini_get_int(path, "App", "ActiveCamera", 0);

  if (count <= 0) {
    *out_cams = NULL;
    *out_count = 0;
    if (out_active_camera) *out_active_camera = 0;
    return 0;
  }

  CameraConfig *cams = (CameraConfig *)SDL_calloc((size_t)count, sizeof(CameraConfig));
  if (!cams) return -1;

  for (int i = 0; i < count; i++) {
    char section[64];
    snprintf(section, sizeof(section), "Camera%d", i);

    char type_buf[64] = {0};
    char title_buf[256] = {0};
    char url_buf[2048] = {0};
    char preview_buf[2048] = {0};

    ini_get_string(path, section, "Type", "MJPEG", type_buf, (DWORD)sizeof(type_buf));
    ini_get_string(path, section, "Title", "Camera", title_buf, (DWORD)sizeof(title_buf));
    ini_get_string(path, section, "Url", "", url_buf, (DWORD)sizeof(url_buf));
    ini_get_string(path, section, "PreviewUrl", "", preview_buf, (DWORD)sizeof(preview_buf));
    int rec = ini_get_int(path, section, "Record", 0);
    int keyint = ini_get_int(path, section, "KeyIntMs", 0);
    // Absent key means a config written before Enabled existed: treat as on.
    cams[i].enabled = ini_get_int(path, section, "Enabled", 1) ? 1 : 0;

    cams[i].type = camera_type_from_string(type_buf);
    cams[i].title_utf8 = str_dup(title_buf);
    cams[i].url = str_dup(url_buf);
    cams[i].preview_url = (*preview_buf) ? str_dup(preview_buf) : NULL;

    if (cams[i].type == CAMERA_TYPE_ONVIF) {
      char **slots[] = {&cams[i].onvif_device_url, &cams[i].onvif_name,
                        &cams[i].onvif_manufacturer, &cams[i].onvif_model,
                        &cams[i].onvif_firmware, &cams[i].onvif_serial,
                        &cams[i].onvif_ip, &cams[i].onvif_mac};
      for (int k = 0; k < (int)(sizeof(kOnvifStringKeys) / sizeof(kOnvifStringKeys[0])); k++) {
        char buf[2048] = {0};
        ini_get_string(path, section, kOnvifStringKeys[k], "", buf, (DWORD)sizeof(buf));
        *slots[k] = (*buf) ? str_dup(buf) : NULL;
      }
      cams[i].onvif_has_ptz = ini_get_int(path, section, "OnvifPtz", 0) ? 1 : 0;
    }

    {
      char **ptz_slots[] = {&cams[i].ptz_url, &cams[i].ptz_user, &cams[i].ptz_pass,
                            &cams[i].ptz_profile};
      for (int k = 0; k < (int)(sizeof(kPtzStringKeys) / sizeof(kPtzStringKeys[0])); k++) {
        char buf[2048] = {0};
        ini_get_string(path, section, kPtzStringKeys[k], "", buf, (DWORD)sizeof(buf));
        *ptz_slots[k] = (*buf) ? str_dup(buf) : NULL;
      }
      cams[i].ptz_enabled = ini_get_int(path, section, "PtzEnabled", 0) ? 1 : 0;
    }
    cams[i].record_enabled = rec ? 1 : 0;
    cams[i].keyint_ms = (keyint > 0 && keyint <= 60000) ? keyint : 0;
    cams[i].last_yuv = NULL;
    cams[i].last_yuv_size = 0;
    cams[i].last_w = 0;
    cams[i].last_h = 0;
    cams[i].last_seen_ticks = 0;
    cams[i].thumb_yuv = NULL;
    cams[i].thumb_yuv_size = 0;
    cams[i].thumb_w = 0;
    cams[i].thumb_h = 0;
    cams[i].thumb_last_update_ticks = 0;
    cams[i].feed_yuv = NULL;
    cams[i].feed_yuv_size = 0;
    cams[i].feed_w = 0;
    cams[i].feed_h = 0;
    cams[i].feed_ticks = 0;

    if (!cams[i].title_utf8 || !cams[i].url || !*cams[i].url) {
      free_cameras(cams, count);
      return -1;
    }
  }

  if (active < 0) active = 0;
  if (active >= count) active = count - 1;

  *out_cams = cams;
  *out_count = count;
  if (out_active_camera) *out_active_camera = active;
  return 0;
}

int save_cameras_to_ini(const char *path, const CameraConfig *cams, int count, int active_camera) {
  if (!path) return -1;

  // Refuse to write Count=0 if the on-disk file still has cameras — that would
  // be a destructive overwrite, almost always caused by a load-time bug (e.g.
  // a path-encoding problem misreading Count). Better to leave the user's data
  // intact and surface the issue.
  if (count == 0) {
    char probe[16] = {0};
    ini_get_string(path, "Camera0", "Url", "", probe, (DWORD)sizeof(probe));
    if (probe[0]) {
      return -2;  // refused: would destroy on-disk camera list
    }
  }

  ini_set_int(path, "Cameras", "Count", count);
  ini_set_int(path, "App", "ActiveCamera", active_camera);

  for (int i = 0; i < count; i++) {
    char section[64];
    snprintf(section, sizeof(section), "Camera%d", i);
    ini_set_string(path, section, "Type", camera_type_to_string(cams[i].type));
    ini_set_string(path, section, "Title", cams[i].title_utf8 ? cams[i].title_utf8 : "");
    ini_set_string(path, section, "Url", cams[i].url ? cams[i].url : "");
    ini_set_string(path, section, "PreviewUrl", cams[i].preview_url ? cams[i].preview_url : "");
    ini_set_int(path, section, "Enabled", cams[i].enabled ? 1 : 0);
    ini_set_int(path, section, "Record", cams[i].record_enabled ? 1 : 0);
    if (cams[i].keyint_ms > 0) ini_set_int(path, section, "KeyIntMs", cams[i].keyint_ms);

    if (cams[i].type == CAMERA_TYPE_ONVIF) {
      const char *vals[] = {cams[i].onvif_device_url, cams[i].onvif_name,
                            cams[i].onvif_manufacturer, cams[i].onvif_model,
                            cams[i].onvif_firmware, cams[i].onvif_serial,
                            cams[i].onvif_ip, cams[i].onvif_mac};
      for (int k = 0; k < (int)(sizeof(kOnvifStringKeys) / sizeof(kOnvifStringKeys[0])); k++) {
        ini_set_string(path, section, kOnvifStringKeys[k], vals[k] ? vals[k] : "");
      }
      ini_set_int(path, section, "OnvifPtz", cams[i].onvif_has_ptz ? 1 : 0);
    } else {
      // A camera of another type may reuse a section that used to be ONVIF.
      for (int k = 0; k < (int)(sizeof(kOnvifStringKeys) / sizeof(kOnvifStringKeys[0])); k++) {
        ini_delete_key(path, section, kOnvifStringKeys[k]);
      }
      ini_delete_key(path, section, "OnvifPtz");
    }

    {
      // The endpoint/credentials are kept even while PTZ is switched off, so
      // toggling it back on doesn't mean re-typing them; only PtzEnabled
      // reflects the toggle. Keys are dropped only once nothing is configured.
      const char *ptz_vals[] = {cams[i].ptz_url, cams[i].ptz_user, cams[i].ptz_pass,
                                cams[i].ptz_profile};
      int nkeys = (int)(sizeof(kPtzStringKeys) / sizeof(kPtzStringKeys[0]));
      int have_any = 0;
      for (int k = 0; k < nkeys; k++) {
        if (ptz_vals[k] && ptz_vals[k][0]) have_any = 1;
      }
      if (have_any || cams[i].ptz_enabled) {
        for (int k = 0; k < nkeys; k++) {
          ini_set_string(path, section, kPtzStringKeys[k], ptz_vals[k] ? ptz_vals[k] : "");
        }
        ini_set_int(path, section, "PtzEnabled", cams[i].ptz_enabled ? 1 : 0);
      } else {
        for (int k = 0; k < nkeys; k++) {
          ini_delete_key(path, section, kPtzStringKeys[k]);
        }
        ini_delete_key(path, section, "PtzEnabled");
      }
    }
  }

  for (int i = count; i < count + 32; i++) {
    char section[64];
    snprintf(section, sizeof(section), "Camera%d", i);
    ini_delete_section(path, section);
  }

  return 0;
}
#endif
