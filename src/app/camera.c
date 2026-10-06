#include "app/camera.h"

#include <string.h>

#include "core/util.h"

void free_cameras(CameraConfig *cams, int count) {
  if (!cams) return;
  for (int i = 0; i < count; i++) {
    SDL_free(cams[i].title_utf8);
    SDL_free(cams[i].url);
    SDL_free(cams[i].preview_url);
    SDL_free(cams[i].last_yuv);
    SDL_free(cams[i].thumb_yuv);
    SDL_free(cams[i].feed_yuv);
    camera_clear_onvif(&cams[i]);
    camera_clear_ptz(&cams[i]);
    if (cams[i].lock) SDL_DestroyMutex(cams[i].lock);
  }
  SDL_free(cams);
}

void camera_clear_onvif(CameraConfig *cam) {
  if (!cam) return;
  SDL_free(cam->onvif_device_url);
  SDL_free(cam->onvif_name);
  SDL_free(cam->onvif_manufacturer);
  SDL_free(cam->onvif_model);
  SDL_free(cam->onvif_firmware);
  SDL_free(cam->onvif_serial);
  SDL_free(cam->onvif_ip);
  SDL_free(cam->onvif_mac);
  cam->onvif_device_url = NULL;
  cam->onvif_name = NULL;
  cam->onvif_manufacturer = NULL;
  cam->onvif_model = NULL;
  cam->onvif_firmware = NULL;
  cam->onvif_serial = NULL;
  cam->onvif_ip = NULL;
  cam->onvif_mac = NULL;
  cam->onvif_has_ptz = 0;
}

static char *dup_or_null(const char *s) {
  return (s && *s) ? str_dup(s) : NULL;
}

void camera_set_onvif_meta(CameraConfig *cam, const char *name, const char *manufacturer,
                           const char *model, const char *firmware, const char *serial,
                           const char *ip, const char *mac, int has_ptz) {
  if (!cam) return;
  SDL_free(cam->onvif_name);
  SDL_free(cam->onvif_manufacturer);
  SDL_free(cam->onvif_model);
  SDL_free(cam->onvif_firmware);
  SDL_free(cam->onvif_serial);
  SDL_free(cam->onvif_ip);
  SDL_free(cam->onvif_mac);
  cam->onvif_name = dup_or_null(name);
  cam->onvif_manufacturer = dup_or_null(manufacturer);
  cam->onvif_model = dup_or_null(model);
  cam->onvif_firmware = dup_or_null(firmware);
  cam->onvif_serial = dup_or_null(serial);
  cam->onvif_ip = dup_or_null(ip);
  cam->onvif_mac = dup_or_null(mac);
  cam->onvif_has_ptz = has_ptz ? 1 : 0;
}

void camera_clear_ptz(CameraConfig *cam) {
  if (!cam) return;
  SDL_free(cam->ptz_url);
  SDL_free(cam->ptz_user);
  SDL_free(cam->ptz_pass);
  SDL_free(cam->ptz_profile);
  cam->ptz_url = NULL;
  cam->ptz_user = NULL;
  cam->ptz_pass = NULL;
  cam->ptz_profile = NULL;
}

void camera_set_ptz(CameraConfig *cam, int enabled, const char *url, const char *user,
                    const char *pass, const char *profile) {
  if (!cam) return;
  camera_clear_ptz(cam);
  cam->ptz_enabled = enabled ? 1 : 0;
  cam->ptz_url = dup_or_null(url);
  cam->ptz_user = dup_or_null(user);
  cam->ptz_pass = dup_or_null(pass);
  cam->ptz_profile = dup_or_null(profile);
}

void cameras_ensure_locks(CameraConfig *cams, int count) {
  if (!cams) return;
  for (int i = 0; i < count; i++) {
    if (!cams[i].lock) cams[i].lock = SDL_CreateMutex();
  }
}

