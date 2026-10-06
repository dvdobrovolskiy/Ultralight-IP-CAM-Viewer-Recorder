#pragma once

#include <stdint.h>

#include <SDL.h>

#include "app/camera.h"

typedef struct CacheFileHeader {
  uint32_t magic;   // 'YUV1' or 'THM1'
  uint32_t version; // 1
  uint32_t w;
  uint32_t h;
  uint32_t size;
  uint64_t key;
} CacheFileHeader;

void camera_cache_update(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h);

void camera_thumb_set_yuv420p(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h,
                              Uint32 now);
// Plugin feed: full-resolution frame for plugins (face detection etc.). Caller
// holds the camera lock. Tile workers only produce these while
// camera_plugin_feed_wanted() is non-zero (set by the UI when a plugin is on).
void camera_feed_set_yuv420p(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h, Uint32 now);
void camera_plugin_feed_set_wanted(int wanted);
int camera_plugin_feed_wanted(void);
// Largest plugin-feed width; bigger streams are scaled down (aspect kept). Face crops
// are cut from this frame, so it bounds the picture quality in the attendance UI: a face
// 3 m from a 4 MP camera is ~110 px at 1920 wide, ~70 px at 1280.
#define CAMERA_PLUGIN_FEED_MAX_W 1920

void camera_main_set_yuv420p(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h,
                             Uint32 now);

#ifdef _WIN32
uint64_t camera_cache_key(const CameraConfig *cam);
void camera_thumb_dims(int src_w, int src_h, int *out_w, int *out_h);
void camera_thumb_update(CameraConfig *cam, const uint8_t *yuv, int src_size, int src_w, int src_h);

void camera_cache_path(char *out, size_t out_sz, const char *base_dir, uint64_t key);
void camera_thumb_path(char *out, size_t out_sz, const char *base_dir, uint64_t key);
void camera_cache_ensure_dir(const char *base_dir);

int camera_cache_load_one(CameraConfig *cam, const char *base_dir);
void camera_cache_load_all(CameraConfig *cams, int count, const char *base_dir);
int camera_thumb_load_one(CameraConfig *cam, const char *base_dir);
void camera_thumb_load_all(CameraConfig *cams, int count, const char *base_dir);

int camera_cache_save_one(CameraConfig *cam, const char *base_dir);
int camera_thumb_save_one(CameraConfig *cam, const char *base_dir);
void camera_cache_save_all(CameraConfig *cams, int count, const char *base_dir);
void camera_thumb_save_all(CameraConfig *cams, int count, const char *base_dir);

typedef struct StartupCacheLoadArgs {
  CameraConfig *cams;
  int cam_count;
  int active_index;
  char base_dir[1024];
  SDL_atomic_t *quit;
} StartupCacheLoadArgs;

int startup_cache_load_thread_fn(void *userdata);
#endif
