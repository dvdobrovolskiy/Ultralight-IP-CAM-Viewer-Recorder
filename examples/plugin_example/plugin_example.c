#include <windows.h>

#include "../../src/plugin_api.h"

static void *g_state = NULL;
static RtspOverlayRect g_rects[1];

static const char *name_utf8(void) { return "Example Rect Tracker"; }
static const char *ver_utf8(void) { return "0.1"; }

static int init_fn(const RtspPluginHostApi *host, void **out_state) {
  (void)host;
  if (!out_state) return -1;
  g_state = (void *)0x1;
  *out_state = g_state;
  return 0;
}

static void shutdown_fn(void *state) { (void)state; }

static int on_video(void *state, const RtspVideoFrameYuv420p *frame, int zoomed_in) {
  (void)state;
  if (!frame || frame->w <= 0 || frame->h <= 0) return 0;

  // Example: when zoomed in, slightly brighten luma in-place.
  if (zoomed_in && frame->y) {
    int w = frame->w;
    int h = frame->h;
    for (int y = 0; y < h; y++) {
      uint8_t *row = frame->y + y * frame->y_stride;
      for (int x = 0; x < w; x++) {
        int v = row[x] + 20;
        row[x] = (uint8_t)(v > 255 ? 255 : v);
      }
    }
  }

  // Example overlay: fixed rectangle at center.
  int rw = frame->w / 3;
  int rh = frame->h / 3;
  g_rects[0].x = frame->w / 2 - rw / 2;
  g_rects[0].y = frame->h / 2 - rh / 2;
  g_rects[0].w = rw;
  g_rects[0].h = rh;
  g_rects[0].r = 255;
  g_rects[0].g = 0;
  g_rects[0].b = 0;
  g_rects[0].a = 220;
  g_rects[0].thickness = 2;
  return 0;
}

static int get_overlays(void *state, RtspOverlayRect *out_rects, int max_rects) {
  (void)state;
  if (!out_rects || max_rects <= 0) return 0;
  out_rects[0] = g_rects[0];
  return 1;
}

static RtspPluginApiV1 g_api = {
    .magic = RTSP_PLUGIN_MAGIC,
    .abi_version = RTSP_PLUGIN_ABI_VERSION,
    .size = sizeof(RtspPluginApiV1),
    .get_name_utf8 = name_utf8,
    .get_version_utf8 = ver_utf8,
    .init = init_fn,
    .shutdown = shutdown_fn,
    .on_video_frame_yuv420p = on_video,
    .on_audio_s16 = NULL,
    .get_overlays = get_overlays,
};

RTSP_PLUGIN_EXPORT const RtspPluginApiV1 *rtsp_plugin_get_api(void) { return &g_api; }

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)hinstDLL;
  (void)fdwReason;
  (void)lpvReserved;
  return TRUE;
}