int cameras_add(CameraConfig **cams, int *count, CameraType type, const char *title,
                const char *url, const char *preview_url) {
  if (!cams || !count || !title || !url) return -1;
  if (!*title || !*url) return -1;

  int new_count = *count + 1;
  CameraConfig *next = (CameraConfig *)SDL_realloc(*cams, (size_t)new_count * sizeof(CameraConfig));
  if (!next) return -1;
  // The realloc may have moved (and freed) the old block — publish the new
  // pointer immediately so no failure path below leaves *cams dangling.
  *cams = next;
  memset(&next[new_count - 1], 0, sizeof(CameraConfig));
  next[new_count - 1].type = type;
  next[new_count - 1].title_utf8 = str_dup(title);
  next[new_count - 1].url = str_dup(url);
  next[new_count - 1].preview_url = (preview_url && *preview_url) ? str_dup(preview_url) : NULL;
  next[new_count - 1].enabled = 1;  // new cameras are active by default
  next[new_count - 1].record_enabled = 0;
  next[new_count - 1].keyint_ms = 0;
  next[new_count - 1].last_yuv = NULL;
  next[new_count - 1].last_yuv_size = 0;
  next[new_count - 1].last_w = 0;
  next[new_count - 1].last_h = 0;
  next[new_count - 1].last_seen_ticks = 0;
  next[new_count - 1].thumb_yuv = NULL;
  next[new_count - 1].thumb_yuv_size = 0;
  next[new_count - 1].thumb_w = 0;
  next[new_count - 1].thumb_h = 0;
  next[new_count - 1].thumb_last_update_ticks = 0;
  next[new_count - 1].feed_yuv = NULL;
  next[new_count - 1].feed_yuv_size = 0;
  next[new_count - 1].feed_w = 0;
  next[new_count - 1].feed_h = 0;
  next[new_count - 1].feed_ticks = 0;
  if (!next[new_count - 1].title_utf8 || !next[new_count - 1].url) {
    SDL_free(next[new_count - 1].title_utf8);
    SDL_free(next[new_count - 1].url);
    SDL_free(next[new_count - 1].preview_url);
    memset(&next[new_count - 1], 0, sizeof(CameraConfig));
    return -1;
  }
  *count = new_count;
  return 0;
}

int cameras_edit(CameraConfig *cams, int count, int index, CameraType type, const char *title,
                 const char *url, const char *preview_url) {
  if (!cams || index < 0 || index >= count || !title || !url) return -1;
  if (!*title || !*url) return -1;

  int invalidate_cache = 0;
  if (cams[index].url && SDL_strcasecmp(cams[index].url, url) != 0) invalidate_cache = 1;
  if (cams[index].type != type) invalidate_cache = 1;

  char *t = str_dup(title);
  char *u = str_dup(url);
  char *p = (preview_url && *preview_url) ? str_dup(preview_url) : NULL;
  if (!t || !u || ((preview_url && *preview_url) && !p)) {
    SDL_free(t);
    SDL_free(u);
    SDL_free(p);
    return -1;
  }

  SDL_free(cams[index].title_utf8);
  SDL_free(cams[index].url);
  SDL_free(cams[index].preview_url);
  cams[index].title_utf8 = t;
  cams[index].url = u;
  cams[index].preview_url = p;
  cams[index].type = type;
  if (invalidate_cache) {
    SDL_free(cams[index].last_yuv);
    cams[index].last_yuv = NULL;
    cams[index].last_yuv_size = 0;
    cams[index].last_w = 0;
    cams[index].last_h = 0;
    cams[index].last_seen_ticks = 0;

    SDL_free(cams[index].thumb_yuv);
    cams[index].thumb_yuv = NULL;
    cams[index].thumb_yuv_size = 0;
    cams[index].thumb_w = 0;
    cams[index].thumb_h = 0;
    cams[index].thumb_last_update_ticks = 0;
    cams[index].thumb_from_cache = 0;

    SDL_free(cams[index].feed_yuv);
    cams[index].feed_yuv = NULL;
    cams[index].feed_yuv_size = 0;
    cams[index].feed_w = 0;
    cams[index].feed_h = 0;
    cams[index].feed_ticks = 0;

    cams[index].keyint_ms = 0;
  }
  return 0;
}

int cameras_delete(CameraConfig **cams, int *count, int index) {
  if (!cams || !count || !*cams || *count <= 0) return -1;
  if (index < 0 || index >= *count) return -1;

  CameraConfig *arr = *cams;
  SDL_free(arr[index].title_utf8);
  SDL_free(arr[index].url);
  SDL_free(arr[index].preview_url);
  SDL_free(arr[index].last_yuv);
  SDL_free(arr[index].thumb_yuv);
  SDL_free(arr[index].feed_yuv);
  camera_clear_onvif(&arr[index]);
  camera_clear_ptz(&arr[index]);
  // Destroy this camera's lock before the memmove shifts entries down. Surviving
  // cameras keep their existing lock pointers (only the array storage moves).
  if (arr[index].lock) SDL_DestroyMutex(arr[index].lock);
  if (index < *count - 1) {
    memmove(&arr[index], &arr[index + 1], (size_t)(*count - index - 1) * sizeof(CameraConfig));
  }

  int new_count = *count - 1;
  if (new_count == 0) {
    SDL_free(arr);
    *cams = NULL;
    *count = 0;
    return 0;
  }

  CameraConfig *next = (CameraConfig *)SDL_realloc(arr, (size_t)new_count * sizeof(CameraConfig));
  if (next) arr = next;
  *cams = arr;
  *count = new_count;
  return 0;
}
