#include "cam/cam_cache.h"

#include <stdio.h>
#include <string.h>

#include <libavutil/imgutils.h>

#include "core/build_id.h"
#include "core/log.h"
#include "core/util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef _WIN32
static int read_cache_file(const char *path, uint32_t magic, uint64_t key, uint32_t *out_w, uint32_t *out_h,
                           uint32_t *out_size, uint8_t **out_buf);
static void swap_camera_thumb(CameraConfig *cam, uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t size);
static void swap_camera_cache(CameraConfig *cam, uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t size);
#endif

void camera_cache_update(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h) {
  if (!cam || !yuv || yuv_size <= 0 || w <= 0 || h <= 0) return;
  if (cam->last_yuv_size != yuv_size || !cam->last_yuv) {
    SDL_free(cam->last_yuv);
    cam->last_yuv = (uint8_t *)SDL_malloc((size_t)yuv_size);
    if (!cam->last_yuv) {
      cam->last_yuv_size = 0;
      cam->last_w = 0;
      cam->last_h = 0;
      return;
    }
    cam->last_yuv_size = yuv_size;
  }
  memcpy(cam->last_yuv, yuv, (size_t)yuv_size);
  cam->last_w = w;
  cam->last_h = h;
  cam->last_seen_ticks = SDL_GetTicks();
  cam->cache_dirty = 1;

#ifdef _WIN32
  camera_thumb_update(cam, yuv, yuv_size, w, h);
#endif
}

void camera_thumb_set_yuv420p(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h,
                              Uint32 now) {
  if (!cam || !yuv || yuv_size <= 0 || w <= 0 || h <= 0) return;
  if (cam->thumb_yuv_size != yuv_size || !cam->thumb_yuv) {
    SDL_free(cam->thumb_yuv);
    cam->thumb_yuv = (uint8_t *)SDL_malloc((size_t)yuv_size);
    if (!cam->thumb_yuv) {
      cam->thumb_yuv_size = 0;
      cam->thumb_w = 0;
      cam->thumb_h = 0;
      return;
    }
    cam->thumb_yuv_size = yuv_size;
  }
  memcpy(cam->thumb_yuv, yuv, (size_t)yuv_size);
  cam->thumb_w = w;
  cam->thumb_h = h;
  cam->thumb_last_update_ticks = now;
  cam->thumb_dirty = 1;
  cam->thumb_from_cache = 0;  // live frame
}

void camera_main_set_yuv420p(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h, Uint32 now) {
  if (!cam || !yuv || yuv_size <= 0 || w <= 0 || h <= 0) return;
  if (cam->last_yuv_size != yuv_size || !cam->last_yuv) {
    SDL_free(cam->last_yuv);
    cam->last_yuv = (uint8_t *)SDL_malloc((size_t)yuv_size);
    if (!cam->last_yuv) {
      cam->last_yuv_size = 0;
      cam->last_w = 0;
      cam->last_h = 0;
      return;
    }
    cam->last_yuv_size = yuv_size;
  }
  memcpy(cam->last_yuv, yuv, (size_t)yuv_size);
  cam->last_w = w;
  cam->last_h = h;
  cam->last_seen_ticks = now;
  cam->cache_dirty = 1;
}

static SDL_atomic_t g_plugin_feed_wanted;

void camera_plugin_feed_set_wanted(int wanted) { SDL_AtomicSet(&g_plugin_feed_wanted, wanted ? 1 : 0); }
int camera_plugin_feed_wanted(void) { return SDL_AtomicGet(&g_plugin_feed_wanted); }

void camera_feed_set_yuv420p(CameraConfig *cam, const uint8_t *yuv, int yuv_size, int w, int h, Uint32 now) {
  if (!cam || !yuv || yuv_size <= 0 || w <= 0 || h <= 0) return;
  if (cam->feed_yuv_size != yuv_size || !cam->feed_yuv) {
    SDL_free(cam->feed_yuv);
    cam->feed_yuv = (uint8_t *)SDL_malloc((size_t)yuv_size);
    if (!cam->feed_yuv) {
      cam->feed_yuv_size = 0;
      cam->feed_w = 0;
      cam->feed_h = 0;
      return;
    }
    cam->feed_yuv_size = yuv_size;
  }
  memcpy(cam->feed_yuv, yuv, (size_t)yuv_size);
  cam->feed_w = w;
  cam->feed_h = h;
  cam->feed_ticks = now ? now : 1;
}

