#pragma once
// Minimal plugin API for Ultralight IP CAM Viewer Recorder (Windows DLLs).
//
// Design goals:
// - App runs normally with zero plugins.
// - Plugins can observe/modify video frames and optionally observe audio samples.
// - Plugins can report overlay rectangles to be drawn by the host (e.g. object tracking boxes).
//
// ABI notes:
// - This is a C ABI. Plugins should export `rtsp_plugin_get_api`.
// - All structs include `size` so we can extend them without breaking older plugins.
// - Host verifies `magic`, `abi_version`, and `size` before calling anything.

#include <stdint.h>

#ifdef _WIN32
#define RTSP_PLUGIN_EXPORT __declspec(dllexport)
#define RTSP_PLUGIN_IMPORT __declspec(dllimport)
#else
#define RTSP_PLUGIN_EXPORT
#define RTSP_PLUGIN_IMPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RTSP_PLUGIN_MAGIC 0x50535452u /* 'RTSP' little-endian */
#define RTSP_PLUGIN_ABI_VERSION 1u

typedef struct RtspHostLog {
  uint32_t size;
  void (*log_utf8)(int level, const char *msg_utf8); // level: 0 info, 1 warn, 2 err
} RtspHostLog;

typedef struct RtspPluginHostApi {
  uint32_t size;
  RtspHostLog log;
} RtspPluginHostApi;

typedef struct RtspOverlayRect {
  int x;
  int y;
  int w;
  int h;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
  int thickness; // pixels
} RtspOverlayRect;

typedef struct RtspVideoFrameYuv420p {
  uint32_t size;
  uint8_t *y; // writable: plugins may modify in-place
  uint8_t *u;
  uint8_t *v;
  int y_stride;
  int u_stride;
  int v_stride;
  int w;
  int h;
  int64_t pts_ms; // best-effort, may be 0

  // --- Appended fields (host may pass a smaller struct; plugins MUST check
  // `size >= offsetof(...) + sizeof(field)` — or simply `size >= sizeof(RtspVideoFrameYuv420p)`
  // built against this header — before reading them). Old plugins ignore them safely.
  int camera_index;             // stable index of the source camera in the viewer, -1 unknown
  const char *camera_name_utf8; // camera title (valid only during the call), NULL unknown
  int on_screen;                // 1 = frame is being displayed (overlays should target it);
                                // 0 = background feed of a non-displayed camera
} RtspVideoFrameYuv420p;

typedef struct RtspAudioFrameS16 {
  uint32_t size;
  const int16_t *samples;
  int sample_count;   // total int16 samples (interleaved)
  int channels;       // 1 or 2 typically
  int sample_rate_hz; // e.g. 48000
  int64_t pts_ms;     // best-effort, may be 0
} RtspAudioFrameS16;

typedef struct RtspPluginApiV1 {
  uint32_t magic;
  uint32_t abi_version;
  uint32_t size; // sizeof(RtspPluginApiV1)

  const char *(*get_name_utf8)(void);
  const char *(*get_version_utf8)(void); // optional (may return NULL)

  // Called once after load. Plugin may allocate state and return it via *out_state.
  // Return 0 on success.
  int (*init)(const RtspPluginHostApi *host, void **out_state);

  // Called before unload.
  void (*shutdown)(void *state);

  // Video hook. `frame` memory is owned by host and valid only during the call.
  // Plugin must not keep pointers.
  // Return 0 on success.
  int (*on_video_frame_yuv420p)(void *state, const RtspVideoFrameYuv420p *frame, int zoomed_in);

  // Optional: audio hook. Return 0 on success. May be NULL.
  int (*on_audio_s16)(void *state, const RtspAudioFrameS16 *audio);

  // Overlay query. Plugin fills up to max rects; returns count written.
  // May be called every frame after `on_video_frame_yuv420p`.
  int (*get_overlays)(void *state, RtspOverlayRect *out_rects, int max_rects);
} RtspPluginApiV1;

// Plugin entrypoint. Must be exported from the DLL.
RTSP_PLUGIN_EXPORT const RtspPluginApiV1 *rtsp_plugin_get_api(void);

#ifdef __cplusplus
} // extern "C"
#endif
