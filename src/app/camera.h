#pragma once

#include <stdint.h>

#include <SDL.h>

typedef enum CameraType {
  CAMERA_TYPE_RTSP = 0,
  CAMERA_TYPE_MJPEG = 1,
  CAMERA_TYPE_ONVIF = 2,
} CameraType;

typedef struct CameraConfig {
  CameraType type;
  char *title_utf8;
  char *url;
  char *preview_url;
  // Master switch (persisted, default 1). A disabled camera keeps its entry and
  // its cached thumbnail but is never connected to: no tile/warm worker, no
  // decode, no recording. Lets a broken or unused camera stop consuming
  // connection slots without deleting its configuration.
  int enabled;
  int record_enabled;
  int keyint_ms;

  // ONVIF (persisted; only meaningful for CAMERA_TYPE_ONVIF). onvif_device_url
  // is the service endpoint the user entered; after a successful probe url /
  // preview_url hold the discovered RTSP main/sub stream URIs and the metadata
  // below is filled from the device.
  char *onvif_device_url;
  char *onvif_name;
  char *onvif_manufacturer;
  char *onvif_model;
  char *onvif_firmware;
  char *onvif_serial;
  char *onvif_ip;
  char *onvif_mac;
  int onvif_has_ptz;

  // PTZ (persisted). Available for RTSP cameras, where the user supplies the
  // ONVIF PTZ endpoint and credentials by hand, and for ONVIF cameras, where
  // the probe fills all of it in automatically when the device reports PTZ.
  int ptz_enabled;
  char *ptz_url;
  char *ptz_user;
  char *ptz_pass;
  char *ptz_profile;  // media profile token; auto-discovered when empty

  // Per-camera lock guarding this camera's runtime frame buffers (last_yuv /
  // thumb_yuv and their dimensions). Each camera owns its own mutex so a slow
  // worker or a busy UI tile only ever blocks on a single camera, never the
  // whole fleet. Created lazily via cameras_ensure_locks(); destroyed in
  // cameras_delete() / free_cameras(). The camera array is only mutated
  // (add/delete/realloc) while workers are stopped, so the lock pointer is
  // stable for the lifetime of any worker that holds it.
  SDL_mutex *lock;

  // Runtime-only cache (not persisted): last decoded YUV420P frame for instant preview.
  uint8_t *last_yuv;
  int last_yuv_size;
  int last_w;
  int last_h;
  Uint32 last_seen_ticks;
  int cache_dirty;

  // Thumbnail cache for camera tiles (small YUV420P).
  uint8_t *thumb_yuv;
  int thumb_yuv_size;
  int thumb_w;
  int thumb_h;
  Uint32 thumb_last_update_ticks;
  int thumb_dirty;
  // 1 while thumb_yuv holds an image restored from the on-disk cache rather
  // than a frame from the live stream. The thumbnail still renders (that is the
  // point of the warm start), but the UI must not mistake it for a working feed
  // — without this, a cached still suppresses the "connecting" spinner for the
  // first seconds after launch and the camera looks like it is already playing.
  int thumb_from_cache;

  // Plugin feed (runtime-only): the freshest decoded tile frame at (near) native
  // resolution, refreshed ~5x/s by the tile worker while any plugin is enabled.
  // Face detectors need real pixels - the 320x180 thumb above is far too small
  // (a face 40 px wide in a 640x360 stream is 20 px in the thumb).
  uint8_t *feed_yuv;
  int feed_yuv_size;
  int feed_w;
  int feed_h;
  Uint32 feed_ticks;
} CameraConfig;

void free_cameras(CameraConfig *cams, int count);

// Idempotently create a per-camera lock for every entry whose lock is NULL.
// Call after loading/adding cameras (the initial load path uses SDL_calloc and
// does not go through cameras_add). Safe to call repeatedly; must only run on
// the UI thread while camera workers are stopped.
void cameras_ensure_locks(CameraConfig *cams, int count);

int cameras_add(CameraConfig **cams, int *count, CameraType type, const char *title,
                const char *url, const char *preview_url);
int cameras_edit(CameraConfig *cams, int count, int index, CameraType type, const char *title,
                 const char *url, const char *preview_url);
int cameras_delete(CameraConfig **cams, int *count, int index);

// Free + NULL every onvif_* field (device URL included), reset onvif_has_ptz.
void camera_clear_onvif(CameraConfig *cam);
// Replace the ONVIF metadata (device URL untouched). Empty strings become NULL.
void camera_set_onvif_meta(CameraConfig *cam, const char *name, const char *manufacturer,
                           const char *model, const char *firmware, const char *serial,
                           const char *ip, const char *mac, int has_ptz);

// Free + NULL every ptz_* string; leaves ptz_enabled untouched.
void camera_clear_ptz(CameraConfig *cam);
// Replace the PTZ endpoint/credentials. Empty strings become NULL.
void camera_set_ptz(CameraConfig *cam, int enabled, const char *url, const char *user,
                    const char *pass, const char *profile);