#ifdef _WIN32
uint64_t camera_cache_key(const CameraConfig *cam) {
  if (!cam || !cam->url) return 0;
  char buf[4096];
  snprintf(buf, sizeof(buf), "%d|%s", (int)cam->type, cam->url);
  return fnv1a64(buf, strlen(buf));
}

void camera_thumb_dims(int src_w, int src_h, int *out_w, int *out_h) {
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (src_w <= 0 || src_h <= 0 || !out_w || !out_h) return;
  int w = src_w & ~1;
  int h = src_h & ~1;
  if (w < 2 || h < 2) return;

  const int max_w = 320;
  const int max_h = 180;

  int tw = max_w;
  int th = (int)((int64_t)tw * (int64_t)h / (int64_t)w);
  if (th > max_h) {
    th = max_h;
    tw = (int)((int64_t)th * (int64_t)w / (int64_t)h);
  }
  tw &= ~1;
  th &= ~1;
  if (tw < 2) tw = 2;
  if (th < 2) th = 2;
  *out_w = tw;
  *out_h = th;
}

void camera_thumb_update(CameraConfig *cam, const uint8_t *yuv, int src_size, int src_w, int src_h) {
  if (!cam || !yuv || src_size <= 0) return;
  int w = src_w & ~1;
  int h = src_h & ~1;
  if (w < 2 || h < 2) return;

  Uint32 now = SDL_GetTicks();
  if (cam->thumb_last_update_ticks && (Uint32)(now - cam->thumb_last_update_ticks) < 250) return;

  int tw = 0, th = 0;
  camera_thumb_dims(w, h, &tw, &th);
  if (tw <= 0 || th <= 0) return;

  int expected_src = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
  if (expected_src <= 0 || expected_src > src_size) return;

  int dst_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, tw, th, 1);
  if (dst_size <= 0) return;
  if (!cam->thumb_yuv || cam->thumb_yuv_size != dst_size) {
    SDL_free(cam->thumb_yuv);
    cam->thumb_yuv = (uint8_t *)SDL_malloc((size_t)dst_size);
    if (!cam->thumb_yuv) {
      cam->thumb_yuv_size = 0;
      cam->thumb_w = 0;
      cam->thumb_h = 0;
      return;
    }
    cam->thumb_yuv_size = dst_size;
  }

  const uint8_t *src_y = yuv;
  const uint8_t *src_u = src_y + (size_t)w * (size_t)h;
  const uint8_t *src_v = src_u + ((size_t)w * (size_t)h) / 4;

  uint8_t *dst_y = cam->thumb_yuv;
  uint8_t *dst_u = dst_y + (size_t)tw * (size_t)th;
  uint8_t *dst_v = dst_u + ((size_t)tw * (size_t)th) / 4;

  for (int dy = 0; dy < th; dy++) {
    int sy = (int)((int64_t)dy * (int64_t)h / (int64_t)th);
    const uint8_t *srow = src_y + (size_t)sy * (size_t)w;
    uint8_t *drow = dst_y + (size_t)dy * (size_t)tw;
    for (int dx = 0; dx < tw; dx++) {
      int sx = (int)((int64_t)dx * (int64_t)w / (int64_t)tw);
      drow[dx] = srow[sx];
    }
  }

  int src_w2 = w / 2;
  int src_h2 = h / 2;
  int dst_w2 = tw / 2;
  int dst_h2 = th / 2;
  for (int dy = 0; dy < dst_h2; dy++) {
    int sy = (int)((int64_t)dy * (int64_t)src_h2 / (int64_t)dst_h2);
    const uint8_t *srow_u = src_u + (size_t)sy * (size_t)src_w2;
    const uint8_t *srow_v = src_v + (size_t)sy * (size_t)src_w2;
    uint8_t *drow_u = dst_u + (size_t)dy * (size_t)dst_w2;
    uint8_t *drow_v = dst_v + (size_t)dy * (size_t)dst_w2;
    for (int dx = 0; dx < dst_w2; dx++) {
      int sx = (int)((int64_t)dx * (int64_t)src_w2 / (int64_t)dst_w2);
      drow_u[dx] = srow_u[sx];
      drow_v[dx] = srow_v[sx];
    }
  }

  cam->thumb_w = tw;
  cam->thumb_h = th;
  cam->thumb_last_update_ticks = now;
  cam->thumb_dirty = 1;
  cam->thumb_from_cache = 0;  // live frame
}

void camera_cache_path(char *out, size_t out_sz, const char *base_dir, uint64_t key) {
  if (!out || out_sz == 0) return;
  out[0] = 0;
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  snprintf(out, out_sz, "%scache\\%016llx.yuvc", base_use, (unsigned long long)key);
}

void camera_thumb_path(char *out, size_t out_sz, const char *base_dir, uint64_t key) {
  if (!out || out_sz == 0) return;
  out[0] = 0;
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  snprintf(out, out_sz, "%scache\\thumb_%016llx.yuvc", base_use, (unsigned long long)key);
}

void camera_cache_ensure_dir(const char *base_dir) {
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  char dir[1024] = {0};
  snprintf(dir, sizeof(dir), "%scache", base_use);
  ensure_dir_recursive_win32(dir);
}

int camera_cache_load_one(CameraConfig *cam, const char *base_dir) {
  if (!cam) return -1;
  uint64_t key = camera_cache_key(cam);
  if (!key) return -1;

  char path[1024] = {0};
  camera_cache_path(path, sizeof(path), base_dir, key);
  SDL_RWops *rw = SDL_RWFromFile(path, "rb");
  if (!rw) return -1;

  CacheFileHeader hdr = {0};
  if (SDL_RWread(rw, &hdr, 1, sizeof(hdr)) != sizeof(hdr)) {
    SDL_RWclose(rw);
    return -1;
  }
  if (hdr.magic != 0x31565559u /*YUV1*/ || hdr.version != 1 || hdr.key != key) {
    SDL_RWclose(rw);
    return -1;
  }
  if (hdr.w == 0 || hdr.h == 0 || hdr.size == 0) {
    SDL_RWclose(rw);
    return -1;
  }
  int expected = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, (int)hdr.w, (int)hdr.h, 1);
  if (expected <= 0 || (uint32_t)expected != hdr.size) {
    SDL_RWclose(rw);
    return -1;
  }

  Sint64 file_size = SDL_RWsize(rw);
  if (file_size < 0 || (uint64_t)file_size != (uint64_t)sizeof(hdr) + (uint64_t)hdr.size) {
    SDL_RWclose(rw);
    return -1;
  }

  uint8_t *buf = (uint8_t *)SDL_malloc((size_t)hdr.size);
  if (!buf) {
    SDL_RWclose(rw);
    return -1;
  }
  if (SDL_RWread(rw, buf, 1, hdr.size) != hdr.size) {
    SDL_free(buf);
    SDL_RWclose(rw);
    return -1;
  }
  SDL_RWclose(rw);

  SDL_free(cam->last_yuv);
  cam->last_yuv = buf;
  cam->last_yuv_size = (int)hdr.size;
  cam->last_w = (int)hdr.w;
  cam->last_h = (int)hdr.h;
  cam->last_seen_ticks = 0;
  cam->cache_dirty = 0;
  return 0;
}

void camera_cache_load_all(CameraConfig *cams, int count, const char *base_dir) {
  if (!cams || count <= 0) return;
  for (int i = 0; i < count; i++) {
    camera_cache_load_one(&cams[i], base_dir);
  }
}

int camera_thumb_load_one(CameraConfig *cam, const char *base_dir) {
  if (!cam) return -1;
  uint64_t key = camera_cache_key(cam);
  if (!key) return -1;

  char path[1024] = {0};
  camera_thumb_path(path, sizeof(path), base_dir, key);
  SDL_RWops *rw = SDL_RWFromFile(path, "rb");
  if (!rw) return -1;

  CacheFileHeader hdr = {0};
  if (SDL_RWread(rw, &hdr, 1, sizeof(hdr)) != sizeof(hdr)) {
    SDL_RWclose(rw);
    return -1;
  }
  if (hdr.magic != 0x314d4854u /*THM1*/ || hdr.version != 1 || hdr.key != key) {
    SDL_RWclose(rw);
    return -1;
  }
  if (hdr.w == 0 || hdr.h == 0 || hdr.size == 0) {
    SDL_RWclose(rw);
    return -1;
  }
  int expected = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, (int)hdr.w, (int)hdr.h, 1);
  if (expected <= 0 || (uint32_t)expected != hdr.size) {
    SDL_RWclose(rw);
    return -1;
  }

  Sint64 file_size = SDL_RWsize(rw);
  if (file_size < 0 || (uint64_t)file_size != (uint64_t)sizeof(hdr) + (uint64_t)hdr.size) {
    SDL_RWclose(rw);
    return -1;
  }

  uint8_t *buf = (uint8_t *)SDL_malloc((size_t)hdr.size);
  if (!buf) {
    SDL_RWclose(rw);
    return -1;
  }
  if (SDL_RWread(rw, buf, 1, hdr.size) != hdr.size) {
    SDL_free(buf);
    SDL_RWclose(rw);
    return -1;
  }
  SDL_RWclose(rw);

  SDL_free(cam->thumb_yuv);
  cam->thumb_yuv = buf;
  cam->thumb_yuv_size = (int)hdr.size;
  cam->thumb_w = (int)hdr.w;
  cam->thumb_h = (int)hdr.h;
  cam->thumb_last_update_ticks = SDL_GetTicks();
  cam->thumb_dirty = 0;
  // Restored from disk: display it, but it is not evidence of a live stream.
  // (A non-zero stamp is required for the texture upload path to pick it up,
  // which is why this can't simply be left at 0 like the main-frame loader.)
  cam->thumb_from_cache = 1;
  return 0;
}

void camera_thumb_load_all(CameraConfig *cams, int count, const char *base_dir) {
  if (!cams || count <= 0) return;
  for (int i = 0; i < count; i++) {
    camera_thumb_load_one(&cams[i], base_dir);
  }
}

static int read_cache_file(const char *path, uint32_t magic, uint64_t key, uint32_t *out_w, uint32_t *out_h,
                           uint32_t *out_size, uint8_t **out_buf) {
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (out_size) *out_size = 0;
  if (out_buf) *out_buf = NULL;
  if (!path || !*path || !out_w || !out_h || !out_size || !out_buf) return -1;

  SDL_RWops *rw = SDL_RWFromFile(path, "rb");
  if (!rw) return -1;

  CacheFileHeader hdr = {0};
  if (SDL_RWread(rw, &hdr, 1, sizeof(hdr)) != sizeof(hdr)) {
    SDL_RWclose(rw);
    return -1;
  }
  if (hdr.magic != magic || hdr.version != 1 || hdr.key != key) {
    SDL_RWclose(rw);
    return -1;
  }
  if (hdr.w == 0 || hdr.h == 0 || hdr.size == 0) {
    SDL_RWclose(rw);
    return -1;
  }
  int expected = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, (int)hdr.w, (int)hdr.h, 1);
  if (expected <= 0 || (uint32_t)expected != hdr.size) {
    SDL_RWclose(rw);
    return -1;
  }

  Sint64 file_size = SDL_RWsize(rw);
  if (file_size < 0 || (uint64_t)file_size != (uint64_t)sizeof(hdr) + (uint64_t)hdr.size) {
    SDL_RWclose(rw);
    return -1;
  }

  uint8_t *buf = (uint8_t *)SDL_malloc((size_t)hdr.size);
  if (!buf) {
    SDL_RWclose(rw);
    return -1;
  }
  if (SDL_RWread(rw, buf, 1, hdr.size) != hdr.size) {
    SDL_free(buf);
    SDL_RWclose(rw);
    return -1;
  }
  SDL_RWclose(rw);

  *out_w = hdr.w;
  *out_h = hdr.h;
  *out_size = hdr.size;
  *out_buf = buf;
  return 0;
}

static void swap_camera_thumb(CameraConfig *cam, uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t size) {
  if (!cam || !buf || w == 0 || h == 0 || size == 0) return;
  Uint32 now = SDL_GetTicks();
  if (cam->lock) SDL_LockMutex(cam->lock);
  SDL_free(cam->thumb_yuv);
  cam->thumb_yuv = buf;
  cam->thumb_yuv_size = (int)size;
  cam->thumb_w = (int)w;
  cam->thumb_h = (int)h;
  cam->thumb_last_update_ticks = now;
  cam->thumb_dirty = 0;
  cam->thumb_from_cache = 1;  // restored from disk, not a live frame
  if (cam->lock) SDL_UnlockMutex(cam->lock);
}

static void swap_camera_cache(CameraConfig *cam, uint8_t *buf, uint32_t w, uint32_t h,
                              uint32_t size) {
  if (!cam || !buf || w == 0 || h == 0 || size == 0) return;
  if (cam->lock) SDL_LockMutex(cam->lock);
  SDL_free(cam->last_yuv);
  cam->last_yuv = buf;
  cam->last_yuv_size = (int)size;
  cam->last_w = (int)w;
  cam->last_h = (int)h;
  cam->last_seen_ticks = 0;
  cam->cache_dirty = 0;
  if (cam->lock) SDL_UnlockMutex(cam->lock);
}

int startup_cache_load_thread_fn(void *userdata) {
  StartupCacheLoadArgs *a = (StartupCacheLoadArgs *)userdata;
  if (!a || !a->cams || a->cam_count <= 0 || !a->quit) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  const char *base_use = (a->base_dir[0] ? a->base_dir : ".\\");

  // 1) Load thumbnails first (fast): makes tiles show cached images quickly.
  for (int i = 0; i < a->cam_count; i++) {
    if (SDL_AtomicGet(a->quit)) return 0;

    uint64_t key = camera_cache_key(&a->cams[i]);
    if (!key) continue;

    char path[1024] = {0};
    camera_thumb_path(path, sizeof(path), base_use, key);

    uint32_t w = 0, h = 0, size = 0;
    uint8_t *buf = NULL;
    if (read_cache_file(path, 0x314d4854u /*THM1*/, key, &w, &h, &size, &buf) == 0) {
      swap_camera_thumb(&a->cams[i], buf, w, h, size);
    }

    SDL_Delay((i < 16) ? 0 : 2);
  }

  // 2) Load full cache for the active camera.
  if (!SDL_AtomicGet(a->quit) && a->active_index >= 0 && a->active_index < a->cam_count) {
    int i = a->active_index;
    uint64_t key = camera_cache_key(&a->cams[i]);
    if (key) {
      char path[1024] = {0};
      camera_cache_path(path, sizeof(path), base_use, key);

      uint32_t w = 0, h = 0, size = 0;
      uint8_t *buf = NULL;
      if (read_cache_file(path, 0x31565559u /*YUV1*/, key, &w, &h, &size, &buf) == 0) {
        swap_camera_cache(&a->cams[i], buf, w, h, size);

        if (!SDL_AtomicGet(a->quit)) {
          int need_thumb = 0;
          SDL_mutex *lk = a->cams[i].lock;
          if (lk) SDL_LockMutex(lk);
          need_thumb = (a->cams[i].thumb_yuv == NULL) ? 1 : 0;
          if (lk) SDL_UnlockMutex(lk);
          if (need_thumb) {
            CameraConfig tmp = {0};
            camera_thumb_update(&tmp, buf, (int)size, (int)w, (int)h);
            if (tmp.thumb_yuv && tmp.thumb_w > 0 && tmp.thumb_h > 0 && tmp.thumb_yuv_size > 0) {
              swap_camera_thumb(&a->cams[i], tmp.thumb_yuv, (uint32_t)tmp.thumb_w,
                                (uint32_t)tmp.thumb_h, (uint32_t)tmp.thumb_yuv_size);
              tmp.thumb_yuv = NULL;
            }
            SDL_free(tmp.thumb_yuv);
          }
        }
      }
    }
  }

  return 0;
}

int camera_cache_save_one(CameraConfig *cam, const char *base_dir) {
  if (!cam || !cam->url) return -1;
  if (!cam->last_yuv || cam->last_yuv_size <= 0 || cam->last_w <= 0 || cam->last_h <= 0) return -1;
  uint64_t key = camera_cache_key(cam);
  if (!key) return -1;

  char path[1024] = {0};
  camera_cache_path(path, sizeof(path), base_dir, key);

  char tmp_path[1024] = {0};
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  SDL_RWops *rw = SDL_RWFromFile(tmp_path, "wb");
  if (!rw) return -1;

  CacheFileHeader hdr = {0};
  hdr.magic = 0x31565559u; // YUV1
  hdr.version = 1;
  hdr.w = (uint32_t)cam->last_w;
  hdr.h = (uint32_t)cam->last_h;
  hdr.size = (uint32_t)cam->last_yuv_size;
  hdr.key = key;

  if (SDL_RWwrite(rw, &hdr, 1, sizeof(hdr)) != sizeof(hdr) ||
      SDL_RWwrite(rw, cam->last_yuv, 1, (size_t)cam->last_yuv_size) != (size_t)cam->last_yuv_size) {
    SDL_RWclose(rw);
    DeleteFileA(tmp_path);
    return -1;
  }
  SDL_RWclose(rw);

  if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(tmp_path);
    return -1;
  }
  cam->cache_dirty = 0;
  return 0;
}

int camera_thumb_save_one(CameraConfig *cam, const char *base_dir) {
  if (!cam || !cam->url) return -1;
  if (!cam->thumb_yuv || cam->thumb_yuv_size <= 0 || cam->thumb_w <= 0 || cam->thumb_h <= 0) return -1;
  uint64_t key = camera_cache_key(cam);
  if (!key) return -1;

  char path[1024] = {0};
  camera_thumb_path(path, sizeof(path), base_dir, key);

  char tmp_path[1024] = {0};
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

  SDL_RWops *rw = SDL_RWFromFile(tmp_path, "wb");
  if (!rw) return -1;

  CacheFileHeader hdr = {0};
  hdr.magic = 0x314d4854u; // THM1
  hdr.version = 1;
  hdr.w = (uint32_t)cam->thumb_w;
  hdr.h = (uint32_t)cam->thumb_h;
  hdr.size = (uint32_t)cam->thumb_yuv_size;
  hdr.key = key;

  if (SDL_RWwrite(rw, &hdr, 1, sizeof(hdr)) != sizeof(hdr) ||
      SDL_RWwrite(rw, cam->thumb_yuv, 1, (size_t)cam->thumb_yuv_size) != (size_t)cam->thumb_yuv_size) {
    SDL_RWclose(rw);
    DeleteFileA(tmp_path);
    return -1;
  }
  SDL_RWclose(rw);

  if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(tmp_path);
    return -1;
  }
  cam->thumb_dirty = 0;
  return 0;
}

void camera_cache_save_all(CameraConfig *cams, int count, const char *base_dir) {
  if (!cams || count <= 0) return;
  camera_cache_ensure_dir(base_dir);
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  char log_path[1024] = {0};
  snprintf(log_path, sizeof(log_path), "%scache\\cache.log", base_use);
  int saved = 0;
  int failed = 0;
  int eligible = 0;
  for (int i = 0; i < count; i++) {
    if (cams[i].last_yuv_size <= 0 || cams[i].last_w <= 0 || cams[i].last_h <= 0) continue;
    if (!cams[i].cache_dirty) continue;
    eligible++;
    if (camera_cache_save_one(&cams[i], base_dir) == 0) {
      saved++;
    } else {
      failed++;
    }
  }
  {
    char line[256];
    snprintf(line, sizeof(line), "cache_save: build=%s eligible=%d saved=%d failed=%d", BUILD_ID, eligible,
             saved, failed);
    append_log_line_utf8(log_path, line);
  }
}

void camera_thumb_save_all(CameraConfig *cams, int count, const char *base_dir) {
  if (!cams || count <= 0) return;
  camera_cache_ensure_dir(base_dir);
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  char log_path[1024] = {0};
  snprintf(log_path, sizeof(log_path), "%scache\\cache.log", base_use);
  int saved = 0;
  int failed = 0;
  int eligible = 0;
  for (int i = 0; i < count; i++) {
    if (cams[i].thumb_yuv_size <= 0 || cams[i].thumb_w <= 0 || cams[i].thumb_h <= 0) continue;
    if (!cams[i].thumb_dirty) continue;
    eligible++;
    if (camera_thumb_save_one(&cams[i], base_dir) == 0) {
      saved++;
    } else {
      failed++;
    }
  }
  {
    char line[256];
    snprintf(line, sizeof(line), "thumb_save: build=%s eligible=%d saved=%d failed=%d", BUILD_ID, eligible,
             saved, failed);
    append_log_line_utf8(log_path, line);
  }
}
#endif
