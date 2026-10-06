#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "plugin_host.h"

#include "app/camera.h"
#include "app/config.h"
#include "app/persist.h"
#include "app/i18n.h"
#include "audio/audio.h"
#include "audio/audio_pipe.h"
#include "cam/cam_cache.h"
#include "app/tray.h"
#include "app/single_instance.h"
#include "cam/decode.h"
#include "cam/archive.h"
#include "cam/onvif.h"
#include "cam/ptz.h"
#include "cam/record.h"
#include "cam/tile_stream.h"
#include "cam/time_machine.h"
#include "cam/warm.h"
#include "core/build_id.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/seh.h"
#include "core/util.h"
#include "core/watchdog.h"
#include "ui/text.h"
#include "ui/ui_cam_mgr.h"
#include "ui/ui_intro.h"
#include "ui/ui_plugins.h"
#include "ui/ui_render.h"
#include "ui/ui_settings.h"
#include "ui/ui_timeline.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wingdi.h>
#endif

// Background settings writer. Created once in main(); all runtime settings.ini
// writes go through it so disk I/O never blocks the UI thread. File-scope so the
// apply_cameras_changed() helper can reach it without widening its signature.
static PersistQueue *g_persist = NULL;

// Animated "connecting" spinner: a ring of 12 dots with a rotating bright head,
// drawn centred at (cx,cy). Shown while a camera tile / view is buffering instead
// of the old "Буферизация NN%" text — an unreachable camera now shows a neutral
// activity icon centred in the tile (or screen when expanded) rather than a stuck
// 99%. Uses a fixed unit-circle table so it needs no <math.h>/trig.
static void draw_spinner(SDL_Renderer *renderer, int cx, int cy, int radius, Uint32 ticks) {
  if (!renderer || radius < 4) return;
  static const float kDir[12][2] = {
      { 1.000f,  0.000f}, { 0.866f,  0.500f}, { 0.500f,  0.866f}, { 0.000f,  1.000f},
      {-0.500f,  0.866f}, {-0.866f,  0.500f}, {-1.000f,  0.000f}, {-0.866f, -0.500f},
      {-0.500f, -0.866f}, { 0.000f, -1.000f}, { 0.500f, -0.866f}, { 0.866f, -0.500f},
  };
  int head = (int)((ticks / 80u) % 12u);  // ~one full turn per second
  int dot = radius / 3;
  if (dot < 3) dot = 3;
  SDL_BlendMode prev = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(renderer, &prev);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (int i = 0; i < 12; i++) {
    int dist = (head - i + 12) % 12;
    int a = 235 - dist * 16;
    if (a < 45) a = 45;
    int px = cx + (int)(kDir[i][0] * (float)radius);
    int py = cy + (int)(kDir[i][1] * (float)radius);
    SDL_Rect d = {px - dot / 2, py - dot / 2, dot, dot};
    SDL_SetRenderDrawColor(renderer, 240, 240, 240, (Uint8)a);
    SDL_RenderFillRect(renderer, &d);
  }
  SDL_SetRenderDrawBlendMode(renderer, prev);
}

// A camera may be connected to only when it is enabled and has a URL. Every
// path that starts the main decode thread goes through this, so a disabled
// camera never opens a session (the tile/warm workers are gated separately, in
// their prepare functions, by leaving the worker's URL empty).
static int camera_streamable(const CameraConfig *cams, int count, int idx) {
  if (!cams || idx < 0 || idx >= count) return 0;
  if (!cams[idx].enabled) return 0;
  return (cams[idx].url && cams[idx].url[0]) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// PTZ pad. Directions: 0 up, 1 down, 2 left, 3 right, 4 zoom in, 5 zoom out.

#define PTZ_DIR_COUNT 6
#define PTZ_SPEED 0.5f

// Index of the camera the pad controls, or -1 when no PTZ camera is shown big.
// Mirrors the "one camera fills the window" test used by the renderer.
static int ptz_pad_camera(const CameraConfig *cams, int count, int from_list, ViewMode view_mode,
                          int matrix_expanded, int active_camera) {
  if (!from_list || !cams || count <= 0) return -1;
  if (view_mode == VIEW_MODE_MATRIX && count > 1 && matrix_expanded < 0) return -1;  // grid
  int idx = (view_mode == VIEW_MODE_MATRIX && matrix_expanded >= 0) ? matrix_expanded
                                                                    : active_camera;
  if (idx < 0 || idx >= count) return -1;
  if (!cams[idx].ptz_enabled) return -1;
  if (!cams[idx].ptz_url || !cams[idx].ptz_url[0]) return -1;
  return idx;
}

static void ptz_fill_target(const CameraConfig *cam, PtzTarget *t) {
  memset(t, 0, sizeof(*t));
  if (!cam) return;
  snprintf(t->url, sizeof(t->url), "%s", cam->ptz_url ? cam->ptz_url : "");
  snprintf(t->user, sizeof(t->user), "%s", cam->ptz_user ? cam->ptz_user : "");
  snprintf(t->pass, sizeof(t->pass), "%s", cam->ptz_pass ? cam->ptz_pass : "");
  snprintf(t->profile, sizeof(t->profile), "%s", cam->ptz_profile ? cam->ptz_profile : "");
}

static void ptz_dir_velocity(int dir, float *vx, float *vy, float *vz) {
  *vx = *vy = *vz = 0.0f;
  switch (dir) {
    case 0: *vy = PTZ_SPEED; break;   // up
    case 1: *vy = -PTZ_SPEED; break;  // down
    case 2: *vx = -PTZ_SPEED; break;  // left
    case 3: *vx = PTZ_SPEED; break;   // right
    case 4: *vz = PTZ_SPEED; break;   // zoom in
    case 5: *vz = -PTZ_SPEED; break;  // zoom out
    default: break;
  }
}

// A 3x3 arrow cluster with a zoom row underneath, parked against the right edge
// so it clears both the bottom tile bar and the time-machine strip.
static void ptz_pad_layout(int win_w, int win_h, SDL_Rect *pad, SDL_Rect *btns) {
  const int b = 44, gap = 6, margin = 20;
  int cluster_w = 3 * b + 2 * gap;
  int cluster_h = 4 * b + 3 * gap;
  int x0 = win_w - cluster_w - margin;
  int y0 = (win_h - cluster_h) / 2;
  if (x0 < margin) x0 = margin;
  if (y0 < margin) y0 = margin;

  SDL_Rect up = {x0 + b + gap, y0, b, b};
  SDL_Rect left = {x0, y0 + b + gap, b, b};
  SDL_Rect right = {x0 + 2 * (b + gap), y0 + b + gap, b, b};
  SDL_Rect down = {x0 + b + gap, y0 + 2 * (b + gap), b, b};
  SDL_Rect zin = {x0, y0 + 3 * (b + gap), b, b};
  SDL_Rect zout = {x0 + 2 * (b + gap), y0 + 3 * (b + gap), b, b};
  btns[0] = up;
  btns[1] = down;
  btns[2] = left;
  btns[3] = right;
  btns[4] = zin;
  btns[5] = zout;

  SDL_Rect r = {x0 - 8, y0 - 8, cluster_w + 16, cluster_h + 16};
  *pad = r;
}

static int ptz_pad_hit(const SDL_Rect *btns, int mx, int my) {
  for (int i = 0; i < PTZ_DIR_COUNT; i++) {
    const SDL_Rect *r = &btns[i];
    if (mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h) return i;
  }
  return -1;
}

// Chevron / plus / minus glyphs, stroked a few pixels thick.
static void ptz_draw_glyph(SDL_Renderer *ren, const SDL_Rect *r, int dir) {
  int cx = r->x + r->w / 2, cy = r->y + r->h / 2;
  int s = r->w / 5;
  for (int t = -1; t <= 1; t++) {
    switch (dir) {
      case 0:
        SDL_RenderDrawLine(ren, cx - s, cy + s / 2 + t, cx, cy - s / 2 + t);
        SDL_RenderDrawLine(ren, cx, cy - s / 2 + t, cx + s, cy + s / 2 + t);
        break;
      case 1:
        SDL_RenderDrawLine(ren, cx - s, cy - s / 2 + t, cx, cy + s / 2 + t);
        SDL_RenderDrawLine(ren, cx, cy + s / 2 + t, cx + s, cy - s / 2 + t);
        break;
      case 2:
        SDL_RenderDrawLine(ren, cx + s / 2 + t, cy - s, cx - s / 2 + t, cy);
        SDL_RenderDrawLine(ren, cx - s / 2 + t, cy, cx + s / 2 + t, cy + s);
        break;
      case 3:
        SDL_RenderDrawLine(ren, cx - s / 2 + t, cy - s, cx + s / 2 + t, cy);
        SDL_RenderDrawLine(ren, cx + s / 2 + t, cy, cx - s / 2 + t, cy + s);
        break;
      case 4:
        SDL_RenderDrawLine(ren, cx - s, cy + t, cx + s, cy + t);
        SDL_RenderDrawLine(ren, cx + t, cy - s, cx + t, cy + s);
        break;
      case 5:
        SDL_RenderDrawLine(ren, cx - s, cy + t, cx + s, cy + t);
        break;
      default:
        break;
    }
  }
}

static void ptz_pad_render(SDL_Renderer *ren, const SDL_Rect *pad, const SDL_Rect *btns,
                           int active_dir, int busy) {
  SDL_BlendMode prev = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(ren, &prev);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  SDL_SetRenderDrawColor(ren, 0, 0, 0, 110);
  SDL_RenderFillRect(ren, pad);

  for (int i = 0; i < PTZ_DIR_COUNT; i++) {
    if (i == active_dir) {
      SDL_SetRenderDrawColor(ren, 40, 120, 40, 230);
    } else {
      SDL_SetRenderDrawColor(ren, 45, 45, 45, 190);
    }
    SDL_RenderFillRect(ren, &btns[i]);
    SDL_SetRenderDrawColor(ren, 170, 170, 170, 220);
    SDL_RenderDrawRect(ren, &btns[i]);
    SDL_SetRenderDrawColor(ren, 240, 240, 240, 240);
    ptz_draw_glyph(ren, &btns[i], i);
  }

  if (busy) {
    // Small activity dot in the pad corner while a command is in flight.
    SDL_Rect dot = {pad->x + pad->w - 12, pad->y + 5, 6, 6};
    SDL_SetRenderDrawColor(ren, 120, 220, 120, 230);
    SDL_RenderFillRect(ren, &dot);
  }

  SDL_SetRenderDrawBlendMode(ren, prev);
}

static int apply_cameras_changed(const char *settings_path, SDL_Renderer *renderer, int text_pt,
                                 SDL_Color text_color, CameraConfig *cameras, int camera_count,
                                 CameraUiItem **camera_ui, int old_ui_count, CameraManager *mgr,
                                 int *active_camera, int stream_from_list, int need_restart,
                                 SDL_Window *window, VideoShared *shared, SDL_Thread **th,
                                 DecodeArgs *dargs, AudioState *audio,
                                 TextUiItem *error_text, int *has_error, int *is_loading,
                                 Uint32 *loading_since, double *zoom, double *view_x, double *view_y,
                                 int *dragging, Uint32 *last_frame_ticks, Uint32 *retry_at,
                                 int *retry_delay_ms, char *active_url_buf, size_t active_url_buf_sz,
                                 SDL_mutex *rec_mutex, int *rec_want, char *rec_title_buf,
                                 size_t rec_title_buf_sz) {
  if (!renderer || !camera_ui || !active_camera || !window || !shared || !th || !dargs ||
      !error_text || !has_error || !is_loading || !loading_since || !zoom || !view_x || !view_y ||
      !dragging || !last_frame_ticks || !retry_at || !retry_delay_ms || !active_url_buf ||
      active_url_buf_sz == 0) {
    return -1;
  }

  if (camera_count > 0) {
    if (*active_camera < 0) *active_camera = 0;
    if (*active_camera >= camera_count) *active_camera = camera_count - 1;
  } else {
    *active_camera = 0;
  }

  if (rec_mutex) SDL_LockMutex(rec_mutex);
  if (rec_want) {
    if (stream_from_list && camera_count > 0) {
      *rec_want = cameras[*active_camera].record_enabled ? 1 : 0;
    } else {
      *rec_want = 0;
    }
  }
  if (rec_title_buf && rec_title_buf_sz > 0) {
    if (stream_from_list && camera_count > 0) {
      snprintf(rec_title_buf, rec_title_buf_sz, "%s",
               cameras[*active_camera].title_utf8 ? cameras[*active_camera].title_utf8 : "Camera");
    } else {
      rec_title_buf[0] = 0;
    }
  }
  dargs->camera_index = *active_camera;
  if (rec_title_buf && rec_title_buf_sz > 0) dargs->camera_title_utf8 = rec_title_buf;
  if (rec_mutex) SDL_UnlockMutex(rec_mutex);

#ifdef _WIN32
  if (settings_path && settings_path[0]) {
    persist_save_cameras(g_persist, settings_path, cameras, camera_count, *active_camera);
  }
#else
  (void)settings_path;
#endif

  free_camera_ui(*camera_ui, old_ui_count);
  *camera_ui = build_camera_ui_items(renderer, cameras, camera_count, text_pt, text_color);
  if (camera_count > 0 && !*camera_ui) return -1;

  if (mgr && mgr->is_open) {
    camera_manager_rebuild_list(mgr, cameras, camera_count, text_pt, text_color);
  }

  if (stream_from_list) {
    char title[256];
    if (camera_count > 0) {
      snprintf(title, sizeof(title), "Ultralight IP CAM Viewer Recorder - %s",
               cameras[*active_camera].title_utf8 ? cameras[*active_camera].title_utf8 : "Camera");
    } else {
      snprintf(title, sizeof(title), "Ultralight IP CAM Viewer Recorder");
    }
    SDL_SetWindowTitle(window, title);
  }

  if (stream_from_list && (need_restart || (*th == NULL && camera_count > 0))) {
    stop_decode_thread(shared, th);
    audio_state_clear_queue(audio);
    if (error_text->tex) {
      SDL_DestroyTexture(error_text->tex);
      error_text->tex = NULL;
    }
    *has_error = 0;

    if (camera_count > 0 && camera_streamable(cameras, camera_count, *active_camera)) {
      build_camera_url(cameras[*active_camera].type, cameras[*active_camera].url, active_url_buf,
                       active_url_buf_sz);
      dargs->url = active_url_buf;
      dargs->initial_keyint_ms = (cameras[*active_camera].keyint_ms > 0) ? cameras[*active_camera].keyint_ms : 5000;
      *th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", dargs, &shared->alive);
      if (!*th) return -1;

      *is_loading = 1;
      *loading_since = SDL_GetTicks();
      *zoom = 1.0;
      *view_x = 0.0;
      *view_y = 0.0;
      *dragging = 0;
      *last_frame_ticks = SDL_GetTicks();
      *retry_at = 0;
      *retry_delay_ms = 1000;
    } else {
      active_url_buf[0] = 0;
      dargs->url = active_url_buf;
      *is_loading = 0;
    }
  }

  return 0;
}

// After a camera-manager save: an ONVIF entry remembers the device URL the user
// typed and kicks off a background probe that will fill the stream URLs and
// metadata (completion is handled once per frame in the main loop); any other
// type drops stale ONVIF metadata the entry may carry from a previous type.
// Copy the edit form's PTZ fields onto the camera. The profile token is not a
// form field: it is discovered once and kept across edits.
static void apply_form_ptz(CameraConfig *cam, const CameraManager *mgr) {
  if (!cam || !mgr) return;
  if (mgr->edit_type != CAMERA_TYPE_RTSP && mgr->edit_type != CAMERA_TYPE_ONVIF) {
    camera_set_ptz(cam, 0, NULL, NULL, NULL, NULL);
    return;
  }
  char *profile = cam->ptz_profile ? str_dup(cam->ptz_profile) : NULL;
  camera_set_ptz(cam, mgr->edit_ptz_on, mgr->edit_ptz_url, mgr->edit_ptz_user, mgr->edit_ptz_pass,
                 profile);
  SDL_free(profile);
}

static void onvif_after_save(CameraConfig *cam, CameraType type, const char *device_url,
                             OnvifProbe **probe, char *probe_target, size_t probe_target_sz,
                             CameraManager *mgr, int text_pt, SDL_Color text_color) {
  if (!cam || !probe || !probe_target) return;
  if (type != CAMERA_TYPE_ONVIF || !device_url || !*device_url) {
    camera_clear_onvif(cam);
    return;
  }
  // Same device and metadata already collected: keep both and skip the
  // re-probe, so hand-tuned stream URLs and a mere title edit survive. But if
  // the stream URL is just the device-URL placeholder (blanked by the user or
  // never discovered), a probe is still needed to obtain a playable stream.
  int same_device = cam->onvif_device_url && strcmp(cam->onvif_device_url, device_url) == 0;
  int have_meta = cam->onvif_manufacturer || cam->onvif_model || cam->onvif_serial ||
                  cam->onvif_mac;
  int url_is_placeholder = !cam->url || !*cam->url || strcmp(cam->url, device_url) == 0;
  if (same_device && have_meta && !url_is_placeholder) return;
  camera_clear_onvif(cam);
  cam->onvif_device_url = str_dup(device_url);
  if (*probe) onvif_probe_destroy(*probe);
  *probe = onvif_probe_start(device_url);
  snprintf(probe_target, probe_target_sz, "%s", device_url);
  if (*probe) {
    camera_manager_set_status(mgr, tr(STR_ONVIF_PROBING), text_pt, text_color);
  }
}

int main(int argc, char **argv) {
  CameraConfig *cameras = NULL;
  int camera_count = 0;
  int active_camera = 0;
  char settings_path[1024] = {0};
  char base_dir[1024] = {0};
  char scale_quality[32] = "linear";
  PreviewMode preview_mode = PREVIEW_MODE_STREAM;
  int tiles_use_preview_url = 1;
  int fast_exit = 1;
  ViewMode view_mode = VIEW_MODE_TILES;
  int rec_segment_seconds = 60;
  int archive_limit_gb = 0; // recordings size cap in GB (0 = Off); INI [Recording] MaxArchiveGB
  int expected_keyint_ms = 5000;
  int created_settings_ini = 0;
  int intro_shown = 0;
  // Current "show help at startup" preference (settings.ini [App] IntroAtStartup).
  // Tracked so the F1 help overlay opens with the checkbox in the right state.
  int intro_pref_show = 1;

#ifdef _WIN32
  SDL_Thread *watchdog_th = NULL;
  SDL_atomic_t main_heartbeat;
  SDL_atomic_t watchdog_quit;
  UiHangWatchdogArgs watchdog_args = {0};

  // Recording archive size manager: low-priority thread that trims oldest
  // segments when the recordings folder exceeds the user's GB cap. The cap is
  // shared live via archive_limit_gb_atomic so Settings changes take effect on
  // the next check (0 = Off).
  SDL_Thread *archive_th = NULL;
  SDL_atomic_t archive_quit;
  SDL_atomic_t archive_limit_gb_atomic;
  ArchiveTrimArgs archive_args = {0};
#endif

  AudioState audio = {0};
  SDL_Thread *audio_th = NULL;

  // Phase 3/6: dedicated audio-stream worker for the currently-selected camera. To
  // keep tile clicks instant, the worker is *detached* on switch — main never calls
  // SDL_WaitThread on it. Each spawn gets its own heap-allocated args struct so the
  // dying thread can keep using its old args while the new thread uses the new ones;
  // each thread frees its own args on exit.
  int audio_volume_pct = 100;
  // The audio pipe + dedicated worker own the only SDL audio queue write path.
  // Producers (decode.c, tile_stream.c) just clone audio packets into the pipe
  // when their camera is the active source. audio_active_idx_atomic is the
  // active source camera; volume + playing-sticky live inside the pipe but we
  // hold pointers so the UI can read/write them.
  SDL_atomic_t audio_volume_pct_atomic;
  SDL_atomic_t audio_playing_sticky;
  SDL_atomic_t audio_active_idx_atomic;
  SDL_AtomicSet(&audio_volume_pct_atomic, 100);
  SDL_AtomicSet(&audio_playing_sticky, 0);
  SDL_AtomicSet(&audio_active_idx_atomic, -1);
  AudioPipe *audio_pipe = NULL;
  SDL_Thread *audio_worker_th = NULL;
  // Tracks which camera is the audio source. When it changes, reset the sticky
  // playing flag so the speaker icon doesn't carry over to a camera with no
  // audio (e.g. MJPEG).
  int last_audio_src_idx = -2;

#ifndef _WIN32
  (void)file_exists;
#endif

  // Only one copy may run: a second launch just brings the running one to the
  // front (it may be hidden in the tray) and exits before touching any files.
  if (!single_instance_acquire()) return 0;

#ifdef _WIN32
  get_settings_ini_path(settings_path, sizeof(settings_path));
  dirname_from_path(settings_path, base_dir, sizeof(base_dir));
  append_startup_marker(base_dir, "begin");
  startup_tracef(base_dir, "begin build=%s", BUILD_ID);
  // Create cache directory up-front so it exists even before first successful save (saves happen on exit only).
  camera_cache_ensure_dir(base_dir);
  append_startup_marker(base_dir, "cache_dir_ok");
  startup_tracef(base_dir, "cache_dir_ok settings_path=%s", settings_path);

  // Phase 2: install per-thread crash containment as early as possible — before any
  // background threads spawn — so a fault in any of them stays scoped to that thread.
  seh_install_global_handler(base_dir);
  if (!file_exists(settings_path)) {
    save_cameras_to_ini(settings_path, NULL, 0, 0);
    created_settings_ini = 1;
  }
  append_startup_marker(base_dir, "ini_ok");
  startup_tracef(base_dir, "ini_ok created=%d", created_settings_ini);
  // Phase 9: intro is shown at every startup unless the user has ticked
  // "Don't show at startup" — that ticks IntroAtStartup to 0 in the INI.
  intro_pref_show = ini_get_int(settings_path, "App", "IntroAtStartup", 1);
  intro_shown = intro_pref_show ? 0 : 1;
  ini_get_string(settings_path, "App", "RenderScaleQuality", "linear", scale_quality,
                 (DWORD)sizeof(scale_quality));
  snprintf(scale_quality, sizeof(scale_quality), "%s", normalize_scale_quality(scale_quality));
  {
    char pm_buf[32] = {0};
    ini_get_string(settings_path, "App", "PreviewMode", "", pm_buf, (DWORD)sizeof(pm_buf));
    if (pm_buf[0]) {
      preview_mode = preview_mode_from_string(pm_buf);
    } else {
      char old_buf[32] = {0};
      ini_get_string(settings_path, "App", "KeepCamerasWarm", "", old_buf, (DWORD)sizeof(old_buf));
      if (old_buf[0]) {
        preview_mode = atoi(old_buf) ? PREVIEW_MODE_WARM : PREVIEW_MODE_STATIC;
      } else {
        preview_mode = PREVIEW_MODE_STREAM;
      }
    }
  }
  tiles_use_preview_url = ini_get_int(settings_path, "App", "TilesUsePreviewURL", 1) ? 1 : 0;
  fast_exit = ini_get_int(settings_path, "App", "FastExit", 1) ? 1 : 0;
  {
    int vol = ini_get_int(settings_path, "App", "AudioVolumePct", 100);
    if (vol < 0) vol = 0; else if (vol > 200) vol = 200;
    audio_volume_pct = vol;
    SDL_AtomicSet(&audio_volume_pct_atomic, vol);
  }
  {
    char lang_buf[16] = {0};
    ini_get_string(settings_path, "App", "Language", "en", lang_buf, (DWORD)sizeof(lang_buf));
    i18n_set_language(language_from_string(lang_buf));
  }
  {
    char mode_buf[32] = {0};
    // Fresh installs (no INI / no ViewMode key) start in the matrix grid —
    // it's the multi-camera dashboard view most users want first.
    ini_get_string(settings_path, "App", "ViewMode", "Matrix", mode_buf, (DWORD)sizeof(mode_buf));
    view_mode = view_mode_from_string(mode_buf);
  }

  {
    char seg_buf[32] = {0};
    ini_get_string(settings_path, "Recording", "SegmentSeconds", "60", seg_buf, (DWORD)sizeof(seg_buf));
    int v = atoi(seg_buf);
    if (v < 5) v = 60;
    if (v > 3600) v = 3600;
    rec_segment_seconds = v;
  }
  archive_limit_gb = ini_get_int(settings_path, "Recording", "MaxArchiveGB", 0);
  if (archive_limit_gb < 0) archive_limit_gb = 0;
  if (archive_limit_gb > SETTINGS_ARCHIVE_MAX_GB) archive_limit_gb = SETTINGS_ARCHIVE_MAX_GB;

  if (load_cameras_from_ini(settings_path, &cameras, &camera_count, &active_camera) != 0) {
    fprintf(stderr, "Failed to load settings.ini\n");
    return 1;
  }
  // Each camera owns its own frame-buffer lock; the load path uses SDL_calloc
  // (not cameras_add), so create the locks here before any worker spawns.
  cameras_ensure_locks(cameras, camera_count);
  // Background settings writer: all runtime settings.ini writes are queued here so
  // disk I/O never stalls the UI thread (a camera switch alone would otherwise do
  // dozens of WritePrivateProfileString flushes inline).
  g_persist = persist_start();
  append_startup_marker(base_dir, "load_cameras_ok");
  startup_tracef(base_dir, "load_cameras_ok camera_count=%d active=%d", camera_count, active_camera);
  if (camera_count > 0) {
    if (active_camera >= 0 && active_camera < camera_count && cameras[active_camera].keyint_ms > 0) {
      expected_keyint_ms = cameras[active_camera].keyint_ms;
    }
  }

  {
    char log_path[1024] = {0};
    snprintf(log_path, sizeof(log_path), "%scache\\cache.log", base_dir[0] ? base_dir : ".\\");
    char line[2048] = {0};
    snprintf(line, sizeof(line),
             "startup: build=%s settings=%s base=%s cameras=%d active=%d preview_mode=%s tiles_use_preview_url=%d",
             BUILD_ID, settings_path, base_dir, camera_count, active_camera, preview_mode_to_string(preview_mode),
             tiles_use_preview_url ? 1 : 0);
    append_log_line_utf8(log_path, line);
  }
#else
  // Non-Windows: no INI persistence (yet), start with no cameras.
  camera_count = 0;
  active_camera = 0;
#endif

  if (argc >= 2) {
    // If URL is passed explicitly, treat it as the active stream (camera list still shown).
    // This keeps the app usable for ad-hoc URLs.
  }
  char active_url_buf[2048] = {0};
  if (argc >= 2) {
    build_effective_url(argv[1], active_url_buf, sizeof(active_url_buf));
  } else {
    if (camera_count > 0) {
      build_camera_url(cameras[active_camera].type, cameras[active_camera].url, active_url_buf,
                       sizeof(active_url_buf));
    }
  }
  // In Matrix mode we render the grid from cached thumbnails; only start a live decode when a camera
  // is expanded (or when there's only 1 camera).
  if (argc < 2 && view_mode == VIEW_MODE_MATRIX && camera_count > 1) {
    active_url_buf[0] = 0;
  }
  // When Stream mode is enabled and tiles use Main URL, the main stream is already being received
  // by the per-tile threads. Avoid starting a duplicate main decode at startup.
  if (stream_tiles_use_main_mode(argc, preview_mode, tiles_use_preview_url, camera_count)) {
    active_url_buf[0] = 0;
  }
  const char *url = active_url_buf;

  av_log_set_level(AV_LOG_ERROR);
  avformat_network_init();

  SDL_SetMainReady();
  Uint32 t0 = SDL_GetTicks();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) die_sdl("SDL_Init failed");
  append_startup_marker(base_dir, "sdl_init_ok");
  startup_tracef(base_dir, "sdl_init_ok dt_ms=%u", (unsigned)(SDL_GetTicks() - t0));

#ifdef _WIN32
  SDL_AtomicSet(&main_heartbeat, (int)SDL_GetTicks());
  SDL_AtomicSet(&watchdog_quit, 0);
  snprintf(watchdog_args.base_dir, sizeof(watchdog_args.base_dir), "%s", base_dir);
  watchdog_args.quit = &watchdog_quit;
  watchdog_args.main_heartbeat = &main_heartbeat;
  watchdog_th = seh_create_thread(ui_hang_watchdog_thread_fn, "ui-watchdog", &watchdog_args, NULL);
  startup_tracef(base_dir, "watchdog_started ok=%d", watchdog_th ? 1 : 0);

  // Recording archive trimmer (low priority, casual periodic checks).
  SDL_AtomicSet(&archive_quit, 0);
  SDL_AtomicSet(&archive_limit_gb_atomic, archive_limit_gb);
  snprintf(archive_args.base_dir, sizeof(archive_args.base_dir), "%s", base_dir);
  snprintf(archive_args.recordings_dir, sizeof(archive_args.recordings_dir), "%srecordings",
           base_dir[0] ? base_dir : ".\\");
  archive_args.limit_gb = &archive_limit_gb_atomic;
  archive_args.quit = &archive_quit;
  archive_th = seh_create_thread(archive_trim_thread_fn, "archive-trim", &archive_args, NULL);
  {
    // "Off" is not unlimited: the trimmer falls back to a percentage of the
    // recordings drive's total capacity. Resolve it here too, so the effective
    // cap for this run is visible in the startup trace and any change in drive
    // size since the last run is recorded.
    double eff_gb = (double)archive_limit_gb;
    int is_auto = 0;
    if (archive_limit_gb <= 0) {
      uint64_t b = archive_auto_limit_bytes(archive_args.recordings_dir);
      eff_gb = (double)b / (1024.0 * 1024.0 * 1024.0);
      is_auto = 1;
    }
    startup_tracef(base_dir, "archive_trimmer_started ok=%d limit_gb=%d effective_gb=%.1f auto=%d",
                   archive_th ? 1 : 0, archive_limit_gb, eff_gb, is_auto);
  }
#endif

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_quality);
  SDL_ShowCursor(SDL_ENABLE);

#ifdef _WIN32
  // Cache/thumb loading is done asynchronously after the UI is shown (see startup_cache_load_thread_fn).
#endif

  char initial_title[256] = {0};
  snprintf(initial_title, sizeof(initial_title), "Ultralight IP CAM Viewer Recorder (build %s)", BUILD_ID);
  SDL_Window *window = SDL_CreateWindow(initial_title,
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        1280, 720,
                                        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                            SDL_WINDOW_MAXIMIZED);
  if (!window) die_sdl("SDL_CreateWindow failed");
  append_startup_marker(base_dir, "window_ok");
  startup_tracef(base_dir, "window_ok");
  tray_init(window); // X / minimize -> tray; tray menu Show / Exit
  Uint32 main_win_id = SDL_GetWindowID(window);

  t0 = SDL_GetTicks();
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) die_sdl("SDL_CreateRenderer failed");
  append_startup_marker(base_dir, "renderer_ok");
  startup_tracef(base_dir, "renderer_ok dt_ms=%u", (unsigned)(SDL_GetTicks() - t0));
#ifdef _WIN32
  SDL_AtomicSet(&main_heartbeat, (int)SDL_GetTicks());
#endif

  // Present immediately so the window becomes responsive even if background init is slow.
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  SDL_RenderPresent(renderer);
  SDL_PumpEvents();

  // Frame-buffer locking is now per-camera (CameraConfig.lock), created in
  // cameras_ensure_locks(). No single global camera mutex — each camera worker
  // and each UI tile only ever contend on their own camera's lock.
  append_startup_marker(base_dir, "cam_locks_ok");
  startup_tracef(base_dir, "cam_locks_ok");
#ifdef _WIN32
  SDL_AtomicSet(&main_heartbeat, (int)SDL_GetTicks());
#endif
#ifdef _WIN32
  SDL_Thread *startup_cache_th = NULL;
  SDL_atomic_t startup_cache_quit;
  SDL_AtomicSet(&startup_cache_quit, 0);
  // Liveness flag for the cache loader: the SEH wrapper sets it to 1 on entry and
  // 0 on exit. The main loop reaps the thread non-blocking once it finishes on its
  // own (guarded by startup_cache_seen_alive to avoid the brief create→entry race),
  // so by the time the user edits cameras the handle is already NULL and the
  // edit-path joins don't block the UI. Those joins remain as a correctness barrier
  // for the rare case where an edit lands while the loader is still running.
  SDL_atomic_t startup_cache_alive;
  SDL_AtomicSet(&startup_cache_alive, 0);
  int startup_cache_seen_alive = 0;
  StartupCacheLoadArgs startup_cache_args = {0};
  int startup_cache_pending_start = (argc < 2 && camera_count > 0) ? 1 : 0;
  if (startup_cache_pending_start) {
    startup_cache_args.cams = cameras;
    startup_cache_args.cam_count = camera_count;
    startup_cache_args.active_index = active_camera;
    snprintf(startup_cache_args.base_dir, sizeof(startup_cache_args.base_dir), "%s", base_dir);
    startup_cache_args.quit = &startup_cache_quit;
  }
#endif
  SDL_Thread **warm_threads = NULL;
  WarmCamArgs *warm_args = NULL;
  int warm_count = 0;
  int warm_spawn_next = 0;
  int warm_spawning = 0;
  SDL_atomic_t warm_quit;
  SDL_AtomicSet(&warm_quit, 0);
  SDL_Thread **tile_threads = NULL;
  TileStreamArgs *tile_args = NULL;
  int tile_count = 0;
  int tile_spawn_next = 0;
  int tile_spawning = 0;
  SDL_atomic_t tile_quit;
  SDL_AtomicSet(&tile_quit, 0);
  SDL_atomic_t tile_hires_index;
  SDL_AtomicSet(&tile_hires_index, -1);

// Every tile/warm worker holds a raw pointer to the camera array (args[i].cams)
// and reads live fields through it — record_enabled, title_utf8, the per-camera
// lock, the thumbnail buffers. So the array must never be mutated underneath
// them: cameras_add/cameras_delete realloc it (the block moves, every worker
// pointer dangles) and cameras_edit frees strings still in use. A worker that
// touches freed memory dies in the SEH handler, and if it was holding a camera
// lock at the time, the UI thread blocks on that lock forever — the app hangs
// with no crash dialog.
//
// Use before ANY mutation of `cameras`/`camera_count`. The existing
// stop+prepare that follows each edit then simply re-creates the workers.
// (main()-local: it names main()'s variables directly.)
#define STOP_CAMERA_WORKERS()                                            \
  do {                                                                   \
    tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);   \
    tile_threads = NULL;                                                 \
    tile_args = NULL;                                                    \
    tile_count = 0;                                                      \
    warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);      \
    warm_threads = NULL;                                                 \
    warm_args = NULL;                                                    \
    warm_count = 0;                                                      \
  } while (0)

  PluginHost plugins = {0};
  startup_tracef(base_dir, "plugins_init_begin");
  if (plugin_host_init(&plugins, settings_path) == 0) {
    // Load plugins now so ones enabled in settings.ini work from the first frame
    // (previously deferred until F10, which left enabled plugins dormant until the
    // user opened the overlay). Only DLLs in plugins\ are loaded; usually zero.
    plugin_host_rescan(&plugins);
  }
  startup_tracef(base_dir, "plugins_init_end");
#ifdef _WIN32
  SDL_AtomicSet(&main_heartbeat, (int)SDL_GetTicks());
#endif

  audio.mutex = SDL_CreateMutex();
  SDL_AtomicSet(&audio.ready, 0);
  SDL_AtomicSet(&audio.quit, 0);
  audio.dev = 0;
  memset(&audio.have, 0, sizeof(audio.have));
  audio.last_err[0] = 0;

  // The audio pipe must exist before the device opens — SDL is in callback
  // (pull) mode and will invoke audio_pipe_sdl_callback as soon as the device
  // is unpaused. The worker thread fills the pipe's internal ring; the SDL
  // audio thread drains it. This decouples device timing from packet arrival.
  if (audio_pipe_init(&audio_pipe, &audio, &plugins, &audio_volume_pct_atomic,
                      &audio_playing_sticky) == 0) {
    audio_worker_th = seh_create_thread(audio_worker_thread_fn, "audio-worker",
                                        audio_pipe, NULL);
    startup_tracef(base_dir, "audio_worker_started ok=%d", audio_worker_th ? 1 : 0);
  } else {
    startup_tracef(base_dir, "audio_pipe_init_failed");
  }

  {
    AudioInitArgs *aa = (AudioInitArgs *)SDL_malloc(sizeof(AudioInitArgs));
    if (aa) {
      memset(aa, 0, sizeof(*aa));
      aa->st = &audio;
      snprintf(aa->base_dir, sizeof(aa->base_dir), "%s", base_dir);
      aa->callback = audio_pipe ? audio_pipe_sdl_callback : NULL;
      aa->callback_userdata = audio_pipe;
      audio_th = seh_create_thread(audio_init_thread_fn, "audio-init", aa, NULL);
      startup_tracef(base_dir, "audio_thread_started ok=%d", audio_th ? 1 : 0);
      if (!audio_th) SDL_free(aa);
    } else {
      startup_tracef(base_dir, "audio_thread_started ok=0");
    }
  }

  VideoShared shared = {0};
  shared.mutex = SDL_CreateMutex();
  shared.cond = SDL_CreateCond();
  if (!shared.mutex || !shared.cond) die_sdl("SDL_CreateMutex/Cond failed");
  SDL_AtomicSet(&shared.quit, 0);
  startup_tracef(base_dir, "shared_mutex_ok");
#ifdef _WIN32
  SDL_AtomicSet(&main_heartbeat, (int)SDL_GetTicks());
#endif

  SDL_mutex *rec_mutex = SDL_CreateMutex();
  int rec_want = 0;
  char rec_title_buf[256] = {0};

  // Concurrent-open cap. Default is now 0 = fully parallel: every camera
  // connects independently, so offline cameras (e.g. after a power loss) cannot
  // queue-starve the ones that answer. What makes this safe where it once
  // overwhelmed NVRs is the hard per-open deadline (TILE_OPEN_DEADLINE_MS):
  // a dead camera's connect used to hold a slot for minutes; now every attempt
  // is bounded to seconds. If an NVR still chokes on simultaneous handshakes,
  // set [Streaming] MaxConcurrentOpens in settings.ini to restore the gate.
  {
    int max_opens = ini_get_int(settings_path, "Streaming", "MaxConcurrentOpens", 0);
    if (max_opens < 0) max_opens = 0;
    if (max_opens > 64) max_opens = 64;
    tile_stream_init_open_gate(max_opens);
    startup_tracef(base_dir, "open_gate max_concurrent=%d (0=parallel)", max_opens);
  }

  // Time machine (historical playback). Own thread; scans recordings + decodes.
  TimeMachine *tmach = time_machine_create(base_dir);
  UiTimeline timeline = {0};
  uint8_t *pb_buf = NULL;       // latest playback YUV420P frame (packed)
  int pb_cap = 0, pb_w = 0, pb_h = 0;
  uint32_t pb_serial = 0;
  SDL_Texture *pb_tex = NULL;
  int pb_tex_w = 0, pb_tex_h = 0;
  Uint32 tm_last_rescan = 0;
  int tm_cam = -1;  // camera the time machine is currently bound to
  const int TM_VIEW_MAX = 4096;
  TmSeg *tm_view = (TmSeg *)SDL_malloc(sizeof(TmSeg) * (size_t)TM_VIEW_MAX);
  if (argc < 2 && camera_count > 0) {
    rec_want = cameras[active_camera].record_enabled ? 1 : 0;
    snprintf(rec_title_buf, sizeof(rec_title_buf), "%s",
             cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
  }

  DecodeArgs dargs = {.url = url,
                      .shared = &shared,
                      .audio = &audio,
                      .audio_enabled = 1,
                      .plugins = &plugins,
                      .camera_index = active_camera,
                      .camera_title_utf8 = rec_title_buf,
                      .recordings_base_dir = base_dir,
                      .rec_mutex = rec_mutex,
                      .rec_want = &rec_want,
                      .rec_segment_seconds = rec_segment_seconds,
                      .initial_keyint_ms = expected_keyint_ms,
                      .audio_pipe = audio_pipe,
                      .audio_active_idx = &audio_active_idx_atomic};
  SDL_Thread *th = NULL;
  SDL_Thread *th_join = NULL;
  SDL_atomic_t th_join_done;
  SDL_AtomicSet(&th_join_done, 0);
  int th_join_rc = 0;
  int pending_main_start_idx = -1;
  // argc >= 2 is a URL straight from the command line: no camera list, so no
  // Enabled flag to honour.
  if (url && *url && (argc >= 2 || camera_streamable(cameras, camera_count, active_camera))) {
    th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", &dargs, &shared.alive);
    if (!th) die_sdl("SDL_CreateThread failed");
  } else if (url && *url) {
    // Disabled camera at startup: keep the thread unstarted. is_loading is
    // derived from `th` where it is declared below, so it lands on 0 by itself.
    active_url_buf[0] = 0;
    dargs.url = active_url_buf;
  }

  if (argc < 2) {
    if (camera_count > 0) {
      char title[256];
      snprintf(title, sizeof(title), "Ultralight IP CAM Viewer Recorder - %s",
               cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
      SDL_SetWindowTitle(window, title);
    }
  }

  SDL_Texture *texture = NULL;
  int tex_w = 0, tex_h = 0;
  AVFrame *tmp_yuv = av_frame_alloc();
  if (!tmp_yuv) {
    fprintf(stderr, "av_frame_alloc failed\n");
    SDL_AtomicSet(&shared.quit, 1);
    SDL_WaitThread(th, NULL);
    return 1;
  }

  uint8_t *frame_copy = NULL;
  int frame_copy_size = 0;

  SDL_Color text_color = {255, 255, 255, 255};
  const int text_pt = 16;
  CameraUiItem *camera_ui =
      (camera_count > 0) ? (CameraUiItem *)SDL_calloc((size_t)camera_count, sizeof(CameraUiItem)) : NULL;
  if (camera_count > 0 && !camera_ui) die_sdl("Failed to allocate camera UI state");

  if (argc < 2 && camera_count > 0) {
    if (preview_mode == PREVIEW_MODE_WARM) {
      warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
      warm_count = camera_count;
      warm_spawn_next = 0;
      warm_spawning = 1;
    } else if (preview_mode == PREVIEW_MODE_STREAM) {
      tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                           tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
      tile_count = camera_count;
      tile_spawn_next = 0;
      tile_spawning = 1;
#ifdef _WIN32
      if (tile_args) startup_trace_tile_sources(base_dir, tile_args, cameras, camera_count);
#endif
    }
  }

  // If we have a persisted preview cache, show it immediately for the initial camera.
  // This avoids a black screen while we wait for the first keyframe/IDR.
  if (argc < 2 && camera_count > 0) {
    CameraConfig *cam = &cameras[active_camera];
    if (cam->lock) SDL_LockMutex(cam->lock);
    int have_cached = (cam->last_yuv && cam->last_yuv_size > 0 && cam->last_w > 0 && cam->last_h > 0);
    if (have_cached) {
      int cw = cam->last_w;
      int ch = cam->last_h;
      if (!texture || tex_w != cw || tex_h != ch) {
        if (texture) SDL_DestroyTexture(texture);
        texture = recreate_texture(renderer, cw, ch);
        if (!texture) die_sdl("SDL_CreateTexture failed");
        tex_w = cw;
        tex_h = ch;
      }
      tmp_yuv->format = AV_PIX_FMT_YUV420P;
      tmp_yuv->width = cw;
      tmp_yuv->height = ch;
      int rc = av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize, cam->last_yuv, AV_PIX_FMT_YUV420P,
                                    cw, ch, 1);
      if (rc < 0) die_ffmpeg("av_image_fill_arrays failed", rc);
      if (SDL_UpdateYUVTexture(texture, NULL, tmp_yuv->data[0], tmp_yuv->linesize[0], tmp_yuv->data[1],
                               tmp_yuv->linesize[1], tmp_yuv->data[2], tmp_yuv->linesize[2]) != 0) {
        die_sdl("SDL_UpdateYUVTexture failed");
      }
    }
    if (cam->lock) SDL_UnlockMutex(cam->lock);
  }

  TextUiItem loading_text = {0};
  loading_text = make_text(renderer, "Буферизация", text_pt, text_color);
  if (!loading_text.tex) {
    fprintf(stderr, "Failed to create loading text texture.\n");
    return 1;
  }
  int buf_pt = (text_pt > 12) ? (text_pt - 6) : text_pt;
  if (buf_pt < 10) buf_pt = 10;
  TextUiItem loading_text_small = {0};
  loading_text_small = make_text(renderer, "Буферизация", buf_pt, text_color);
  if (!loading_text_small.tex) {
    fprintf(stderr, "Failed to create loading text texture.\n");
    return 1;
  }

  Uint32 overlay_until = 0;
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  int is_loading = (th != NULL) ? 1 : 0;
  Uint32 loading_since = SDL_GetTicks();
#ifdef _WIN32
  startup_tracef(base_dir,
                 "main_buffer_state initial is_loading=%d have_texture=%d preview_mode=%s tiles_use_preview_url=%d",
                 is_loading, (texture != NULL) ? 1 : 0, preview_mode_to_string(preview_mode), tiles_use_preview_url);
#endif
  int last_buffer_pct = -1;
  TextUiItem buffering_pct_text = {0};
  int matrix_last_buffer_pct = -1;
  TextUiItem matrix_buffering_pct_text = {0};
  // Big/expanded view: a substream picture (cached still or live tile thumb) is on
  // screen while the freshly started main-stream decode hasn't delivered a frame yet.
  // Drives the corner "Loading main stream" notice; cleared on the first main frame.
  int big_main_pending = 0;
  TextUiItem main_loading_text = {0};
  int main_loading_text_ver = -1;
  // Plugin feeding: displayed camera goes through the render paths (on_screen=1);
  // all other cameras' tile frames are fed in a throttled background pass so face
  // detection covers every camera. Scratch buffer reused across cameras.
  Uint32 last_display_feed_ticks = 0;
  uint8_t *plugin_feed_buf = NULL;
  int plugin_feed_cap = 0;
  int has_error = 0;
  TextUiItem error_text = {0};

  TextUiItem no_cameras_text = {0};

  // Phase 4: cached "NN%" label shown next to the speaker icon. Regenerated only on
  // volume change so we don't burn GDI text raster cycles per frame.
  TextUiItem volume_text = {0};
  int volume_text_pct = -1;

  double zoom = 1.0;
  double view_x = 0.0;
  double view_y = 0.0;
  int last_mouse_x = 0;
  int last_mouse_y = 0;
  int dragging = 0;
  int drag_last_x = 0;
  int drag_last_y = 0;
  int tile_scroll_px = 0;
  int matrix_expanded = -1; // -1 grid, >=0 single-camera view (active_camera)
  Uint32 matrix_last_click_ticks = 0;
  int matrix_last_click_idx = -1;
  int matrix_main_idx = -1; // active_camera index being buffered for main URL in grid mode
  int matrix_main_loading = 0;
  Uint32 matrix_main_loading_since = 0;

  Uint32 last_frame_ticks = SDL_GetTicks();
  Uint32 retry_at = 0;
  int retry_delay_ms = 1000;
  const Uint32 stall_ms = 5000;

  int is_fullscreen = 0;
  int prev_x = SDL_WINDOWPOS_CENTERED;
  int prev_y = SDL_WINDOWPOS_CENTERED;
  int prev_w = 1280;
  int prev_h = 720;
  int prev_was_maximized = 0;

  int stream_from_list = (argc < 2) ? 1 : 0;
  CameraManager mgr = {0};

  // In-flight ONVIF device probe (at most one; a re-save supersedes it) and the
  // device URL it was started for, used to find the target camera when it
  // completes (the list may have been edited meanwhile).
  OnvifProbe *onvif_probe_th = NULL;
  char onvif_probe_target[2048] = {0};

  // PTZ: worker thread for ONVIF pan/tilt/zoom commands, plus the on-screen pad
  // state. ptz_active_dir is the direction currently held down (-1 = none), so
  // a release can be matched to the press that started the move.
  PtzController *ptz = ptz_start();
  int ptz_active_dir = -1;
  SDL_Rect ptz_pad_rect = {0, 0, 0, 0};
  SDL_Rect ptz_btn_rects[6];
  memset(ptz_btn_rects, 0, sizeof(ptz_btn_rects));
  TextUiItem ptz_error_text = {0};
  char ptz_error_shown[256] = {0};
  SettingsOverlay settings = {0};
  IntroOverlay intro = {0};
  PluginsOverlay plugins_ui = {0};
  // Face-plugin indicator turns solid yellow while faces are being detected and stays
  // so for this long after the last detection (so a single hit is visible, not a blink).
  Uint32 face_flash_until_ms = 0;

  if (argc < 2 && view_mode == VIEW_MODE_MATRIX && camera_count == 1) {
    matrix_expanded = 0;
    active_camera = 0;
  }

  if (!intro_shown) {
    // Checkbox "Show at startup" is pre-ticked by default. Closing without
    // touching it keeps showing the banner; unticking opts out.
    intro_open(&intro, renderer, text_pt, text_color, intro_pref_show);
  }

#ifdef _WIN32
  int logged_main_loop_enter = 0;
  int logged_any_tile_buf_begin = 0;
  int logged_first_present = 0;
#endif

  int running = 1;
  while (running) {
#ifdef _WIN32
    SDL_AtomicSet(&main_heartbeat, (int)SDL_GetTicks());
#endif

#ifdef _WIN32
    if (!logged_main_loop_enter) {
      startup_tracef(base_dir, "main_loop_enter");
      logged_main_loop_enter = 1;
    }
#endif
    Uint32 loop_ticks = SDL_GetTicks();

    // Background ONVIF probe finished? Fill the target camera's stream URLs and
    // metadata, persist, and restart the affected pipelines. The target is
    // matched by device URL because the list may have been edited meanwhile.
    if (onvif_probe_th) {
      OnvifInfo oinfo;
      if (onvif_probe_poll(onvif_probe_th, &oinfo) != 0) {
        onvif_probe_destroy(onvif_probe_th);
        onvif_probe_th = NULL;
        int oidx = -1;
        for (int i = 0; i < camera_count; i++) {
          if (cameras[i].type == CAMERA_TYPE_ONVIF && cameras[i].onvif_device_url &&
              strcmp(cameras[i].onvif_device_url, onvif_probe_target) == 0) {
            oidx = i;
            break;
          }
        }
        if (oinfo.ok && oidx >= 0) {
#ifdef _WIN32
          if (startup_cache_th) {
            SDL_AtomicSet(&startup_cache_quit, 1);
            SDL_WaitThread(startup_cache_th, NULL);
            startup_cache_th = NULL;
            SDL_AtomicSet(&startup_cache_quit, 0);
          }
#endif
          // Replaces url/preview_url strings in place; stop the workers first
          // for the same reason the camera-manager edits do (see
          // STOP_CAMERA_WORKERS). They are re-prepared further down.
          STOP_CAMERA_WORKERS();
          CameraConfig *ocam = &cameras[oidx];
          char *nu = str_dup(oinfo.main_uri);
          if (nu) {
            SDL_free(ocam->url);
            ocam->url = nu;
          }
          if (oinfo.sub_uri[0]) {
            char *np = str_dup(oinfo.sub_uri);
            if (np) {
              SDL_free(ocam->preview_url);
              ocam->preview_url = np;
            }
          }
          camera_set_onvif_meta(ocam, oinfo.name, oinfo.manufacturer, oinfo.model,
                                oinfo.firmware, oinfo.serial, oinfo.ip, oinfo.mac,
                                oinfo.has_ptz);
          // A device that reports PTZ configures itself: endpoint, credentials
          // and profile token all come from the probe, so the pad works with no
          // extra typing. A device without PTZ has its stale config cleared.
          if (oinfo.has_ptz && oinfo.ptz_url[0]) {
            camera_set_ptz(ocam, 1, oinfo.ptz_url, oinfo.user, oinfo.pass, oinfo.profile_token);
          } else {
            camera_set_ptz(ocam, 0, NULL, NULL, NULL, NULL);
          }

          int need_restart = (stream_from_list && oidx == active_camera) ? 1 : 0;
          if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                    camera_count, &camera_ui, camera_count, &mgr, &active_camera,
                                    stream_from_list, need_restart, window, &shared, &th, &dargs,
                                    &audio, &error_text, &has_error, &is_loading, &loading_since,
                                    &zoom, &view_x, &view_y, &dragging, &last_frame_ticks,
                                    &retry_at, &retry_delay_ms, active_url_buf,
                                    sizeof(active_url_buf), rec_mutex, &rec_want, rec_title_buf,
                                    sizeof(rec_title_buf)) != 0) {
            die_sdl("Failed to apply camera list changes");
          }
          destroy_text(&buffering_pct_text);
          last_buffer_pct = -1;

#ifdef _WIN32
          if (argc < 2 && camera_count > 0) {
            startup_cache_args.cams = cameras;
            startup_cache_args.cam_count = camera_count;
            startup_cache_args.active_index = active_camera;
            snprintf(startup_cache_args.base_dir, sizeof(startup_cache_args.base_dir), "%s",
                     base_dir);
            startup_cache_args.quit = &startup_cache_quit;
            startup_cache_th = seh_create_thread(startup_cache_load_thread_fn, "cache-load",
                                                 &startup_cache_args, &startup_cache_alive);
            startup_cache_seen_alive = 0;
          }
#endif

          if (argc < 2) {
            if (preview_mode == PREVIEW_MODE_WARM) {
              warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
              warm_threads = NULL;
              warm_args = NULL;
              warm_count = 0;
              if (camera_count > 0) {
                warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
                warm_count = camera_count;
                warm_spawn_next = 0;
                warm_spawning = 1;
              }
            } else if (preview_mode == PREVIEW_MODE_STREAM) {
              tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
              tile_threads = NULL;
              tile_args = NULL;
              tile_count = 0;
              if (camera_count > 0) {
                tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                     tiles_use_preview_url, &tile_hires_index, base_dir,
                                     base_dir, rec_mutex,
                                     audio_pipe, &audio_active_idx_atomic);
                tile_count = camera_count;
                tile_spawn_next = 0;
                tile_spawning = 1;
              }
            }
          }

          // If the user is editing this very camera right now, refresh the URL
          // fields in place so the discovered streams show up immediately.
          if (mgr.is_open && mgr.mode == 1 && mgr.edit_index == oidx) {
            snprintf(mgr.edit_url, sizeof(mgr.edit_url), "%s", ocam->url ? ocam->url : "");
            snprintf(mgr.edit_preview_url, sizeof(mgr.edit_preview_url), "%s",
                     ocam->preview_url ? ocam->preview_url : "");
            mgr.edit_ptz_on = ocam->ptz_enabled ? 1 : 0;
            snprintf(mgr.edit_ptz_url, sizeof(mgr.edit_ptz_url), "%s",
                     ocam->ptz_url ? ocam->ptz_url : "");
            snprintf(mgr.edit_ptz_user, sizeof(mgr.edit_ptz_user), "%s",
                     ocam->ptz_user ? ocam->ptz_user : "");
            snprintf(mgr.edit_ptz_pass, sizeof(mgr.edit_ptz_pass), "%s",
                     ocam->ptz_pass ? ocam->ptz_pass : "");
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
          if (mgr.is_open) {
            char sbuf[320];
            snprintf(sbuf, sizeof(sbuf), "%s (%s %s)", tr(STR_ONVIF_OK),
                     oinfo.manufacturer[0] ? oinfo.manufacturer : "?",
                     oinfo.model[0] ? oinfo.model : "?");
            camera_manager_set_status(&mgr, sbuf, text_pt, text_color);
          }
        } else if (mgr.is_open && !oinfo.ok) {
          // (A successful probe whose target camera was deleted or re-typed
          // meanwhile is dropped silently.)
          char sbuf[320];
          snprintf(sbuf, sizeof(sbuf), "%s%s", tr(STR_ONVIF_FAILED),
                   oinfo.error[0] ? oinfo.error : "?");
          camera_manager_set_status(&mgr, sbuf, text_pt, text_color);
        }
      }
    }

    int stream_tiles_use_main = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = 0;

      // A press that started a PTZ move must always be able to end it, so the
      // release is handled before any overlay branch can swallow the event and
      // without the window-id filter — losing focus mid-move counts as release.
      if (ptz_active_dir >= 0) {
        int release = 0;
        if (e.type == SDL_KEYUP) {
          SDL_Keycode k = e.key.keysym.sym;
          release = (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT ||
                     k == SDLK_PAGEUP || k == SDLK_PAGEDOWN);
        } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
          release = 1;
        } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
          // Losing focus swallows the key-up. (Not SDL_WINDOWEVENT_LEAVE: the
          // pointer drifting off-window must not interrupt a keyboard hold.)
          release = 1;
        }
        if (release) {
          ptz_release(ptz);
          ptz_active_dir = -1;
          // Key-ups have no other consumer; mouse-ups still need to clear drag.
          if (e.type == SDL_KEYUP) continue;
        }
      }

      if (intro.is_open) {
        int should_close = 0;
        if (e.type == SDL_KEYDOWN) {
          if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN ||
              e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_F1) {
            should_close = 1;
          }
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
          // A click on the "Show at startup" checkbox toggles the tick without
          // closing the intro. Any other click closes the intro.
          int mx = e.button.x;
          int my = e.button.y;
          if (intro.chk_rect_w > 0 && intro.chk_rect_h > 0 &&
              mx >= intro.chk_rect_x && mx < intro.chk_rect_x + intro.chk_rect_w &&
              my >= intro.chk_rect_y && my < intro.chk_rect_y + intro.chk_rect_h) {
            intro.show_at_startup = intro.show_at_startup ? 0 : 1;
          } else {
            should_close = 1;
          }
        }
        if (should_close) {
          int show_next = intro.show_at_startup;
          intro_pref_show = show_next ? 1 : 0;
          intro_close(&intro);
#ifdef _WIN32
          if (settings_path[0]) {
            // Tick state is "show at startup" — persisted off the UI thread.
            persist_set_int(g_persist, settings_path, "App", "IntroAtStartup", show_next ? 1 : 0);
          }
#else
          (void)show_next;
#endif
        }
        continue;
      }
      if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_F1) {
          // Help / hints overlay (the same one shown once at first boot).
          if (settings.is_open) settings_close(&settings);
          if (mgr.is_open) camera_manager_close(&mgr);
          if (plugins_ui.is_open) plugins_overlay_close(&plugins_ui);
          intro_open(&intro, renderer, text_pt, text_color, intro_pref_show);
          continue;
        }

        if (e.key.keysym.sym == SDLK_F2) {
          if (settings.is_open) {
            settings_close(&settings);
          } else {
            if (mgr.is_open) camera_manager_close(&mgr);
            if (plugins_ui.is_open) plugins_overlay_close(&plugins_ui);
            settings_open(&settings, renderer, scale_quality, view_mode, preview_mode, tiles_use_preview_url,
                          archive_limit_gb, text_pt, text_color);
          }
          continue;
        }

        // F9: toggle the time machine strip for the selected camera. Opening it
        // binds the (async) playback engine to the active camera and rescans its
        // recordings; clicking the strip starts historical playback.
        if (e.key.keysym.sym == SDLK_F9) {
          if (camera_count <= 0 || active_camera < 0) continue;
          timeline.open = !timeline.open;
          if (timeline.open) {
            if (settings.is_open) settings_close(&settings);
            if (mgr.is_open) camera_manager_close(&mgr);
            if (plugins_ui.is_open) plugins_overlay_close(&plugins_ui);
            time_machine_set_camera(tmach, cameras[active_camera].title_utf8);
            tm_cam = active_camera;
            timeline.initialized = 0;
            time_machine_request_rescan(tmach);
            tm_last_rescan = SDL_GetTicks();
          } else {
            time_machine_set_playing(tmach, 0);
          }
          continue;
        }
        // Esc closes the strip (if no modal overlay claims it first).
        if (e.key.keysym.sym == SDLK_ESCAPE && timeline.open && !settings.is_open &&
            !mgr.is_open && !plugins_ui.is_open && !intro.is_open) {
          timeline.open = 0;
          time_machine_set_playing(tmach, 0);
          continue;
        }
        // Space toggles play/pause while the strip is open.
        if (e.key.keysym.sym == SDLK_SPACE && timeline.open && !settings.is_open && !mgr.is_open) {
          time_machine_set_playing(tmach, !time_machine_is_playing(tmach));
          continue;
        }

        if (settings.is_open) {
          if (e.key.keysym.sym == SDLK_ESCAPE) {
            settings_close(&settings);
            continue;
          }
          if (e.key.keysym.sym == SDLK_UP) {
            settings.selected--;
            int max_sel = (settings.pending_preview_mode == PREVIEW_MODE_STREAM) ? 3 : 2;
            if (settings.selected < 0) settings.selected = max_sel;
            continue;
          }
          if (e.key.keysym.sym == SDLK_DOWN) {
            settings.selected++;
            int max_sel = (settings.pending_preview_mode == PREVIEW_MODE_STREAM) ? 3 : 2;
            if (settings.selected > max_sel) settings.selected = 0;
            continue;
          }
          if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE) {
            if (settings.selected == 0) {
              // Cycle render scale quality.
              if (SDL_strcasecmp(settings.pending_quality, "nearest") == 0) {
                snprintf(settings.pending_quality, sizeof(settings.pending_quality), "linear");
              } else if (SDL_strcasecmp(settings.pending_quality, "linear") == 0) {
                snprintf(settings.pending_quality, sizeof(settings.pending_quality), "best");
              } else {
                snprintf(settings.pending_quality, sizeof(settings.pending_quality), "nearest");
              }
            } else if (settings.selected == 1) {
              settings.pending_view_mode =
                  (settings.pending_view_mode == VIEW_MODE_TILES) ? VIEW_MODE_MATRIX : VIEW_MODE_TILES;
            } else if (settings.selected == 2) {
              settings.pending_preview_mode =
                  (PreviewMode)(((int)settings.pending_preview_mode + 1) % 3);
            } else if (settings.selected == 3) {
              settings.pending_tiles_use_preview_url = settings.pending_tiles_use_preview_url ? 0 : 1;
            }
            continue;
          }
            if ((e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.sym == SDLK_s)) {
            if (settings_save_to_ini(settings_path, &settings) == 0) {
              // i18n: re-render main-view localizable text in the now-current language.
              destroy_text(&no_cameras_text);
              if (error_text.tex) { SDL_DestroyTexture(error_text.tex); error_text.tex = NULL; has_error = 0; }
              snprintf(scale_quality, sizeof(scale_quality), "%s",
                       normalize_scale_quality(settings.pending_quality));
              SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_quality);

              // Apply the recording archive limit live for the trimmer thread.
              archive_limit_gb = settings.pending_archive_gb;
#ifdef _WIN32
              SDL_AtomicSet(&archive_limit_gb_atomic, archive_limit_gb);
#endif

              ViewMode prev_mode = view_mode;
              view_mode = settings.pending_view_mode;
              if (argc < 2 && prev_mode != view_mode) {
                if (view_mode == VIEW_MODE_MATRIX && camera_count > 1) {
                  matrix_expanded = -1;
                  stop_decode_thread(&shared, &th);
                  audio_state_clear_queue(&audio);
                  active_url_buf[0] = 0;
                  dargs.url = active_url_buf;
                  is_loading = 0;
                  retry_at = 0;
                  retry_delay_ms = 1000;
                  dragging = 0;
                  zoom = 1.0;
                  view_x = 0.0;
                  view_y = 0.0;
                  if (error_text.tex) {
                    SDL_DestroyTexture(error_text.tex);
                    error_text.tex = NULL;
                  }
                  has_error = 0;
                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;
                } else if (view_mode == VIEW_MODE_MATRIX && camera_count == 1) {
                  matrix_expanded = 0;
                  active_camera = 0;
                  if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                            camera_count, &camera_ui, camera_count, &mgr, &active_camera,
                                            stream_from_list, 1, window, &shared, &th, &dargs, &audio,
                                            &error_text, &has_error, &is_loading, &loading_since, &zoom,
                                            &view_x, &view_y, &dragging, &last_frame_ticks, &retry_at,
                                            &retry_delay_ms, active_url_buf, sizeof(active_url_buf),
                                            rec_mutex, &rec_want, rec_title_buf,
                                            sizeof(rec_title_buf)) != 0) {
                    die_sdl("Failed to apply view mode change");
                  }
                } else if (view_mode == VIEW_MODE_TILES) {
                  matrix_expanded = -1;
                  if (camera_count > 0) {
                    if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                              camera_count, &camera_ui, camera_count, &mgr, &active_camera,
                                              stream_from_list, 1, window, &shared, &th, &dargs, &audio,
                                              &error_text, &has_error, &is_loading, &loading_since, &zoom,
                                              &view_x, &view_y, &dragging, &last_frame_ticks, &retry_at,
                                              &retry_delay_ms, active_url_buf, sizeof(active_url_buf),
                                              rec_mutex, &rec_want, rec_title_buf,
                                              sizeof(rec_title_buf)) != 0) {
                      die_sdl("Failed to apply view mode change");
                    }
                  }
                }
              }

              int prev_tiles_use_preview_url = tiles_use_preview_url;
              tiles_use_preview_url = settings.pending_tiles_use_preview_url ? 1 : 0;

              PreviewMode prev = preview_mode;
              preview_mode = settings.pending_preview_mode;
              if (argc < 2 && prev != preview_mode) {
                if (prev == PREVIEW_MODE_WARM) {
                  warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
                  warm_threads = NULL;
                  warm_args = NULL;
                  warm_count = 0;
                }
                if (prev == PREVIEW_MODE_STREAM) {
                  tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
                  tile_threads = NULL;
                  tile_args = NULL;
                  tile_count = 0;
                }

                if (preview_mode == PREVIEW_MODE_WARM) {
                  warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
                  warm_count = camera_count;
                  warm_spawn_next = 0;
                  warm_spawning = 1;
                } else if (preview_mode == PREVIEW_MODE_STREAM) {
                  tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                       tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                  tile_count = camera_count;
                  tile_spawn_next = 0;
                  tile_spawning = 1;
                }
              } else if (argc < 2 && preview_mode == PREVIEW_MODE_STREAM &&
                         prev_tiles_use_preview_url != tiles_use_preview_url) {
                tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
                tile_threads = NULL;
                tile_args = NULL;
                tile_count = 0;
                if (camera_count > 0) {
                  tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                       tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                  tile_count = camera_count;
                  tile_spawn_next = 0;
                  tile_spawning = 1;
#ifdef _WIN32
                  if (tile_args) startup_trace_tile_sources(base_dir, tile_args, cameras, camera_count);
#endif
                }

                if (view_mode == VIEW_MODE_MATRIX && matrix_expanded < 0 && tiles_use_preview_url == 0 &&
                    matrix_main_idx >= 0) {
                  pending_main_start_idx = -1;
                  if (th) request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
                  audio_state_clear_queue(&audio);
                  active_url_buf[0] = 0;
                  dargs.url = active_url_buf;
                  matrix_main_idx = -1;
                  matrix_main_loading = 0;
                  retry_at = 0;
                  retry_delay_ms = 1000;
                  is_loading = 0;
                  if (error_text.tex) {
                    SDL_DestroyTexture(error_text.tex);
                    error_text.tex = NULL;
                  }
                  has_error = 0;
                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;
                }
              }
            }
            settings_close(&settings);
            continue;
          }
        }

        if (e.key.keysym.sym == SDLK_F10) {
          if (plugins_ui.is_open) {
            plugins_overlay_close(&plugins_ui);
          } else {
            if (settings.is_open) settings_close(&settings);
            if (mgr.is_open) camera_manager_close(&mgr);
            plugin_host_rescan(&plugins);
            plugins_overlay_open(&plugins_ui, renderer, &plugins, text_pt, text_color);
          }
          continue;
        }

        if (plugins_ui.is_open) {
          if (e.key.keysym.sym == SDLK_ESCAPE) {
            plugins_overlay_close(&plugins_ui);
            continue;
          }
          if (e.key.keysym.sym == SDLK_UP) {
            if (plugins_ui.selected > 0) plugins_ui.selected--;
            continue;
          }
          if (e.key.keysym.sym == SDLK_DOWN) {
            if (plugins_ui.selected < plugins_ui.list_count - 1) plugins_ui.selected++;
            continue;
          }
          if (e.key.keysym.sym == SDLK_r) {
            plugin_host_rescan(&plugins);
            plugins_overlay_rebuild_list(&plugins_ui, renderer, &plugins, text_pt, text_color);
            continue;
          }
          if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE) {
            int idx = plugins_ui.selected;
            int enabled = 0;
            if (plugins.mutex) SDL_LockMutex(plugins.mutex);
            if (idx >= 0 && idx < plugins.row_count) enabled = plugins.rows[idx].enabled ? 1 : 0;
            if (plugins.mutex) SDL_UnlockMutex(plugins.mutex);
            plugin_host_set_enabled(&plugins, idx, !enabled);
            plugins_overlay_rebuild_list(&plugins_ui, renderer, &plugins, text_pt, text_color);
            continue;
          }
        }

        if (e.key.keysym.sym == SDLK_F3) {
          if (settings.is_open) continue;
          if (plugins_ui.is_open) continue;
          if (mgr.is_open) {
            camera_manager_close(&mgr);
          } else {
            if (camera_manager_open(&mgr, window, renderer, active_camera, cameras, camera_count,
                                    text_pt, text_color) != 0) {
              fprintf(stderr, "Failed to open camera manager overlay.\n");
            }
          }
          continue;
        }

        if (mgr.is_open) {
          int changed = 0;
          int need_restart = 0;
          int old_count = camera_count;

          if (mgr.mode == 0) {
            if (e.key.keysym.sym == SDLK_ESCAPE) {
              camera_manager_close(&mgr);
              continue;
            }
            if (e.key.keysym.sym == SDLK_UP) {
              if (mgr.selected > 0) mgr.selected--;
              continue;
            }
            if (e.key.keysym.sym == SDLK_DOWN) {
              if (mgr.selected < camera_count - 1) mgr.selected++;
              continue;
            }
            if (e.key.keysym.sym == SDLK_a) {
              camera_manager_begin_edit(&mgr, -1, cameras, camera_count, text_pt, text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_e || e.key.keysym.sym == SDLK_RETURN) {
              camera_manager_begin_edit(&mgr, mgr.selected, cameras, camera_count, text_pt,
                                        text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_DELETE) {
              int del_idx = mgr.selected;
#ifdef _WIN32
              if (startup_cache_th) {
                SDL_AtomicSet(&startup_cache_quit, 1);
                SDL_WaitThread(startup_cache_th, NULL);
                startup_cache_th = NULL;
                SDL_AtomicSet(&startup_cache_quit, 0);
              }
#endif
              STOP_CAMERA_WORKERS();
              if (cameras_delete(&cameras, &camera_count, del_idx) == 0) {
                changed = 1;
                if (del_idx < active_camera) active_camera--;
                if (del_idx == active_camera) {
                  if (active_camera >= camera_count) active_camera = camera_count - 1;
                  need_restart = stream_from_list ? 1 : 0;
                }
                if (mgr.selected >= camera_count) mgr.selected = camera_count - 1;
              }
            }
          } else {
            if (e.key.keysym.sym == SDLK_ESCAPE) {
              mgr.mode = 0;
              mgr.edit_index = -1;
              SDL_StopTextInput();
              continue;
            }
            if (e.key.keysym.sym == SDLK_TAB) {
              mgr.edit_field =
                  camera_manager_next_field(mgr.edit_type, mgr.edit_ptz_on, mgr.edit_field);
              continue;
            }
            if (e.key.keysym.sym == SDLK_SPACE && mgr.edit_field == CAMMGR_FIELD_TYPE) {
              mgr.edit_type = camera_manager_cycle_type(mgr.edit_type);
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_SPACE && mgr.edit_field == CAMMGR_FIELD_PTZ_ON) {
              mgr.edit_ptz_on = !mgr.edit_ptz_on;
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_SPACE && mgr.edit_field == CAMMGR_FIELD_ENABLED) {
              mgr.edit_enabled = !mgr.edit_enabled;
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_LEFT) {
              if (mgr.edit_field == 1) {
                mgr.title_cursor = camui_utf8_prev(mgr.edit_title, mgr.title_cursor);
                mgr.title_cursor_px = camui_measure_px(text_pt, mgr.edit_title, mgr.title_cursor);
              } else if (mgr.edit_field == 2) {
                mgr.url_cursor = camui_utf8_prev(mgr.edit_url, mgr.url_cursor);
                mgr.url_cursor_px = camui_measure_px(text_pt, mgr.edit_url, mgr.url_cursor);
              } else if (mgr.edit_field == 3) {
                mgr.preview_cursor = camui_utf8_prev(mgr.edit_preview_url, mgr.preview_cursor);
                mgr.preview_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_preview_url, mgr.preview_cursor);
              } else if (mgr.edit_field == 4) {
                mgr.onvif_cursor = camui_utf8_prev(mgr.edit_onvif_url, mgr.onvif_cursor);
                mgr.onvif_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_onvif_url, mgr.onvif_cursor);
              } else if (mgr.edit_field == 6) {
                mgr.ptz_url_cursor = camui_utf8_prev(mgr.edit_ptz_url, mgr.ptz_url_cursor);
                mgr.ptz_url_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_ptz_url, mgr.ptz_url_cursor);
              } else if (mgr.edit_field == 7) {
                mgr.ptz_user_cursor = camui_utf8_prev(mgr.edit_ptz_user, mgr.ptz_user_cursor);
                mgr.ptz_user_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_ptz_user, mgr.ptz_user_cursor);
              } else if (mgr.edit_field == 8) {
                if (mgr.ptz_pass_cursor > 0) mgr.ptz_pass_cursor--;
                camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              }
              continue;
            }
            if (e.key.keysym.sym == SDLK_RIGHT) {
              if (mgr.edit_field == 1) {
                mgr.title_cursor = camui_utf8_next(mgr.edit_title, mgr.title_cursor);
                mgr.title_cursor_px = camui_measure_px(text_pt, mgr.edit_title, mgr.title_cursor);
              } else if (mgr.edit_field == 2) {
                mgr.url_cursor = camui_utf8_next(mgr.edit_url, mgr.url_cursor);
                mgr.url_cursor_px = camui_measure_px(text_pt, mgr.edit_url, mgr.url_cursor);
              } else if (mgr.edit_field == 3) {
                mgr.preview_cursor = camui_utf8_next(mgr.edit_preview_url, mgr.preview_cursor);
                mgr.preview_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_preview_url, mgr.preview_cursor);
              } else if (mgr.edit_field == 4) {
                mgr.onvif_cursor = camui_utf8_next(mgr.edit_onvif_url, mgr.onvif_cursor);
                mgr.onvif_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_onvif_url, mgr.onvif_cursor);
              } else if (mgr.edit_field == 6) {
                mgr.ptz_url_cursor = camui_utf8_next(mgr.edit_ptz_url, mgr.ptz_url_cursor);
                mgr.ptz_url_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_ptz_url, mgr.ptz_url_cursor);
              } else if (mgr.edit_field == 7) {
                mgr.ptz_user_cursor = camui_utf8_next(mgr.edit_ptz_user, mgr.ptz_user_cursor);
                mgr.ptz_user_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_ptz_user, mgr.ptz_user_cursor);
              } else if (mgr.edit_field == 8) {
                if (mgr.ptz_pass_cursor < (int)strlen(mgr.edit_ptz_pass)) mgr.ptz_pass_cursor++;
                camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              }
              continue;
            }
            if (e.key.keysym.sym == SDLK_HOME) {
              if (mgr.edit_field == 1) {
                mgr.title_cursor = 0;
                mgr.title_cursor_px = 0;
              } else if (mgr.edit_field == 2) {
                mgr.url_cursor = 0;
                mgr.url_cursor_px = 0;
              } else if (mgr.edit_field == 3) {
                mgr.preview_cursor = 0;
                mgr.preview_cursor_px = 0;
              } else if (mgr.edit_field == 4) {
                mgr.onvif_cursor = 0;
                mgr.onvif_cursor_px = 0;
              } else if (mgr.edit_field == 6) {
                mgr.ptz_url_cursor = 0;
                mgr.ptz_url_cursor_px = 0;
              } else if (mgr.edit_field == 7) {
                mgr.ptz_user_cursor = 0;
                mgr.ptz_user_cursor_px = 0;
              } else if (mgr.edit_field == 8) {
                mgr.ptz_pass_cursor = 0;
                mgr.ptz_pass_cursor_px = 0;
              }
              continue;
            }
            if (e.key.keysym.sym == SDLK_END) {
              if (mgr.edit_field == 1) {
                mgr.title_cursor = (int)strlen(mgr.edit_title);
                mgr.title_cursor_px = camui_measure_px(text_pt, mgr.edit_title, mgr.title_cursor);
              } else if (mgr.edit_field == 2) {
                mgr.url_cursor = (int)strlen(mgr.edit_url);
                mgr.url_cursor_px = camui_measure_px(text_pt, mgr.edit_url, mgr.url_cursor);
              } else if (mgr.edit_field == 3) {
                mgr.preview_cursor = (int)strlen(mgr.edit_preview_url);
                mgr.preview_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_preview_url, mgr.preview_cursor);
              } else if (mgr.edit_field == 4) {
                mgr.onvif_cursor = (int)strlen(mgr.edit_onvif_url);
                mgr.onvif_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_onvif_url, mgr.onvif_cursor);
              } else if (mgr.edit_field == 6) {
                mgr.ptz_url_cursor = (int)strlen(mgr.edit_ptz_url);
                mgr.ptz_url_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_ptz_url, mgr.ptz_url_cursor);
              } else if (mgr.edit_field == 7) {
                mgr.ptz_user_cursor = (int)strlen(mgr.edit_ptz_user);
                mgr.ptz_user_cursor_px =
                    camui_measure_px(text_pt, mgr.edit_ptz_user, mgr.ptz_user_cursor);
              } else if (mgr.edit_field == 8) {
                mgr.ptz_pass_cursor = (int)strlen(mgr.edit_ptz_pass);
                camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              }
              continue;
            }
            if ((e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.sym == SDLK_c)) {
              // Copy the focused field's full contents to the clipboard (there is
              // no text selection model here, so this copies the whole field).
              const char *src = NULL;
              if (mgr.edit_field == 1) src = mgr.edit_title;
              else if (mgr.edit_field == 2) src = mgr.edit_url;
              else if (mgr.edit_field == 3) src = mgr.edit_preview_url;
              else if (mgr.edit_field == 4) src = mgr.edit_onvif_url;
              else if (mgr.edit_field == 6) src = mgr.edit_ptz_url;
              else if (mgr.edit_field == 7) src = mgr.edit_ptz_user;
              // Field 8 (password) is deliberately not copyable.
              if (src && *src) SDL_SetClipboardText(src);
              continue;
            }
            if ((e.key.keysym.mod & KMOD_CTRL) && (e.key.keysym.sym == SDLK_v)) {
              char *clip = SDL_GetClipboardText();
              if (clip && *clip) {
                if (mgr.edit_field == 1) {
                  camui_insert_at(mgr.edit_title, sizeof(mgr.edit_title), &mgr.title_cursor, text_pt,
                                 clip);
                } else if (mgr.edit_field == 2) {
                  camui_insert_at(mgr.edit_url, sizeof(mgr.edit_url), &mgr.url_cursor, text_pt, clip);
                } else if (mgr.edit_field == 3) {
                  camui_insert_at(mgr.edit_preview_url, sizeof(mgr.edit_preview_url), &mgr.preview_cursor,
                                 text_pt, clip);
                } else if (mgr.edit_field == 4) {
                  camui_insert_at(mgr.edit_onvif_url, sizeof(mgr.edit_onvif_url), &mgr.onvif_cursor,
                                 text_pt, clip);
                } else if (mgr.edit_field == 6) {
                  camui_insert_at(mgr.edit_ptz_url, sizeof(mgr.edit_ptz_url), &mgr.ptz_url_cursor,
                                 text_pt, clip);
                } else if (mgr.edit_field == 7) {
                  camui_insert_at(mgr.edit_ptz_user, sizeof(mgr.edit_ptz_user),
                                 &mgr.ptz_user_cursor, text_pt, clip);
                } else if (mgr.edit_field == 8) {
                  camui_insert_at(mgr.edit_ptz_pass, sizeof(mgr.edit_ptz_pass),
                                 &mgr.ptz_pass_cursor, text_pt, clip);
                }
                camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              }
              if (clip) SDL_free(clip);
              continue;
            }
            if (e.key.keysym.sym == SDLK_BACKSPACE) {
              if (mgr.edit_field == 1) camui_backspace_at(mgr.edit_title, &mgr.title_cursor);
              if (mgr.edit_field == 2) camui_backspace_at(mgr.edit_url, &mgr.url_cursor);
              if (mgr.edit_field == 3) camui_backspace_at(mgr.edit_preview_url, &mgr.preview_cursor);
              if (mgr.edit_field == 4) camui_backspace_at(mgr.edit_onvif_url, &mgr.onvif_cursor);
              if (mgr.edit_field == 6) camui_backspace_at(mgr.edit_ptz_url, &mgr.ptz_url_cursor);
              if (mgr.edit_field == 7) camui_backspace_at(mgr.edit_ptz_user, &mgr.ptz_user_cursor);
              if (mgr.edit_field == 8) camui_backspace_at(mgr.edit_ptz_pass, &mgr.ptz_pass_cursor);
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_DELETE) {
              if (mgr.edit_field == 1) camui_delete_at(mgr.edit_title, &mgr.title_cursor);
              if (mgr.edit_field == 2) camui_delete_at(mgr.edit_url, &mgr.url_cursor);
              if (mgr.edit_field == 3) camui_delete_at(mgr.edit_preview_url, &mgr.preview_cursor);
              if (mgr.edit_field == 4) camui_delete_at(mgr.edit_onvif_url, &mgr.onvif_cursor);
              if (mgr.edit_field == 6) camui_delete_at(mgr.edit_ptz_url, &mgr.ptz_url_cursor);
              if (mgr.edit_field == 7) camui_delete_at(mgr.edit_ptz_user, &mgr.ptz_user_cursor);
              if (mgr.edit_field == 8) camui_delete_at(mgr.edit_ptz_pass, &mgr.ptz_pass_cursor);
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (e.key.keysym.sym == SDLK_RETURN) {
              // An ONVIF camera may be saved with only its device URL — it then
              // serves as the stream-URL placeholder until the probe fills them.
              const char *mgr_save_url =
                  mgr.edit_url[0] ? mgr.edit_url
                  : (mgr.edit_type == CAMERA_TYPE_ONVIF) ? mgr.edit_onvif_url
                                                         : "";
              if (mgr.edit_title[0] && mgr_save_url[0]) {
#ifdef _WIN32
                if (startup_cache_th) {
                  SDL_AtomicSet(&startup_cache_quit, 1);
                  SDL_WaitThread(startup_cache_th, NULL);
                  startup_cache_th = NULL;
                  SDL_AtomicSet(&startup_cache_quit, 0);
                }
#endif
                STOP_CAMERA_WORKERS();
                if (mgr.edit_index < 0) {
                  if (cameras_add(&cameras, &camera_count, mgr.edit_type, mgr.edit_title,
                                  mgr_save_url, mgr.edit_preview_url) == 0) {
                    cameras_ensure_locks(cameras, camera_count);
                    changed = 1;
                    mgr.selected = camera_count - 1;
                  }
                } else {
                  if (cameras_edit(cameras, camera_count, mgr.edit_index, mgr.edit_type,
                                   mgr.edit_title, mgr_save_url, mgr.edit_preview_url) == 0) {
                    changed = 1;
                    if (mgr.edit_index == active_camera) need_restart = stream_from_list ? 1 : 0;
                  }
                }
                if (changed) {
                  int saved_idx = (mgr.edit_index < 0) ? camera_count - 1 : mgr.edit_index;
                  cameras[saved_idx].enabled = mgr.edit_enabled ? 1 : 0;
                  apply_form_ptz(&cameras[saved_idx], &mgr);
                  onvif_after_save(&cameras[saved_idx], mgr.edit_type, mgr.edit_onvif_url,
                                   &onvif_probe_th, onvif_probe_target,
                                   sizeof(onvif_probe_target), &mgr, text_pt, text_color);
                }
                mgr.mode = 0;
                mgr.edit_index = -1;
                SDL_StopTextInput();
              }
              // No `continue` here: fall through to the shared apply block below
              // so an Enter-confirmed save rebuilds camera_ui / persists /
              // restarts streams exactly like the mouse Save button does.
            }
          }

          if (changed) {
            if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                      camera_count, &camera_ui, old_count, &mgr, &active_camera,
                                      stream_from_list, need_restart, window, &shared, &th, &dargs,
                                      &audio, &error_text, &has_error, &is_loading, &loading_since,
                                      &zoom, &view_x, &view_y, &dragging, &last_frame_ticks, &retry_at,
                                      &retry_delay_ms, active_url_buf, sizeof(active_url_buf),
                                      rec_mutex, &rec_want, rec_title_buf, sizeof(rec_title_buf)) != 0) {
              die_sdl("Failed to apply camera list changes");
            }
            destroy_text(&buffering_pct_text);
            last_buffer_pct = -1;

            // Matrix mode, 1 -> N cameras: release the single-camera latch so the grid
            // appears (mirrors the mouse-Save fix-up; the keyboard path had none).
            if (argc < 2 && view_mode == VIEW_MODE_MATRIX && old_count <= 1 && camera_count > 1 &&
                matrix_expanded == 0) {
              matrix_expanded = -1;
              stop_decode_thread(&shared, &th);
              audio_state_clear_queue(&audio);
              active_url_buf[0] = 0;
              dargs.url = active_url_buf;
              is_loading = 0;
              retry_at = 0;
              retry_delay_ms = 1000;
              if (texture) SDL_DestroyTexture(texture);
              texture = NULL;
              tex_w = 0;
              tex_h = 0;
            }

#ifdef _WIN32
            // Restart async cache/thumb loading for the updated camera list.
            if (argc < 2) {
              if (startup_cache_th) {
                SDL_AtomicSet(&startup_cache_quit, 1);
                SDL_WaitThread(startup_cache_th, NULL);
                startup_cache_th = NULL;
                SDL_AtomicSet(&startup_cache_quit, 0);
              }
              if (camera_count > 0) {
                startup_cache_args.cams = cameras;
                startup_cache_args.cam_count = camera_count;
                startup_cache_args.active_index = active_camera;
                snprintf(startup_cache_args.base_dir, sizeof(startup_cache_args.base_dir), "%s", base_dir);
                startup_cache_args.quit = &startup_cache_quit;
                startup_cache_th = seh_create_thread(startup_cache_load_thread_fn, "cache-load",
                                                    &startup_cache_args, &startup_cache_alive);
                startup_cache_seen_alive = 0;
              }
            }
#endif

            if (argc < 2) {
              if (preview_mode == PREVIEW_MODE_WARM) {
                warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
                warm_threads = NULL;
                warm_args = NULL;
                warm_count = 0;
                if (camera_count > 0) {
                  warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
                  warm_count = camera_count;
                  warm_spawn_next = 0;
                  warm_spawning = 1;
                }
              } else if (preview_mode == PREVIEW_MODE_STREAM) {
                tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
                tile_threads = NULL;
                tile_args = NULL;
                tile_count = 0;
                if (camera_count > 0) {
                  tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                       tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                  tile_count = camera_count;
                  tile_spawn_next = 0;
                  tile_spawning = 1;
                }
              }
            }
          }
          continue;
        }

        // PTZ: arrows pan/tilt, PgUp/PgDn zoom, for the PTZ camera shown big.
        // Held keys move continuously; the matching key-up stops the head.
        if (!settings.is_open && !plugins_ui.is_open && !mgr.is_open && !intro.is_open) {
          int ptz_dir = -1;
          if (e.key.keysym.sym == SDLK_UP) ptz_dir = 0;
          else if (e.key.keysym.sym == SDLK_DOWN) ptz_dir = 1;
          else if (e.key.keysym.sym == SDLK_LEFT) ptz_dir = 2;
          else if (e.key.keysym.sym == SDLK_RIGHT) ptz_dir = 3;
          else if (e.key.keysym.sym == SDLK_PAGEUP) ptz_dir = 4;
          else if (e.key.keysym.sym == SDLK_PAGEDOWN) ptz_dir = 5;
          if (ptz_dir >= 0) {
            // Key auto-repeat would re-send the same ContinuousMove endlessly;
            // one command per physical press is enough.
            if (!e.key.repeat && ptz_active_dir != ptz_dir) {
              int ci = ptz_pad_camera(cameras, camera_count, stream_from_list, view_mode,
                                      matrix_expanded, active_camera);
              if (ci >= 0) {
                PtzTarget t;
                ptz_fill_target(&cameras[ci], &t);
                float vx, vy, vz;
                ptz_dir_velocity(ptz_dir, &vx, &vy, &vz);
                ptz_move(ptz, &t, vx, vy, vz);
                ptz_active_dir = ptz_dir;
              }
            }
            continue;
          }
        }

        if (e.key.keysym.sym == SDLK_F5) {
          if (settings.is_open || plugins_ui.is_open || mgr.is_open) continue;
          // F5 works in every view (tiles big, matrix expanded, matrix grid):
          // it toggles cameras[active].record_enabled. The actual recording is
          // performed by whichever worker holds the camera's RTSP feed — the
          // decode_thread when a single tile is shown big, the tile_stream
          // worker when in matrix grid view.
          if (camera_count > 0 && active_camera >= 0 && active_camera < camera_count) {
            cameras[active_camera].record_enabled = cameras[active_camera].record_enabled ? 0 : 1;
#ifdef _WIN32
            persist_save_cameras(g_persist, settings_path, cameras, camera_count, active_camera);
#endif
            if (rec_mutex) SDL_LockMutex(rec_mutex);
            rec_want = cameras[active_camera].record_enabled ? 1 : 0;
            dargs.camera_index = active_camera;
            snprintf(rec_title_buf, sizeof(rec_title_buf), "%s",
                     cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
            if (rec_mutex) SDL_UnlockMutex(rec_mutex);
          }
          continue;
        }

        // Esc when no overlay is open: exit single-camera display (matrix expanded ->
        // grid, or F11 fullscreen -> windowed). Never quits the app — closing the
        // window is the only quit gesture.
        if (e.key.keysym.sym == SDLK_ESCAPE) {
          if (view_mode == VIEW_MODE_MATRIX && matrix_expanded >= 0) {
            // Mirrors the matrix double-click collapse path.
            matrix_expanded = -1;
            dargs.audio_enabled = 0;

            int keep_main_in_grid = 0;
            if (argc < 2 && preview_mode == PREVIEW_MODE_STREAM && camera_count > 1) {
              if (active_camera >= 0 && active_camera < camera_count &&
                  cameras[active_camera].preview_url && cameras[active_camera].preview_url[0]) {
                keep_main_in_grid = 1;
              }
            }
            if (keep_main_in_grid) {
              matrix_main_idx = active_camera;
              matrix_main_loading = is_loading ? 1 : 0;
              matrix_main_loading_since = loading_since;
              is_loading = 0;
              if (error_text.tex) {
                SDL_DestroyTexture(error_text.tex);
                error_text.tex = NULL;
              }
              has_error = 0;
            } else {
              stop_decode_thread(&shared, &th);
              audio_state_clear_queue(&audio);
              active_url_buf[0] = 0;
              dargs.url = active_url_buf;
              matrix_main_idx = -1;
              matrix_main_loading = 0;
              is_loading = 0;
              retry_at = 0;
              retry_delay_ms = 1000;
              if (error_text.tex) {
                SDL_DestroyTexture(error_text.tex);
                error_text.tex = NULL;
              }
              has_error = 0;
              if (texture) SDL_DestroyTexture(texture);
              texture = NULL;
              tex_w = 0;
              tex_h = 0;
            }
            dragging = 0;
            zoom = 1.0;
            view_x = 0.0;
            view_y = 0.0;
            continue;
          }
          if (is_fullscreen) {
            if (SDL_SetWindowFullscreen(window, 0) == 0) {
              is_fullscreen = 0;
              SDL_SetWindowPosition(window, prev_x, prev_y);
              SDL_SetWindowSize(window, prev_w, prev_h);
              if (prev_was_maximized) SDL_MaximizeWindow(window);
            }
            continue;
          }
        }

        // Volume +/- (handles both `+`/`=` and the numpad variants).
        if (e.key.keysym.sym == SDLK_PLUS || e.key.keysym.sym == SDLK_EQUALS ||
            e.key.keysym.sym == SDLK_KP_PLUS ||
            e.key.keysym.sym == SDLK_MINUS || e.key.keysym.sym == SDLK_KP_MINUS) {
          int step = (e.key.keysym.sym == SDLK_MINUS || e.key.keysym.sym == SDLK_KP_MINUS) ? -10 : +10;
          int cur = audio_volume_pct;
          int nv = cur + step;
          if (nv < 0) nv = 0;
          if (nv > 200) nv = 200;
          if (nv != cur) {
            audio_volume_pct = nv;
            SDL_AtomicSet(&audio_volume_pct_atomic, nv);
#ifdef _WIN32
            persist_set_int(g_persist, settings_path, "App", "AudioVolumePct", nv);
#endif
          }
          continue;
        }

        if (e.key.keysym.sym == SDLK_F11) {
          if (!is_fullscreen) {
            Uint32 wf = SDL_GetWindowFlags(window);
            prev_was_maximized = (wf & SDL_WINDOW_MAXIMIZED) ? 1 : 0;
            SDL_GetWindowPosition(window, &prev_x, &prev_y);
            SDL_GetWindowSize(window, &prev_w, &prev_h);
            if (prev_was_maximized) SDL_RestoreWindow(window);
            if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
              is_fullscreen = 1;
            }
          } else {
            if (SDL_SetWindowFullscreen(window, 0) == 0) {
              is_fullscreen = 0;
              SDL_SetWindowPosition(window, prev_x, prev_y);
              SDL_SetWindowSize(window, prev_w, prev_h);
              if (prev_was_maximized) SDL_MaximizeWindow(window);
            }
          }
        }
      }

      if (settings.is_open && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int mx = e.button.x;
        int my = e.button.y;

        SDL_Rect panel = {win_w / 2 - 380, win_h / 2 - 450, 760, 900};
        if (panel.x < 20) panel.x = 20;
        if (panel.y < 20) panel.y = 20;
        if (panel.x + panel.w > win_w - 20) panel.w = win_w - panel.x - 20;
        if (panel.y + panel.h > win_h - 20) panel.h = win_h - panel.y - 20;

        // Language picker — must match settings_render geometry exactly.
        {
          int lang_btn_h = 30;
          int lang_btn_w = 110;
          int lang_y = panel.y + 18 + 28 + 10;
          int total_w = lang_btn_w * 2 + 12;
          int en_x = panel.x + (panel.w - total_w) / 2;
          SDL_Rect lbtn_en = {en_x,                   lang_y, lang_btn_w, lang_btn_h};
          SDL_Rect lbtn_ru = {en_x + lang_btn_w + 12, lang_y, lang_btn_w, lang_btn_h};
          if (mx >= lbtn_en.x && mx < lbtn_en.x + lbtn_en.w && my >= lbtn_en.y &&
              my < lbtn_en.y + lbtn_en.h) {
            settings.pending_language = LANG_EN;
            continue;
          }
          if (mx >= lbtn_ru.x && mx < lbtn_ru.x + lbtn_ru.w && my >= lbtn_ru.y &&
              my < lbtn_ru.y + lbtn_ru.h) {
            settings.pending_language = LANG_RU;
            continue;
          }
        }

        const int btn_h = 44;
        const int btn_w = 200;
        const int gap = 12;
        SDL_Rect btn_save = {panel.x + panel.w / 2 - btn_w - gap / 2, panel.y + panel.h - btn_h - 26,
                             btn_w, btn_h};
        SDL_Rect btn_cancel = {panel.x + panel.w / 2 + gap / 2, panel.y + panel.h - btn_h - 26, btn_w,
                               btn_h};

        int x = panel.x + 40;
        int y = panel.y + 110;

        // Render scale quality buttons (match settings_render layout).
        y += 60;
        SDL_Rect opt_nearest = {x, y, panel.w - 80, 44};
        SDL_Rect opt_linear = opt_nearest;
        opt_linear.y += opt_linear.h + gap;
        SDL_Rect opt_best = opt_linear;
        opt_best.y += opt_best.h + gap;

        // Recording storage limit stepper (match settings_render layout).
        int arch_label_y = opt_best.y + opt_best.h + gap + 10;
        int arch_row_y = arch_label_y + 34;
        SDL_Rect arch_minus = {x, arch_row_y, 44, 44};
        SDL_Rect arch_value = {x + 44 + gap, arch_row_y, btn_w, 44};
        SDL_Rect arch_plus = {arch_value.x + arch_value.w + gap, arch_row_y, 44, 44};

        // View mode buttons (match settings_render layout).
        y = arch_row_y + 44 + 16;
        y += 34;
        SDL_Rect mode_tiles = {x, y, btn_w, 44};
        SDL_Rect mode_matrix = {x + btn_w + gap, y, btn_w, 44};

        // Preview mode buttons (match settings_render layout).
        int preview_label_y = mode_tiles.y + mode_tiles.h + 24;
        const int stream_switch_reserved =
            (settings.pending_preview_mode == PREVIEW_MODE_STREAM) ? (44 + 18 + 34 + 44) : 0;
        int pv_max_y = btn_save.y - btn_h - 14 - stream_switch_reserved;
        int help_y = preview_label_y + 34;
        int pv_y = help_y;
        settings_preview_compute_layout(&settings, preview_label_y, pv_max_y, &help_y, &pv_y, NULL, NULL);
        SDL_Rect pv_static = {x, pv_y, btn_w, 44};
        SDL_Rect pv_warm = {x + btn_w + gap, pv_y, btn_w, 44};
        SDL_Rect pv_stream = {x + 2 * (btn_w + gap), pv_y, btn_w, 44};

        SDL_Rect src_preview = {0};
        SDL_Rect src_main = {0};
        if (settings.pending_preview_mode == PREVIEW_MODE_STREAM) {
          int sy = pv_y + 44 + 18 + 34;
          src_preview = (SDL_Rect){x, sy, btn_w, 44};
          src_main = (SDL_Rect){x + btn_w + gap, sy, btn_w, 44};
        }

        if (mx >= opt_nearest.x && mx < opt_nearest.x + opt_nearest.w && my >= opt_nearest.y &&
            my < opt_nearest.y + opt_nearest.h) {
          snprintf(settings.pending_quality, sizeof(settings.pending_quality), "nearest");
          settings.selected = 0;
          continue;
        }
        if (mx >= opt_linear.x && mx < opt_linear.x + opt_linear.w && my >= opt_linear.y &&
            my < opt_linear.y + opt_linear.h) {
          snprintf(settings.pending_quality, sizeof(settings.pending_quality), "linear");
          settings.selected = 0;
          continue;
        }
        if (mx >= opt_best.x && mx < opt_best.x + opt_best.w && my >= opt_best.y &&
            my < opt_best.y + opt_best.h) {
          snprintf(settings.pending_quality, sizeof(settings.pending_quality), "best");
          settings.selected = 0;
          continue;
        }

        // Recording storage limit: step the GB value down/up (0 = Off).
        if (mx >= arch_minus.x && mx < arch_minus.x + arch_minus.w && my >= arch_minus.y &&
            my < arch_minus.y + arch_minus.h) {
          settings.pending_archive_gb -= SETTINGS_ARCHIVE_STEP_GB;
          if (settings.pending_archive_gb < 0) settings.pending_archive_gb = 0;
          continue;
        }
        if (mx >= arch_plus.x && mx < arch_plus.x + arch_plus.w && my >= arch_plus.y &&
            my < arch_plus.y + arch_plus.h) {
          settings.pending_archive_gb += SETTINGS_ARCHIVE_STEP_GB;
          if (settings.pending_archive_gb > SETTINGS_ARCHIVE_MAX_GB) {
            settings.pending_archive_gb = SETTINGS_ARCHIVE_MAX_GB;
          }
          continue;
        }

        if (mx >= mode_tiles.x && mx < mode_tiles.x + mode_tiles.w && my >= mode_tiles.y &&
            my < mode_tiles.y + mode_tiles.h) {
          settings.pending_view_mode = VIEW_MODE_TILES;
          settings.selected = 1;
          continue;
        }
        if (mx >= mode_matrix.x && mx < mode_matrix.x + mode_matrix.w && my >= mode_matrix.y &&
            my < mode_matrix.y + mode_matrix.h) {
          settings.pending_view_mode = VIEW_MODE_MATRIX;
          settings.selected = 1;
          continue;
        }

        if (mx >= pv_static.x && mx < pv_static.x + pv_static.w && my >= pv_static.y &&
            my < pv_static.y + pv_static.h) {
          settings.pending_preview_mode = PREVIEW_MODE_STATIC;
          settings.selected = 2;
          continue;
        }
        if (mx >= pv_warm.x && mx < pv_warm.x + pv_warm.w && my >= pv_warm.y &&
            my < pv_warm.y + pv_warm.h) {
          settings.pending_preview_mode = PREVIEW_MODE_WARM;
          settings.selected = 2;
          continue;
        }
        if (mx >= pv_stream.x && mx < pv_stream.x + pv_stream.w && my >= pv_stream.y &&
            my < pv_stream.y + pv_stream.h) {
          settings.pending_preview_mode = PREVIEW_MODE_STREAM;
          settings.selected = 2;
          continue;
        }

        if (settings.pending_preview_mode == PREVIEW_MODE_STREAM) {
          if (mx >= src_preview.x && mx < src_preview.x + src_preview.w && my >= src_preview.y &&
              my < src_preview.y + src_preview.h) {
            settings.pending_tiles_use_preview_url = 1;
            settings.selected = 3;
            continue;
          }
          if (mx >= src_main.x && mx < src_main.x + src_main.w && my >= src_main.y &&
              my < src_main.y + src_main.h) {
            settings.pending_tiles_use_preview_url = 0;
            settings.selected = 3;
            continue;
          }
        }

        if (mx >= btn_cancel.x && mx < btn_cancel.x + btn_cancel.w && my >= btn_cancel.y &&
            my < btn_cancel.y + btn_cancel.h) {
          settings_close(&settings);
          continue;
        }

          if (mx >= btn_save.x && mx < btn_save.x + btn_save.w && my >= btn_save.y &&
             my < btn_save.y + btn_save.h) {
          if (settings_save_to_ini(settings_path, &settings) == 0) {
            // i18n: re-render main-view localizable text in the now-current language.
            destroy_text(&no_cameras_text);
            if (error_text.tex) { SDL_DestroyTexture(error_text.tex); error_text.tex = NULL; has_error = 0; }
            snprintf(scale_quality, sizeof(scale_quality), "%s",
                     normalize_scale_quality(settings.pending_quality));
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_quality);

            // Apply the recording archive limit live for the trimmer thread.
            archive_limit_gb = settings.pending_archive_gb;
#ifdef _WIN32
            SDL_AtomicSet(&archive_limit_gb_atomic, archive_limit_gb);
#endif

            ViewMode prev_mode = view_mode;
            view_mode = settings.pending_view_mode;
            if (argc < 2 && prev_mode != view_mode) {
              if (view_mode == VIEW_MODE_MATRIX && camera_count > 1) {
                matrix_expanded = -1;
                stop_decode_thread(&shared, &th);
                audio_state_clear_queue(&audio);
                active_url_buf[0] = 0;
                dargs.url = active_url_buf;
                is_loading = 0;
                retry_at = 0;
                retry_delay_ms = 1000;
                dragging = 0;
                zoom = 1.0;
                view_x = 0.0;
                view_y = 0.0;
                if (error_text.tex) {
                  SDL_DestroyTexture(error_text.tex);
                  error_text.tex = NULL;
                }
                has_error = 0;
                if (texture) SDL_DestroyTexture(texture);
                texture = NULL;
                tex_w = 0;
                tex_h = 0;
              } else if (view_mode == VIEW_MODE_MATRIX && camera_count == 1) {
                matrix_expanded = 0;
                active_camera = 0;
                if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                          camera_count, &camera_ui, camera_count, &mgr, &active_camera,
                                          stream_from_list, 1, window, &shared, &th, &dargs, &audio,
                                          &error_text, &has_error, &is_loading, &loading_since, &zoom,
                                          &view_x, &view_y, &dragging, &last_frame_ticks, &retry_at,
                                          &retry_delay_ms, active_url_buf, sizeof(active_url_buf),
                                          rec_mutex, &rec_want, rec_title_buf,
                                          sizeof(rec_title_buf)) != 0) {
                  die_sdl("Failed to apply view mode change");
                }
              } else if (view_mode == VIEW_MODE_TILES) {
                matrix_expanded = -1;
                if (camera_count > 0) {
                  if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                            camera_count, &camera_ui, camera_count, &mgr, &active_camera,
                                            stream_from_list, 1, window, &shared, &th, &dargs, &audio,
                                            &error_text, &has_error, &is_loading, &loading_since, &zoom,
                                            &view_x, &view_y, &dragging, &last_frame_ticks, &retry_at,
                                            &retry_delay_ms, active_url_buf, sizeof(active_url_buf),
                                            rec_mutex, &rec_want, rec_title_buf,
                                            sizeof(rec_title_buf)) != 0) {
                    die_sdl("Failed to apply view mode change");
                  }
                }
              }
            }

            int prev_tiles_use_preview_url = tiles_use_preview_url;
            tiles_use_preview_url = settings.pending_tiles_use_preview_url ? 1 : 0;

            PreviewMode prev = preview_mode;
            preview_mode = settings.pending_preview_mode;
            if (argc < 2 && prev != preview_mode) {
              if (prev == PREVIEW_MODE_WARM) {
                warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
                warm_threads = NULL;
                warm_args = NULL;
                warm_count = 0;
              }
              if (prev == PREVIEW_MODE_STREAM) {
                tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
                tile_threads = NULL;
                tile_args = NULL;
                tile_count = 0;
              }

              if (preview_mode == PREVIEW_MODE_WARM) {
                warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
                warm_count = camera_count;
                warm_spawn_next = 0;
                warm_spawning = 1;
              } else if (preview_mode == PREVIEW_MODE_STREAM) {
                tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                     tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                tile_count = camera_count;
                tile_spawn_next = 0;
                tile_spawning = 1;
              }
            } else if (argc < 2 && preview_mode == PREVIEW_MODE_STREAM &&
                       prev_tiles_use_preview_url != tiles_use_preview_url) {
              tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
              tile_threads = NULL;
              tile_args = NULL;
              tile_count = 0;
              if (camera_count > 0) {
                tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                     tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                tile_count = camera_count;
                tile_spawn_next = 0;
                tile_spawning = 1;
              }

              if (view_mode == VIEW_MODE_MATRIX && matrix_expanded < 0 && tiles_use_preview_url == 0 &&
                  matrix_main_idx >= 0) {
                pending_main_start_idx = -1;
                if (th) request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
                audio_state_clear_queue(&audio);
                active_url_buf[0] = 0;
                dargs.url = active_url_buf;
                matrix_main_idx = -1;
                matrix_main_loading = 0;
                retry_at = 0;
                retry_delay_ms = 1000;
                is_loading = 0;
                if (error_text.tex) {
                  SDL_DestroyTexture(error_text.tex);
                  error_text.tex = NULL;
                }
                has_error = 0;
                if (texture) SDL_DestroyTexture(texture);
                texture = NULL;
                tex_w = 0;
                tex_h = 0;
              }
            }
          }
          settings_close(&settings);
          continue;
        }

        // Click outside panel closes.
        if (!(mx >= panel.x && mx < panel.x + panel.w && my >= panel.y && my < panel.y + panel.h)) {
          settings_close(&settings);
          continue;
        }
      }

      if (settings.is_open) continue;

      if (plugins_ui.is_open && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        int mx = e.button.x;
        int my = e.button.y;

        SDL_Rect panel = {win_w / 2 - 450, win_h / 2 - 320, 900, 640};
        if (panel.x < 20) panel.x = 20;
        if (panel.y < 20) panel.y = 20;
        if (panel.x + panel.w > win_w - 20) panel.w = win_w - panel.x - 20;
        if (panel.y + panel.h > win_h - 20) panel.h = win_h - panel.y - 20;

        SDL_Rect list_rect = {panel.x + 20, panel.y + 70, panel.w - 40, panel.h - 70 - 44 - 26 - 10};

        const int btn_h = 44;
        const int btn_w = 160;
        const int gap = 12;
        SDL_Rect btn_rescan = {panel.x + panel.w / 2 - (btn_w * 3 + gap * 2) / 2,
                               panel.y + panel.h - btn_h - 26, btn_w, btn_h};
        SDL_Rect btn_toggle = {btn_rescan.x + btn_w + gap, btn_rescan.y, btn_w, btn_h};
        SDL_Rect btn_close = {btn_toggle.x + btn_w + gap, btn_rescan.y, btn_w, btn_h};

        if (mx >= btn_close.x && mx < btn_close.x + btn_close.w && my >= btn_close.y &&
            my < btn_close.y + btn_close.h) {
          plugins_overlay_close(&plugins_ui);
          continue;
        }
        if (mx >= btn_rescan.x && mx < btn_rescan.x + btn_rescan.w && my >= btn_rescan.y &&
            my < btn_rescan.y + btn_rescan.h) {
          plugin_host_rescan(&plugins);
          plugins_overlay_rebuild_list(&plugins_ui, renderer, &plugins, text_pt, text_color);
          continue;
        }
        if (mx >= btn_toggle.x && mx < btn_toggle.x + btn_toggle.w && my >= btn_toggle.y &&
            my < btn_toggle.y + btn_toggle.h) {
          int idx = plugins_ui.selected;
          int enabled = 0;
          if (plugins.mutex) SDL_LockMutex(plugins.mutex);
          if (idx >= 0 && idx < plugins.row_count) enabled = plugins.rows[idx].enabled ? 1 : 0;
          if (plugins.mutex) SDL_UnlockMutex(plugins.mutex);
          plugin_host_set_enabled(&plugins, idx, !enabled);
          plugins_overlay_rebuild_list(&plugins_ui, renderer, &plugins, text_pt, text_color);
          continue;
        }

        if (mx >= list_rect.x && mx < list_rect.x + list_rect.w && my >= list_rect.y &&
            my < list_rect.y + list_rect.h) {
          int row_h = 34;
          int row = (my - list_rect.y) / row_h;
          int idx = plugins_ui.scroll + row;
          if (idx >= 0 && idx < plugins_ui.list_count) plugins_ui.selected = idx;
          continue;
        }

        // Click outside closes.
        if (!(mx >= panel.x && mx < panel.x + panel.w && my >= panel.y && my < panel.y + panel.h)) {
          plugins_overlay_close(&plugins_ui);
          continue;
        }
      }

      if (plugins_ui.is_open) continue;

      if (mgr.is_open && e.type == SDL_TEXTINPUT && mgr.mode == 1) {
        if (mgr.edit_field == 1) {
          if (camui_insert_at(mgr.edit_title, sizeof(mgr.edit_title), &mgr.title_cursor, text_pt,
                              e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        } else if (mgr.edit_field == 2) {
          if (camui_insert_at(mgr.edit_url, sizeof(mgr.edit_url), &mgr.url_cursor, text_pt,
                              e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        } else if (mgr.edit_field == 3) {
          if (camui_insert_at(mgr.edit_preview_url, sizeof(mgr.edit_preview_url), &mgr.preview_cursor,
                              text_pt, e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        } else if (mgr.edit_field == 4) {
          if (camui_insert_at(mgr.edit_onvif_url, sizeof(mgr.edit_onvif_url), &mgr.onvif_cursor,
                              text_pt, e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        } else if (mgr.edit_field == 6) {
          if (camui_insert_at(mgr.edit_ptz_url, sizeof(mgr.edit_ptz_url), &mgr.ptz_url_cursor,
                              text_pt, e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        } else if (mgr.edit_field == 7) {
          if (camui_insert_at(mgr.edit_ptz_user, sizeof(mgr.edit_ptz_user), &mgr.ptz_user_cursor,
                              text_pt, e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        } else if (mgr.edit_field == 8) {
          if (camui_insert_at(mgr.edit_ptz_pass, sizeof(mgr.edit_ptz_pass), &mgr.ptz_pass_cursor,
                              text_pt, e.text.text) == 0) {
            camera_manager_update_edit_texts(&mgr, text_pt, text_color);
          }
        }
        continue;
      }

      if (mgr.is_open && e.type == SDL_MOUSEWHEEL) {
        if (mgr.mode == 0) {
          if (e.wheel.y > 0 && mgr.scroll > 0) mgr.scroll--;
          if (e.wheel.y < 0) mgr.scroll++;
        }
        continue;
      }

      if (mgr.is_open && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
          const int pad = 18;
          const int row_h = 34;
          const int btn_h = 44;
          const int btn_w = 140;
          const int btn_gap = 12;

          SDL_Rect main_rect = {pad, 60, win_w - 2 * pad, win_h - 60 - btn_h - pad - 10};
          int mx = e.button.x;
          int my = e.button.y;

          if (mgr.mode == 0) {
            // List click -> select
            if (mx >= main_rect.x && mx < main_rect.x + main_rect.w && my >= main_rect.y &&
                my < main_rect.y + main_rect.h) {
              int row = (my - main_rect.y) / row_h;
              int idx = mgr.scroll + row;
              if (idx >= 0 && idx < camera_count) mgr.selected = idx;
              continue;
            }

            int total_btns = 4;
            int bar_w = total_btns * btn_w + (total_btns - 1) * btn_gap;
            int x0 = (win_w - bar_w) / 2;
            int y0 = win_h - btn_h - pad;

            SDL_Rect btn_add = {x0 + 0 * (btn_w + btn_gap), y0, btn_w, btn_h};
            SDL_Rect btn_edit = {x0 + 1 * (btn_w + btn_gap), y0, btn_w, btn_h};
            SDL_Rect btn_del = {x0 + 2 * (btn_w + btn_gap), y0, btn_w, btn_h};
            SDL_Rect btn_close = {x0 + 3 * (btn_w + btn_gap), y0, btn_w, btn_h};

            if (mx >= btn_add.x && mx < btn_add.x + btn_add.w && my >= btn_add.y &&
                my < btn_add.y + btn_add.h) {
              camera_manager_begin_edit(&mgr, -1, cameras, camera_count, text_pt, text_color);
              continue;
            }
            if (mx >= btn_edit.x && mx < btn_edit.x + btn_edit.w && my >= btn_edit.y &&
                my < btn_edit.y + btn_edit.h) {
              camera_manager_begin_edit(&mgr, mgr.selected, cameras, camera_count, text_pt, text_color);
              continue;
            }
            if (mx >= btn_close.x && mx < btn_close.x + btn_close.w && my >= btn_close.y &&
                my < btn_close.y + btn_close.h) {
              camera_manager_close(&mgr);
              continue;
            }
            if (mx >= btn_del.x && mx < btn_del.x + btn_del.w && my >= btn_del.y &&
                my < btn_del.y + btn_del.h) {
              int old_count = camera_count;
              int del_idx = mgr.selected;
              int need_restart = 0;
#ifdef _WIN32
              if (startup_cache_th) {
                SDL_AtomicSet(&startup_cache_quit, 1);
                SDL_WaitThread(startup_cache_th, NULL);
                startup_cache_th = NULL;
                SDL_AtomicSet(&startup_cache_quit, 0);
              }
#endif
              STOP_CAMERA_WORKERS();
              if (cameras_delete(&cameras, &camera_count, del_idx) == 0) {
                if (del_idx < active_camera) active_camera--;
                if (del_idx == active_camera) {
                  if (active_camera >= camera_count) active_camera = camera_count - 1;
                  need_restart = stream_from_list ? 1 : 0;
                }
                if (mgr.selected >= camera_count) mgr.selected = camera_count - 1;

                if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                          camera_count, &camera_ui, old_count, &mgr, &active_camera,
                                          stream_from_list, need_restart, window, &shared, &th, &dargs,
                                          &audio, &error_text, &has_error, &is_loading, &loading_since,
                                          &zoom, &view_x, &view_y, &dragging, &last_frame_ticks, &retry_at,
                                          &retry_delay_ms, active_url_buf, sizeof(active_url_buf),
                                          rec_mutex, &rec_want, rec_title_buf, sizeof(rec_title_buf)) != 0) {
                  die_sdl("Failed to apply camera list changes");
                }

                if (argc < 2) {
                  if (preview_mode == PREVIEW_MODE_WARM) {
                    warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
                    warm_threads = NULL;
                    warm_args = NULL;
                    warm_count = 0;
                    if (camera_count > 0) {
                      warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
                      warm_count = camera_count;
                      warm_spawn_next = 0;
                      warm_spawning = 1;
                    }
                  } else if (preview_mode == PREVIEW_MODE_STREAM) {
                    tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
                    tile_threads = NULL;
                    tile_args = NULL;
                    tile_count = 0;
                    if (camera_count > 0) {
                      tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                           tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                      tile_count = camera_count;
                      tile_spawn_next = 0;
                      tile_spawning = 1;
                    }
                  }
                }

#ifdef _WIN32
                if (argc < 2) {
                  if (startup_cache_th) {
                    SDL_AtomicSet(&startup_cache_quit, 1);
                    SDL_WaitThread(startup_cache_th, NULL);
                    startup_cache_th = NULL;
                    SDL_AtomicSet(&startup_cache_quit, 0);
                  }
                  if (camera_count > 0) {
                    startup_cache_args.cams = cameras;
                    startup_cache_args.cam_count = camera_count;
                    startup_cache_args.active_index = active_camera;
                    snprintf(startup_cache_args.base_dir, sizeof(startup_cache_args.base_dir), "%s", base_dir);
                    startup_cache_args.quit = &startup_cache_quit;
                    startup_cache_th =
                        seh_create_thread(startup_cache_load_thread_fn, "cache-load", &startup_cache_args, &startup_cache_alive);
                    startup_cache_seen_alive = 0;
                  }
                }
#endif
              }
              continue;
            }
          } else {
            // Exact same geometry the renderer uses.
            CamMgrLayout lay;
            camera_manager_layout(&mgr, win_w, win_h, &lay);
            int clicked_field = -1;
            SDL_Rect clicked_box = {0, 0, 0, 0};
            for (int f = 0; f < CAMMGR_FIELD_COUNT; f++) {
              if (!lay.visible[f]) continue;
              SDL_Rect box = lay.boxes[f];
              if (mx >= box.x && mx < box.x + box.w && my >= box.y && my < box.y + box.h) {
                clicked_field = f;
                clicked_box = box;
                break;
              }
            }
            if (clicked_field == CAMMGR_FIELD_TYPE) {
              mgr.edit_field = CAMMGR_FIELD_TYPE;
              mgr.edit_type = camera_manager_cycle_type(mgr.edit_type);
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (clicked_field == CAMMGR_FIELD_PTZ_ON) {
              mgr.edit_field = CAMMGR_FIELD_PTZ_ON;
              mgr.edit_ptz_on = !mgr.edit_ptz_on;
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (clicked_field == CAMMGR_FIELD_ENABLED) {
              mgr.edit_field = CAMMGR_FIELD_ENABLED;
              mgr.edit_enabled = !mgr.edit_enabled;
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }
            if (clicked_field == 1) {
              mgr.edit_field = 1;
              int x_px = mx - (clicked_box.x + 10);
              mgr.title_cursor = camui_cursor_from_x(mgr.edit_title, text_pt, x_px);
              mgr.title_cursor_px = camui_measure_px(text_pt, mgr.edit_title, mgr.title_cursor);
              continue;
            }
            if (clicked_field == 2) {
              mgr.edit_field = 2;
              int x_px = mx - (clicked_box.x + 10);
              mgr.url_cursor = camui_cursor_from_x(mgr.edit_url, text_pt, x_px);
              mgr.url_cursor_px = camui_measure_px(text_pt, mgr.edit_url, mgr.url_cursor);
              continue;
            }
            if (clicked_field == 3) {
              mgr.edit_field = 3;
              int x_px = mx - (clicked_box.x + 10);
              mgr.preview_cursor = camui_cursor_from_x(mgr.edit_preview_url, text_pt, x_px);
              mgr.preview_cursor_px =
                  camui_measure_px(text_pt, mgr.edit_preview_url, mgr.preview_cursor);
              continue;
            }
            if (clicked_field == 4) {
              mgr.edit_field = 4;
              int x_px = mx - (clicked_box.x + 10);
              mgr.onvif_cursor = camui_cursor_from_x(mgr.edit_onvif_url, text_pt, x_px);
              mgr.onvif_cursor_px = camui_measure_px(text_pt, mgr.edit_onvif_url, mgr.onvif_cursor);
              continue;
            }
            if (clicked_field == 6) {
              mgr.edit_field = 6;
              int x_px = mx - (clicked_box.x + 10);
              mgr.ptz_url_cursor = camui_cursor_from_x(mgr.edit_ptz_url, text_pt, x_px);
              mgr.ptz_url_cursor_px =
                  camui_measure_px(text_pt, mgr.edit_ptz_url, mgr.ptz_url_cursor);
              continue;
            }
            if (clicked_field == 7) {
              mgr.edit_field = 7;
              int x_px = mx - (clicked_box.x + 10);
              mgr.ptz_user_cursor = camui_cursor_from_x(mgr.edit_ptz_user, text_pt, x_px);
              mgr.ptz_user_cursor_px =
                  camui_measure_px(text_pt, mgr.edit_ptz_user, mgr.ptz_user_cursor);
              continue;
            }
            if (clicked_field == 8) {
              // Password: caret goes to the end (the text is masked anyway).
              mgr.edit_field = 8;
              mgr.ptz_pass_cursor = (int)strlen(mgr.edit_ptz_pass);
              camera_manager_update_edit_texts(&mgr, text_pt, text_color);
              continue;
            }

            int total_btns = 2;
            int bar_w = total_btns * btn_w + (total_btns - 1) * btn_gap;
            int x0 = (win_w - bar_w) / 2;
            int y0 = win_h - btn_h - pad;

            SDL_Rect btn_save = {x0, y0, btn_w, btn_h};
            SDL_Rect btn_cancel = {x0 + (btn_w + btn_gap), y0, btn_w, btn_h};

            if (mx >= btn_cancel.x && mx < btn_cancel.x + btn_cancel.w && my >= btn_cancel.y &&
                my < btn_cancel.y + btn_cancel.h) {
              mgr.mode = 0;
              mgr.edit_index = -1;
              SDL_StopTextInput();
              continue;
            }

            if (mx >= btn_save.x && mx < btn_save.x + btn_save.w && my >= btn_save.y &&
                my < btn_save.y + btn_save.h) {
              // An ONVIF camera may be saved with only its device URL — it then
              // serves as the stream-URL placeholder until the probe fills them.
              const char *mgr_save_url =
                  mgr.edit_url[0] ? mgr.edit_url
                  : (mgr.edit_type == CAMERA_TYPE_ONVIF) ? mgr.edit_onvif_url
                                                         : "";
              if (mgr.edit_title[0] && mgr_save_url[0]) {
                int old_count = camera_count;
                int changed = 0;
                int need_restart = 0;

#ifdef _WIN32
                if (startup_cache_th) {
                  SDL_AtomicSet(&startup_cache_quit, 1);
                  SDL_WaitThread(startup_cache_th, NULL);
                  startup_cache_th = NULL;
                  SDL_AtomicSet(&startup_cache_quit, 0);
                }
#endif
                STOP_CAMERA_WORKERS();
                if (mgr.edit_index < 0) {
                  if (cameras_add(&cameras, &camera_count, mgr.edit_type, mgr.edit_title,
                                  mgr_save_url, mgr.edit_preview_url) == 0) {
                    cameras_ensure_locks(cameras, camera_count);
                    changed = 1;
                    mgr.selected = camera_count - 1;
                  }
                } else {
                  if (cameras_edit(cameras, camera_count, mgr.edit_index, mgr.edit_type,
                                   mgr.edit_title, mgr_save_url, mgr.edit_preview_url) == 0) {
                    changed = 1;
                    if (mgr.edit_index == active_camera) need_restart = stream_from_list ? 1 : 0;
                  }
                }
                if (changed) {
                  int saved_idx = (mgr.edit_index < 0) ? camera_count - 1 : mgr.edit_index;
                  cameras[saved_idx].enabled = mgr.edit_enabled ? 1 : 0;
                  apply_form_ptz(&cameras[saved_idx], &mgr);
                  onvif_after_save(&cameras[saved_idx], mgr.edit_type, mgr.edit_onvif_url,
                                   &onvif_probe_th, onvif_probe_target,
                                   sizeof(onvif_probe_target), &mgr, text_pt, text_color);
                }

                if (changed) {
                  if (apply_cameras_changed(settings_path, renderer, text_pt, text_color, cameras,
                                            camera_count, &camera_ui, old_count, &mgr, &active_camera,
                                            stream_from_list, need_restart, window, &shared, &th, &dargs,
                                            &audio, &error_text, &has_error, &is_loading,
                                            &loading_since, &zoom, &view_x, &view_y, &dragging,
                                            &last_frame_ticks, &retry_at, &retry_delay_ms, active_url_buf,
                                            sizeof(active_url_buf), rec_mutex, &rec_want, rec_title_buf,
                                            sizeof(rec_title_buf)) != 0) {
                    die_sdl("Failed to apply camera list changes");
                  }

#ifdef _WIN32
                  if (argc < 2) {
                    if (startup_cache_th) {
                      SDL_AtomicSet(&startup_cache_quit, 1);
                      SDL_WaitThread(startup_cache_th, NULL);
                      startup_cache_th = NULL;
                      SDL_AtomicSet(&startup_cache_quit, 0);
                    }
                    if (camera_count > 0) {
                      startup_cache_args.cams = cameras;
                      startup_cache_args.cam_count = camera_count;
                      startup_cache_args.active_index = active_camera;
                      snprintf(startup_cache_args.base_dir, sizeof(startup_cache_args.base_dir), "%s", base_dir);
                      startup_cache_args.quit = &startup_cache_quit;
                      startup_cache_th =
                          seh_create_thread(startup_cache_load_thread_fn, "cache-load", &startup_cache_args, &startup_cache_alive);
                      startup_cache_seen_alive = 0;
                    }
                  }
#endif

                  // Matrix mode: grid view shouldn't auto-start a live decode.
                  if (argc < 2 && view_mode == VIEW_MODE_MATRIX) {
                    // 1 -> N cameras: the single-camera latch (matrix_expanded == 0) must be
                    // released, otherwise the new camera never gets a grid tile until Esc/restart.
                    if (old_count <= 1 && camera_count > 1 && matrix_expanded == 0) matrix_expanded = -1;
                    if (camera_count <= 0) {
                      matrix_expanded = -1;
                      stop_decode_thread(&shared, &th);
                      active_url_buf[0] = 0;
                      dargs.url = active_url_buf;
                      is_loading = 0;
                    } else if (camera_count == 1) {
                      matrix_expanded = 0;
                      active_camera = 0;
                      int tiles_main_mode_now =
                          (argc < 2 && preview_mode == PREVIEW_MODE_STREAM && tiles_use_preview_url == 0 &&
                           camera_count > 0)
                              ? 1
                              : 0;
                      if (tiles_main_mode_now) {
                        // Single pipeline: tiles already receive the main stream. Do not start a duplicate decoder.
                        if (th) stop_decode_thread(&shared, &th);
                        active_url_buf[0] = 0;
                        dargs.url = active_url_buf;
                        is_loading = 0;
                        destroy_text(&buffering_pct_text);
                        last_buffer_pct = -1;
                      } else if (!th && !camera_streamable(cameras, camera_count, active_camera)) {
                        // Disabled camera: show it, but never open a session.
                        active_url_buf[0] = 0;
                        dargs.url = active_url_buf;
                        is_loading = 0;
                      } else if (!th) {
                        build_camera_url(cameras[active_camera].type, cameras[active_camera].url,
                                         active_url_buf, sizeof(active_url_buf));
                        dargs.url = active_url_buf;
                        expected_keyint_ms =
                            (cameras[active_camera].keyint_ms > 0) ? cameras[active_camera].keyint_ms : 5000;
                        dargs.initial_keyint_ms = expected_keyint_ms;
                        th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", &dargs, &shared.alive);
                        if (!th) die_sdl("SDL_CreateThread failed");
                        is_loading = 1;
                        loading_since = SDL_GetTicks();
                      }
                    } else if (matrix_expanded < 0) {
                      stop_decode_thread(&shared, &th);
                      audio_state_clear_queue(&audio);
                      active_url_buf[0] = 0;
                      dargs.url = active_url_buf;
                      is_loading = 0;
                      retry_at = 0;
                      retry_delay_ms = 1000;
                      if (texture) SDL_DestroyTexture(texture);
                      texture = NULL;
                      tex_w = 0;
                      tex_h = 0;
                    } else if (matrix_expanded >= camera_count) {
                      matrix_expanded = -1;
                    }
                  }

                  if (argc < 2) {
                    if (preview_mode == PREVIEW_MODE_WARM) {
                      warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
                      warm_threads = NULL;
                      warm_args = NULL;
                      warm_count = 0;
                      if (camera_count > 0) {
                        warm_cams_prepare(&warm_threads, &warm_args, &warm_quit, cameras, camera_count);
                        warm_count = camera_count;
                        warm_spawn_next = 0;
                        warm_spawning = 1;
                      }
                    } else if (preview_mode == PREVIEW_MODE_STREAM) {
                      tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
                      tile_threads = NULL;
                      tile_args = NULL;
                      tile_count = 0;
                      if (camera_count > 0) {
                        tile_streams_prepare(&tile_threads, &tile_args, &tile_quit, cameras, camera_count,
                                             tiles_use_preview_url, &tile_hires_index, base_dir,
                           base_dir, rec_mutex,
                                       audio_pipe, &audio_active_idx_atomic);
                        tile_count = camera_count;
                        tile_spawn_next = 0;
                        tile_spawning = 1;
                      }
                    }
                  }
                }

                mgr.mode = 0;
                mgr.edit_index = -1;
                SDL_StopTextInput();
              }
              continue;
            }
          }
        }
        continue;
      }

    if (mgr.is_open) continue;

    // Never do any potentially-blocking work before pumping events.
#ifdef _WIN32
    if (startup_cache_pending_start && !startup_cache_th) {
      startup_cache_th = seh_create_thread(startup_cache_load_thread_fn, "cache-load", &startup_cache_args, &startup_cache_alive);
      startup_cache_seen_alive = 0;
      startup_cache_pending_start = 0;
      append_startup_marker(base_dir, "cache_thread_started");
      startup_tracef(base_dir, "cache_thread_started ok=%d", startup_cache_th ? 1 : 0);
    }
#endif
    if (e.type == SDL_MOUSEMOTION) {
      if (e.motion.windowID != main_win_id) continue;
      last_mouse_x = e.motion.x;
        last_mouse_y = e.motion.y;
        if (view_mode == VIEW_MODE_TILES) overlay_until = SDL_GetTicks() + 5000;

        if (dragging && zoom > 1.0 && texture && tex_w > 0 && tex_h > 0 &&
            (view_mode == VIEW_MODE_TILES || matrix_expanded >= 0)) {
          int win_w = 0, win_h = 0;
          SDL_GetWindowSize(window, &win_w, &win_h);
          if (win_w > 0 && win_h > 0) {
            double src_w = (double)tex_w / zoom;
            double src_h = (double)tex_h / zoom;

            int dx = e.motion.x - drag_last_x;
            int dy = e.motion.y - drag_last_y;
            drag_last_x = e.motion.x;
            drag_last_y = e.motion.y;

            // Drag like a "hand": moving mouse right moves image right => view window moves left.
            view_x -= (double)dx * (src_w / (double)win_w);
            view_y -= (double)dy * (src_h / (double)win_h);

            if (view_x < 0.0) view_x = 0.0;
            if (view_y < 0.0) view_y = 0.0;
            if (view_x > (double)tex_w - src_w) view_x = (double)tex_w - src_w;
            if (view_y > (double)tex_h - src_h) view_y = (double)tex_h - src_h;
          }
        }
      }
      // Time machine strip claims the wheel while open: pan time, Ctrl = zoom.
      if (e.type == SDL_MOUSEWHEEL && timeline.open) {
        if (e.wheel.windowID == main_win_id) {
          int win_w = 0, win_h = 0;
          SDL_GetWindowSize(window, &win_w, &win_h);
          SDL_Rect strip = ui_timeline_rect(win_w, win_h);
          int mx_i = 0, my_i = 0;
          SDL_GetMouseState(&mx_i, &my_i);
          if (SDL_GetModState() & KMOD_CTRL) {
            ui_timeline_zoom(&timeline, mx_i, strip, (int)e.wheel.y);
          } else {
            ui_timeline_pan(&timeline, strip, (int)e.wheel.y);
          }
        }
        continue;
      }
      if (e.type == SDL_MOUSEWHEEL && texture && tex_w > 0 && tex_h > 0 &&
          (view_mode == VIEW_MODE_TILES || matrix_expanded >= 0)) {
        if (e.wheel.windowID != main_win_id) continue;
        int mx_i = 0, my_i = 0;
        SDL_GetMouseState(&mx_i, &my_i);
        last_mouse_x = mx_i;
        last_mouse_y = my_i;
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (win_w > 0 && win_h > 0) {
          if (view_mode == VIEW_MODE_TILES) {
            // If mouse is over the tile bar, mouse wheel scrolls tiles horizontally instead of zooming.
            const int pad = 10;
            const int gap = 10;
            const int tile_w = 220;
            const int tile_h = 124;
            int bar_y = win_h - tile_h - pad;
            if (my_i >= bar_y && my_i <= bar_y + tile_h) {
              // Natural-ish: wheel down => scroll right, wheel up => scroll left.
              int total_w = 0;
              if (camera_count > 0) total_w = camera_count * (tile_w + gap) - gap;
              int view_w = win_w - 2 * pad;
              int max_scroll = total_w - view_w;
              if (max_scroll < 0) max_scroll = 0;

              int delta = (int)(-e.wheel.y) * 80;
              tile_scroll_px += delta;
              if (tile_scroll_px < 0) tile_scroll_px = 0;
              if (tile_scroll_px > max_scroll) tile_scroll_px = max_scroll;
              overlay_until = SDL_GetTicks() + 5000;
              continue;
            }
          }

          double src_w = (double)tex_w / zoom;
          double src_h = (double)tex_h / zoom;

          double mx = (double)last_mouse_x;
          double my = (double)last_mouse_y;
          if (mx < 0) mx = 0;
          if (my < 0) my = 0;
          if (mx > win_w) mx = win_w;
          if (my > win_h) my = win_h;

          double tx = view_x + mx * (src_w / (double)win_w);
          double ty = view_y + my * (src_h / (double)win_h);

          const double step = 1.15;
          if (e.wheel.y > 0) zoom *= step;
          if (e.wheel.y < 0) zoom /= step;
          if (zoom < 1.0) zoom = 1.0;
          if (zoom > 8.0) zoom = 8.0;

          double new_src_w = (double)tex_w / zoom;
          double new_src_h = (double)tex_h / zoom;

          view_x = tx - mx * (new_src_w / (double)win_w);
          view_y = ty - my * (new_src_h / (double)win_h);

          if (view_x < 0.0) view_x = 0.0;
          if (view_y < 0.0) view_y = 0.0;
          if (view_x > (double)tex_w - new_src_w) view_x = (double)tex_w - new_src_w;
          if (view_y > (double)tex_h - new_src_h) view_y = (double)tex_h - new_src_h;

          if (zoom == 1.0) {
            view_x = 0.0;
            view_y = 0.0;
            dragging = 0;
          }
        }
      }
      // A click on the time machine strip seeks + plays; it takes priority over
      // the tile bar (which also lives at the bottom of the window).
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && timeline.open &&
          e.button.windowID == main_win_id) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        SDL_Rect strip = ui_timeline_rect(win_w, win_h);
        if (e.button.x >= strip.x && e.button.x < strip.x + strip.w && e.button.y >= strip.y &&
            e.button.y < strip.y + strip.h) {
          double t = ui_timeline_x_to_time(&timeline, strip, e.button.x);
          time_machine_seek(tmach, t);
          continue;
        }
      }
      if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        if (e.button.windowID != main_win_id) continue;
        Uint32 now = SDL_GetTicks();
        int handled = 0;
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);

        // The PTZ pad claims the click before any tile selection or pan-drag.
        {
          int ci = ptz_pad_camera(cameras, camera_count, stream_from_list, view_mode,
                                  matrix_expanded, active_camera);
          if (ci >= 0) {
            SDL_Rect pad_r;
            SDL_Rect btns[PTZ_DIR_COUNT];
            ptz_pad_layout(win_w, win_h, &pad_r, btns);
            int d = ptz_pad_hit(btns, e.button.x, e.button.y);
            if (d >= 0) {
              PtzTarget t;
              ptz_fill_target(&cameras[ci], &t);
              float vx, vy, vz;
              ptz_dir_velocity(d, &vx, &vy, &vz);
              ptz_move(ptz, &t, vx, vy, vz);
              ptz_active_dir = d;
              continue;
            }
          }
        }

        if (view_mode == VIEW_MODE_TILES) {
          overlay_until = now + 5000;
          const int pad = 10;
          const int gap = 10;
          const int tile_w = 220;
          const int tile_h = 124;
          int x = pad - tile_scroll_px;
          int y = win_h - tile_h - pad;
          for (int i = 0; i < camera_count; i++) {
            int rect_w = tile_w;
            int rect_h = tile_h;
            SDL_Rect r = {x, y, rect_w, rect_h};
            if (e.button.x >= r.x && e.button.x < r.x + r.w && e.button.y >= r.y &&
                e.button.y < r.y + r.h) {
              if (i != active_camera) {
                active_camera = i;
                if (rec_mutex) SDL_LockMutex(rec_mutex);
                rec_want = (camera_count > 0) ? (cameras[active_camera].record_enabled ? 1 : 0) : 0;
                dargs.camera_index = active_camera;
                snprintf(rec_title_buf, sizeof(rec_title_buf), "%s",
                         cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
                if (rec_mutex) SDL_UnlockMutex(rec_mutex);

                // Async: tell the old decoder to die in the background. The new
                // decode is started later by the pending-start path once shared state
                // has been reset. UI doesn't wait — it renders the cached frame for
                // the new camera right below.
                if (th) {
                  request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
                }
                audio_state_clear_queue(&audio);
                if (error_text.tex) {
                  SDL_DestroyTexture(error_text.tex);
                  error_text.tex = NULL;
                }
                has_error = 0;

                // Instant preview: show last cached frame for this camera (if available),
                // otherwise clear the old texture so user doesn't see the previous camera.
                int have_cached = 0;
                int cw = 0;
                int ch = 0;
                if (cameras[active_camera].lock) SDL_LockMutex(cameras[active_camera].lock);
                have_cached =
                    (cameras[active_camera].last_yuv && cameras[active_camera].last_yuv_size > 0 &&
                     cameras[active_camera].last_w > 0 && cameras[active_camera].last_h > 0);
                if (have_cached) {
                  cw = cameras[active_camera].last_w;
                  ch = cameras[active_camera].last_h;
                }
                if (have_cached) {
                  if (!texture || tex_w != cw || tex_h != ch) {
                    if (texture) SDL_DestroyTexture(texture);
                    texture = recreate_texture(renderer, cw, ch);
                    if (!texture) die_sdl("SDL_CreateTexture failed");
                    tex_w = cw;
                    tex_h = ch;
                  }
                  tmp_yuv->format = AV_PIX_FMT_YUV420P;
                  tmp_yuv->width = cw;
                  tmp_yuv->height = ch;
                  int rc = av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize,
                                                cameras[active_camera].last_yuv, AV_PIX_FMT_YUV420P,
                                                cw, ch, 1);
                  if (rc < 0) die_ffmpeg("av_image_fill_arrays failed", rc);
                  if (SDL_UpdateYUVTexture(texture, NULL, tmp_yuv->data[0], tmp_yuv->linesize[0],
                                           tmp_yuv->data[1], tmp_yuv->linesize[1], tmp_yuv->data[2],
                                           tmp_yuv->linesize[2]) != 0) {
                    die_sdl("SDL_UpdateYUVTexture failed");
                  }
                } else {
                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;
                }
                if (cameras[active_camera].lock) SDL_UnlockMutex(cameras[active_camera].lock);

                int reuse_tile_main = stream_tiles_use_main_mode(argc, preview_mode, tiles_use_preview_url, camera_count);
                if (!reuse_tile_main) {
                  // Schedule the new decode to begin once the old thread has finished
                  // unwinding (pending-start path picks it up after th_join_done).
                  pending_main_start_idx = active_camera;
                } else {
                  // Single pipeline: keep using the already-running tile stream (Main URL) for the expanded view.
                  active_url_buf[0] = 0;
                  dargs.url = active_url_buf;
                  pending_main_start_idx = -1;
                }

                stream_from_list = 1;
                if (mgr.is_open) mgr.selected = active_camera;

#ifdef _WIN32
                persist_save_cameras(g_persist, settings_path, cameras, camera_count, active_camera);
#endif

                char title[256];
                snprintf(title, sizeof(title), "Ultralight IP CAM Viewer Recorder - %s",
                         cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
                SDL_SetWindowTitle(window, title);

                if (!reuse_tile_main) {
                  is_loading = 1;
                  loading_since = SDL_GetTicks();
                  destroy_text(&buffering_pct_text);
                  last_buffer_pct = -1;
                  big_main_pending = camera_streamable(cameras, camera_count, active_camera) ? 1 : 0;
                } else {
                  is_loading = 0;
                  retry_at = 0;
                  retry_delay_ms = 1000;
                  destroy_text(&buffering_pct_text);
                  last_buffer_pct = -1;
                  big_main_pending = 0;
                }
                zoom = 1.0;
                view_x = 0.0;
                view_y = 0.0;
                dragging = 0;
                last_frame_ticks = SDL_GetTicks();
                if (!reuse_tile_main) {
                  retry_at = 0;
                  retry_delay_ms = 1000;
                }
              }
              handled = 1;
              break;
            }
            x += rect_w + gap;
          }

                if (!handled && zoom > 1.0) {
            dragging = 1;
            drag_last_x = e.button.x;
            drag_last_y = e.button.y;
          }
        } else if (view_mode == VIEW_MODE_MATRIX) {
          // Matrix mode:
          // - grid view: single click selects, double click expands to live view
          // - expanded view: double click collapses back to grid
          if (camera_count > 0 && matrix_expanded < 0) {
            int rows = 1, cols = 1;
            choose_matrix_grid(camera_count, win_w, win_h, &rows, &cols);
            const int pad = 1;
            const int gap = 1;
            int grid_w = win_w - 2 * pad;
            int grid_h = win_h - 2 * pad;
            if (grid_w < 1) grid_w = 1;
            if (grid_h < 1) grid_h = 1;
            int cell_w = (cols > 0) ? (grid_w - gap * (cols - 1)) / cols : grid_w;
            int cell_h = (rows > 0) ? (grid_h - gap * (rows - 1)) / rows : grid_h;
            if (cell_w < 1) cell_w = 1;
            if (cell_h < 1) cell_h = 1;

            int idx = -1;
            for (int i = 0; i < camera_count; i++) {
              int rr = (cols > 0) ? (i / cols) : 0;
              int cc = (cols > 0) ? (i % cols) : 0;
              int x = pad + cc * (cell_w + gap);
              int y = pad + rr * (cell_h + gap);
              SDL_Rect cell = {x, y, cell_w, cell_h};
              if (e.button.x >= cell.x && e.button.x < cell.x + cell.w && e.button.y >= cell.y &&
                  e.button.y < cell.y + cell.h) {
                idx = i;
                break;
              }
            }

            if (idx >= 0) {
              active_camera = idx;
              if (mgr.is_open) mgr.selected = active_camera;

              int want_matrix_main = 0;
              if (argc < 2 && preview_mode == PREVIEW_MODE_STREAM && view_mode == VIEW_MODE_MATRIX &&
                  camera_count > 1 && matrix_expanded < 0) {
                if (tiles_use_preview_url &&
                    cameras[active_camera].preview_url && cameras[active_camera].preview_url[0] &&
                    cameras[active_camera].url && cameras[active_camera].url[0]) {
                  want_matrix_main = 1;
                }
              }

              if (want_matrix_main) {
                // Start (or switch) main-url buffering for the selected tile. Keep preview streaming running.
                // Grid mode: keep audio muted.
                dargs.audio_enabled = 0;

                if (matrix_main_idx == active_camera && th) {
                  // already buffering/ready for this tile
                  // Do not reset buffering progress on double click (or repeated clicks).
                } else if (!th && !th_join && !camera_streamable(cameras, camera_count, active_camera)) {
                  // Disabled camera: no main-stream prebuffer, nothing to load.
                  matrix_main_loading = 0;
                  matrix_main_idx = -1;
                  pending_main_start_idx = -1;
                  active_url_buf[0] = 0;
                  dargs.url = active_url_buf;
                  is_loading = 0;
                } else if (!th && !th_join) {
                  matrix_main_loading = 1;
                  matrix_main_loading_since = now;

                  // Start immediately (no blocking stop).
                  matrix_main_idx = active_camera;
                  pending_main_start_idx = -1;

                  if (error_text.tex) {
                    SDL_DestroyTexture(error_text.tex);
                    error_text.tex = NULL;
                  }
                  has_error = 0;
                  retry_at = 0;
                  retry_delay_ms = 1000;
                  is_loading = 0;

                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;

                  build_camera_url(cameras[active_camera].type, cameras[active_camera].url, active_url_buf,
                                   sizeof(active_url_buf));
                  dargs.url = active_url_buf;
                  dargs.camera_index = active_camera;
                  expected_keyint_ms =
                      (cameras[active_camera].keyint_ms > 0) ? cameras[active_camera].keyint_ms : 5000;
                  dargs.initial_keyint_ms = expected_keyint_ms;
                  th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", &dargs, &shared.alive);
                  if (!th) die_sdl("SDL_CreateThread failed");
                } else {
                  matrix_main_loading = 1;
                  matrix_main_loading_since = now;

                  // Request async stop and start the latest selection when the old thread is fully gone.
                  pending_main_start_idx = active_camera;
                  matrix_main_idx = active_camera;
                  if (th) {
                    request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
                  }
                }
              } else {
                // No preview URL => tiles already use main URL; don't run a separate main stream in grid mode.
                if (matrix_main_idx >= 0) {
                  pending_main_start_idx = -1;
                  if (th) request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
                  audio_state_clear_queue(&audio);
                  active_url_buf[0] = 0;
                  dargs.url = active_url_buf;
                  matrix_main_idx = -1;
                  matrix_main_loading = 0;
                  retry_at = 0;
                  retry_delay_ms = 1000;
                  is_loading = 0;
                  if (error_text.tex) {
                    SDL_DestroyTexture(error_text.tex);
                    error_text.tex = NULL;
                  }
                  has_error = 0;
                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;
                }
              }

              // Prefer SDL's OS-timed double-click counter (e.button.clicks): it's
              // measured when the click actually happened, so it isn't fooled by the
              // heavy work the first click kicks off (decode-thread spawn, texture
              // teardown) delaying our processing. Keep the manual same-tile timer as
              // a fallback so this only ever adds detections.
              int is_double = (e.button.clicks >= 2) ||
                              (matrix_last_click_idx == idx && (now - matrix_last_click_ticks) <= 350);
              matrix_last_click_idx = idx;
              matrix_last_click_ticks = now;

              if (is_double) {
                matrix_expanded = idx;

                int reuse_tile_main = stream_tiles_use_main_mode(argc, preview_mode, tiles_use_preview_url, camera_count);
                if (reuse_tile_main) {
                  // Tiles already receive Main URL streams; expanding should reuse that stream with no extra buffering.
                  if (th) request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
                  audio_state_clear_queue(&audio);
                  active_url_buf[0] = 0;
                  dargs.url = active_url_buf;
                  matrix_main_idx = -1;
                  matrix_main_loading = 0;
                  pending_main_start_idx = -1;
                  is_loading = 0;
                  retry_at = 0;
                  retry_delay_ms = 1000;
                  big_main_pending = 0;
                  destroy_text(&buffering_pct_text);
                  last_buffer_pct = -1;
                  if (error_text.tex) {
                    SDL_DestroyTexture(error_text.tex);
                    error_text.tex = NULL;
                  }
                  has_error = 0;
                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;
                } else {
                  // Start live decode for the selected camera (reuse existing single-camera pipeline).
                  // decode.c is the single audio source — one RTSP session, video+audio.
                  dargs.audio_enabled = 1;
                  SDL_AtomicSet(&audio_playing_sticky, 0);
                  if (rec_mutex) SDL_LockMutex(rec_mutex);
                  rec_want = (camera_count > 0) ? (cameras[active_camera].record_enabled ? 1 : 0) : 0;
                  dargs.camera_index = active_camera;
                  snprintf(rec_title_buf, sizeof(rec_title_buf), "%s",
                           cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
                  if (rec_mutex) SDL_UnlockMutex(rec_mutex);

                  // If grid buffering is already running for this camera, reuse it; otherwise start now.
                  int need_start = (th == NULL) ? 1 : 0;
                  if (matrix_main_idx != active_camera) need_start = 1;
                  if (!camera_streamable(cameras, camera_count, active_camera)) {
                    // Disabled camera: tear any session down and stay dark.
                    need_start = 0;
                    stop_decode_thread(&shared, &th);
                    active_url_buf[0] = 0;
                    dargs.url = active_url_buf;
                    is_loading = 0;
                  }
                  if (need_start) {
                    stop_decode_thread(&shared, &th);
                    audio_state_clear_queue(&audio);
                    if (error_text.tex) {
                      SDL_DestroyTexture(error_text.tex);
                      error_text.tex = NULL;
                    }
                    has_error = 0;

                    if (texture) SDL_DestroyTexture(texture);
                    texture = NULL;
                    tex_w = 0;
                    tex_h = 0;

                    build_camera_url(cameras[active_camera].type, cameras[active_camera].url, active_url_buf,
                                     sizeof(active_url_buf));
                    dargs.url = active_url_buf;
                    expected_keyint_ms =
                        (cameras[active_camera].keyint_ms > 0) ? cameras[active_camera].keyint_ms : 5000;
                    dargs.initial_keyint_ms = expected_keyint_ms;
                    th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", &dargs, &shared.alive);
                    if (!th) die_sdl("SDL_CreateThread failed");
                  }
                }

                // Instant preview from cache (if available).
                int have_cached = 0;
                int cw = 0;
                int ch = 0;
                if (cameras[active_camera].lock) SDL_LockMutex(cameras[active_camera].lock);
                have_cached =
                    (cameras[active_camera].last_yuv && cameras[active_camera].last_yuv_size > 0 &&
                     cameras[active_camera].last_w > 0 && cameras[active_camera].last_h > 0);
                if (have_cached) {
                  cw = cameras[active_camera].last_w;
                  ch = cameras[active_camera].last_h;
                }
                if (have_cached) {
                  if (!texture || tex_w != cw || tex_h != ch) {
                    if (texture) SDL_DestroyTexture(texture);
                    texture = recreate_texture(renderer, cw, ch);
                    if (!texture) die_sdl("SDL_CreateTexture failed");
                    tex_w = cw;
                    tex_h = ch;
                  }
                  tmp_yuv->format = AV_PIX_FMT_YUV420P;
                  tmp_yuv->width = cw;
                  tmp_yuv->height = ch;
                  int rc = av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize,
                                                cameras[active_camera].last_yuv, AV_PIX_FMT_YUV420P,
                                                cw, ch, 1);
                  if (rc < 0) die_ffmpeg("av_image_fill_arrays failed", rc);
                  if (SDL_UpdateYUVTexture(texture, NULL, tmp_yuv->data[0], tmp_yuv->linesize[0],
                                           tmp_yuv->data[1], tmp_yuv->linesize[1], tmp_yuv->data[2],
                                           tmp_yuv->linesize[2]) != 0) {
                    die_sdl("SDL_UpdateYUVTexture failed");
                  }
                } else {
                  if (texture) SDL_DestroyTexture(texture);
                  texture = NULL;
                  tex_w = 0;
                  tex_h = 0;
                }
                if (cameras[active_camera].lock) SDL_UnlockMutex(cameras[active_camera].lock);

                stream_from_list = 1;

#ifdef _WIN32
                persist_save_cameras(g_persist, settings_path, cameras, camera_count, active_camera);
#endif

                char title[256];
                snprintf(title, sizeof(title), "Ultralight IP CAM Viewer Recorder - %s",
                 cameras[active_camera].title_utf8 ? cameras[active_camera].title_utf8 : "Camera");
                SDL_SetWindowTitle(window, title);

                // A disabled camera never loads: without this it would sit on a
                // spinner forever, since no decoder will ever produce a frame.
                is_loading = (texture != NULL ||
                              !camera_streamable(cameras, camera_count, active_camera))
                                 ? 0
                                 : 1;
                loading_since = now;
                destroy_text(&buffering_pct_text);
                last_buffer_pct = -1;
                zoom = 1.0;
                view_x = 0.0;
                view_y = 0.0;
                dragging = 0;
                last_frame_ticks = SDL_GetTicks();
                retry_at = 0;
                retry_delay_ms = 1000;
                matrix_main_idx = active_camera;
                matrix_main_loading = 0;
                // Even with a cached substream still on screen (is_loading forced 0
                // above), the main stream is only now connecting — show the notice.
                if (!stream_tiles_use_main_mode(argc, preview_mode, tiles_use_preview_url, camera_count)) {
                  big_main_pending = camera_streamable(cameras, camera_count, active_camera) ? 1 : 0;
                }
              } else {
                // Single click: just select/highlight.
                handled = 1;
              }
            }
          } else if (matrix_expanded >= 0) {
            int is_double = (e.button.clicks >= 2) ||
                            (matrix_last_click_idx == active_camera && (now - matrix_last_click_ticks) <= 350);
            matrix_last_click_idx = active_camera;
            matrix_last_click_ticks = now;
            if (is_double) {
              // Collapse back to grid: stop live decode and render only cached thumbnails.
              matrix_expanded = -1;
              // Grid mode: keep audio muted.
              dargs.audio_enabled = 0;

              int keep_main_in_grid = 0;
              if (argc < 2 && preview_mode == PREVIEW_MODE_STREAM && camera_count > 1) {
                if (active_camera >= 0 && active_camera < camera_count && cameras[active_camera].preview_url &&
                    cameras[active_camera].preview_url[0]) {
                  keep_main_in_grid = 1;
                }
              }

              if (keep_main_in_grid) {
                matrix_main_idx = active_camera;
                matrix_main_loading = is_loading ? 1 : 0;
                matrix_main_loading_since = loading_since;
                // Hide fullscreen buffering/error overlays in grid mode.
                is_loading = 0;
                if (error_text.tex) {
                  SDL_DestroyTexture(error_text.tex);
                  error_text.tex = NULL;
                }
                has_error = 0;
              } else {
                stop_decode_thread(&shared, &th);
                audio_state_clear_queue(&audio);
                active_url_buf[0] = 0;
                dargs.url = active_url_buf;
                matrix_main_idx = -1;
                matrix_main_loading = 0;
                is_loading = 0;
                retry_at = 0;
                retry_delay_ms = 1000;
                if (error_text.tex) {
                  SDL_DestroyTexture(error_text.tex);
                  error_text.tex = NULL;
                }
                has_error = 0;
                if (texture) SDL_DestroyTexture(texture);
                texture = NULL;
                tex_w = 0;
                tex_h = 0;
              }

              dragging = 0;
              zoom = 1.0;
              view_x = 0.0;
              view_y = 0.0;
              handled = 1;
              continue;
            }
 
            if (zoom > 1.0) {
              dragging = 1;
              drag_last_x = e.button.x;
              drag_last_y = e.button.y;
            }
          }
        }
      }
      if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if (e.button.windowID != main_win_id) continue;
        dragging = 0;
      }
    }

    // Phase 2: spawn / supervise per-camera threads. Runs once per main-loop iteration
    // (not per SDL event) so an idle UI with no events still drives initial fan-out and
    // crash-respawn. spawn_step is a no-op for slots that are already alive and inside
    // their backoff window, so calling it every tick is cheap.
    {
      static Uint32 last_spawn_ticks = 0;
      if ((Uint32)(loop_ticks - last_spawn_ticks) >= 25u) {
        if (warm_threads && warm_args) {
          warm_cams_spawn_step(warm_threads, warm_args, warm_count, &warm_spawn_next, 1);
          if (warm_spawning && warm_spawn_next >= warm_count) warm_spawning = 0;
        }
        if (tile_threads && tile_args) {
          tile_streams_spawn_step(tile_threads, tile_args, tile_count, &tile_spawn_next, 1);
          if (tile_spawning && tile_spawn_next >= tile_count) tile_spawning = 0;
        }
        last_spawn_ticks = loop_ticks;
      }
    }

    // Compute after event processing so it reflects settings changes done in this loop.
    stream_tiles_use_main = stream_tiles_use_main_mode(argc, preview_mode, tiles_use_preview_url, camera_count);
    // In "Stream tiles source: Main URL" mode, ask the active tile thread to decode full-res when we are in
    // a single-camera view. This keeps quality high without opening a second main stream.
    if (stream_tiles_use_main && (view_mode == VIEW_MODE_TILES || matrix_expanded >= 0)) {
      SDL_AtomicSet(&tile_hires_index, active_camera);
    } else {
      SDL_AtomicSet(&tile_hires_index, -1);
    }

    // Hard guarantee: when Stream mode is enabled and tiles use the Main URL, the per-tile threads are
    // already receiving the main stream. Never start or keep a separate main decode thread in this mode,
    // and never show "buffering" for it.
    if (stream_tiles_use_main) {
      if (th) {
        request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
      }
      if (th_join) {
        // Joining happens asynchronously below; keep UI state clean in the meantime.
      }
      audio_state_clear_queue(&audio);
      active_url_buf[0] = 0;
      dargs.url = active_url_buf;
      is_loading = 0;
      big_main_pending = 0;
      retry_at = 0;
      retry_delay_ms = 1000;
      if (error_text.tex) {
        SDL_DestroyTexture(error_text.tex);
        error_text.tex = NULL;
      }
      has_error = 0;
      destroy_text(&buffering_pct_text);
      last_buffer_pct = -1;
    }

    // Non-blocking decoder shutdown: reap the background joiner when it's done.
    if (th_join && SDL_AtomicGet(&th_join_done)) {
      SDL_WaitThread(th_join, NULL);
      th_join = NULL;
      SDL_AtomicSet(&th_join_done, 0);
      SDL_AtomicSet(&shared.quit, 0);
      clear_shared(&shared);
    }

#ifdef _WIN32
    // Reap the startup cache loader as soon as it finishes on its own, so the
    // camera-manager edit paths almost never have to block-join it. SDL_WaitThread
    // on an already-exited thread returns immediately. The seen_alive guard avoids
    // the brief create→entry window where alive is still 0.
    if (startup_cache_th) {
      if (SDL_AtomicGet(&startup_cache_alive)) {
        startup_cache_seen_alive = 1;
      } else if (startup_cache_seen_alive) {
        SDL_WaitThread(startup_cache_th, NULL);
        startup_cache_th = NULL;
        SDL_AtomicSet(&startup_cache_quit, 0);
        startup_cache_seen_alive = 0;
      }
    }
#endif

    // Start a pending decode once no thread is in flight. Decoupled from the join
    // path so it also fires when the click happened with th==NULL (e.g. first open).
    if (!th && !th_join && pending_main_start_idx >= 0 && camera_count > 0 &&
        pending_main_start_idx < camera_count && argc < 2) {
      int idx = pending_main_start_idx;
      int want_start = 0;
      if (!camera_streamable(cameras, camera_count, idx)) {
        // Disabled camera: drop the request instead of opening a session, and
        // clear the prebuffer state a previous camera may have left set — no
        // decoder will run to clear it later.
        pending_main_start_idx = -1;
        matrix_main_loading = 0;
        matrix_main_idx = -1;
        active_url_buf[0] = 0;
        dargs.url = active_url_buf;
        is_loading = 0;
        big_main_pending = 0;
      } else if (view_mode == VIEW_MODE_MATRIX) {
        if (matrix_expanded >= 0) {
          want_start = 1;
        } else if (preview_mode == PREVIEW_MODE_STREAM && camera_count > 1) {
          want_start = 1;  // grid pre-buffer for the selected tile
        }
      } else if (view_mode == VIEW_MODE_TILES) {
        // Tiles-strip click async-restart path. If we're in stream-tiles-use-main mode
        // the tile_stream worker already serves the active camera, no decode needed.
        if (!stream_tiles_use_main_mode(argc, preview_mode, tiles_use_preview_url, camera_count)) {
          want_start = 1;
        }
      }

      if (want_start) {
        pending_main_start_idx = -1;
        build_camera_url(cameras[idx].type, cameras[idx].url, active_url_buf, sizeof(active_url_buf));
        dargs.url = active_url_buf;
        dargs.camera_index = idx;
        expected_keyint_ms = (cameras[idx].keyint_ms > 0) ? cameras[idx].keyint_ms : 5000;
        dargs.initial_keyint_ms = expected_keyint_ms;
        th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", &dargs, &shared.alive);
        if (!th) die_sdl("SDL_CreateThread failed");
      } else {
        pending_main_start_idx = -1;
      }
    }

    int w = 0, h = 0, decode_err = 0;
    // Non-blocking peek at the decoder's published state. We deliberately do NOT
    // wait for a frame here: blocking the UI thread on the decoder (the old
    // SDL_CondWaitTimeout) dragged the whole interface to the decoder's cadence and
    // collapsed it to ~4 fps whenever a stream stalled. Instead we read what's
    // available and keep rendering the last good frame; the actual frame hand-off
    // happens in the has_frame block below. Pacing comes from PRESENTVSYNC on render
    // and the SDL_Delay(1) idle branch at the bottom of the loop.
    SDL_LockMutex(shared.mutex);
    decode_err = shared.decode_err;
    w = shared.w;
    h = shared.h;
    SDL_UnlockMutex(shared.mutex);

    // Phase 2: if the decode thread died silently (caught crash via SEH; the wrapper
    // cleared shared.alive but no error was reported), treat it like a decode error so
    // the existing retry path takes over instead of the UI stalling on the stale frame.
    if (th && !decode_err && !SDL_AtomicGet(&shared.alive)) {
      decode_err = AVERROR_EXTERNAL;
    }

    int in_matrix_grid_now = 0;
    if (argc < 2 && view_mode == VIEW_MODE_MATRIX && camera_count > 1 && matrix_expanded < 0) {
      in_matrix_grid_now = 1;
    }
    int live_view = in_matrix_grid_now ? 0 : 1;
    // decode.c handles audio for the single-camera/expanded views — one RTSP
    // session per camera, video + audio multiplexed (like ffplay). Matrix grid
    // (in_matrix_grid_now=1) has no decode.c thread; audio is unavailable there
    // until tile_stream.c grows audio support.
    dargs.audio_enabled = in_matrix_grid_now ? 0 : 1;

    // Audio mutes while a modal overlay is open. decode.c (single-camera or
    // matrix-expanded) and tile_stream.c (matrix grid) are the audio producers.
    // We gate the queue via volume=0 instead of tearing down threads.
    {
      int audio_blocked_by_overlay = (intro.is_open || settings.is_open ||
                                      plugins_ui.is_open || mgr.is_open);
      int effective_vol = audio_blocked_by_overlay ? 0 : audio_volume_pct;
      SDL_AtomicSet(&audio_volume_pct_atomic, effective_vol);
    }

    // Pick the camera index that should be producing audio right now, across
    // all view modes. When the choice changes, clear the sticky icon flag so
    // it doesn't carry over to a camera with no audio track (e.g. MJPEG).
    //
    // Producer rules:
    //   - tile_stream.c produces when audio_active_idx_atomic == its index
    //   - decode.c produces when audio_active_idx_atomic == -1
    // So we set audio_active_idx to the camera index whenever tile_stream is
    // the one actually running for that camera (matrix-grid always, plus
    // matrix-expanded when we're reusing the tile_stream's main feed), and
    // to -1 when decode.c is running (single-camera TILES view, or the
    // non-reuse expand path that spawns a fresh decode.c thread).
    {
      int blocked = (intro.is_open || settings.is_open ||
                     plugins_ui.is_open || mgr.is_open);
      int want_tile = -1;
      int want_src  = -1;
      int decode_running = (th != NULL);
      if (!blocked && camera_count > 0) {
        if (in_matrix_grid_now && active_camera >= 0 && active_camera < camera_count) {
          want_tile = active_camera;
          want_src  = active_camera;
        } else if (view_mode == VIEW_MODE_MATRIX && matrix_expanded >= 0 &&
                   matrix_expanded < camera_count) {
          want_src = matrix_expanded;
          if (!decode_running) want_tile = matrix_expanded;  // tile_stream owns
        } else if (active_camera >= 0 && active_camera < camera_count) {
          want_src = active_camera;
          if (!decode_running) want_tile = active_camera;
        }
      } else if (argc >= 2 && !blocked) {
        want_src = 0;
      }
      SDL_AtomicSet(&audio_active_idx_atomic, want_tile);
      if (want_src != last_audio_src_idx) {
        SDL_AtomicSet(&audio_playing_sticky, 0);
        last_audio_src_idx = want_src;
      }
    }

    if (decode_err < 0) {
      if (live_view && !has_error) {
        error_text = make_text(renderer, tr(STR_CAMERA_UNAVAILABLE), text_pt, text_color);
        if (!error_text.tex) {
          // Fallback: avoid crashing if text rendering fails for any reason.
          has_error = 1;
        } else {
          has_error = 1;
        }
        is_loading = 0;
        stop_decode_thread(&shared, &th);
        retry_at = SDL_GetTicks() + (Uint32)retry_delay_ms;
        if (retry_delay_ms < 8000) retry_delay_ms *= 2;
      } else if (!live_view) {
        // Grid prebuffer: fail silently (keep preview tiles).
        if (th) request_stop_decode_thread_async(&shared, &th, &th_join, &th_join_done, &th_join_rc);
        audio_state_clear_queue(&audio);
        active_url_buf[0] = 0;
        dargs.url = active_url_buf;
        matrix_main_idx = -1;
        matrix_main_loading = 0;
        pending_main_start_idx = -1;
        if (error_text.tex) {
          SDL_DestroyTexture(error_text.tex);
          error_text.tex = NULL;
        }
        has_error = 0;
        if (texture) SDL_DestroyTexture(texture);
        texture = NULL;
        tex_w = 0;
        tex_h = 0;
      }
    }

    // Watchdog: if stream stalls while a decoder thread is running, restart it.
      if (live_view && th && !is_loading && !has_error && texture) {
        Uint32 now = SDL_GetTicks();
        if (now - last_frame_ticks > stall_ms) {
          stop_decode_thread(&shared, &th);
          audio_state_clear_queue(&audio);
          is_loading = 1;
          loading_since = now;
          destroy_text(&buffering_pct_text);
          last_buffer_pct = -1;
          retry_at = now + 250;
        }
      }

    // Auto-reconnect loop (camera offline / network drop).
    if (live_view && active_url_buf[0] && !th && retry_at) {
      Uint32 now = SDL_GetTicks();
      if (now >= retry_at) {
        if (error_text.tex) {
          SDL_DestroyTexture(error_text.tex);
          error_text.tex = NULL;
        }
        has_error = 0;
        is_loading = 1;
        loading_since = now;
        destroy_text(&buffering_pct_text);
        last_buffer_pct = -1;

        audio_state_clear_queue(&audio);
        clear_shared(&shared);
        expected_keyint_ms = (camera_count > 0 && active_camera >= 0 && active_camera < camera_count &&
                              cameras[active_camera].keyint_ms > 0)
                                 ? cameras[active_camera].keyint_ms
                                 : 5000;
        dargs.initial_keyint_ms = expected_keyint_ms;
        dargs.url = active_url_buf;
        th = seh_create_thread(decode_thread_fn, "ffmpeg-decode", &dargs, &shared.alive);
        if (!th) die_sdl("SDL_CreateThread failed");
        retry_at = 0;
      }
    }

    if (w > 0 && h > 0 && (w != tex_w || h != tex_h)) {
      if (texture) SDL_DestroyTexture(texture);
      texture = recreate_texture(renderer, w, h);
      if (!texture) die_sdl("SDL_CreateTexture failed");
      tex_w = w;
      tex_h = h;
      zoom = 1.0;
      view_x = 0.0;
      view_y = 0.0;
    }

    int updated = 0;
    int frame_w = 0, frame_h = 0;
    int frame_size = 0;
    SDL_LockMutex(shared.mutex);
    if (shared.has_frame && shared.yuv && shared.yuv_size > 0) {
      frame_w = shared.w;
      frame_h = shared.h;
      frame_size = shared.yuv_size;
      if (frame_size > frame_copy_size) {
        uint8_t *nb = (uint8_t *)SDL_realloc(frame_copy, (size_t)frame_size);
        if (!nb) {
          SDL_UnlockMutex(shared.mutex);
          fprintf(stderr, "Out of memory\n");
          return 1;
        }
        frame_copy = nb;
        frame_copy_size = frame_size;
      }
      memcpy(frame_copy, shared.yuv, (size_t)frame_size);
      shared.has_frame = 0;
      updated = 1;
    }
    SDL_UnlockMutex(shared.mutex);

      if (updated && texture && frame_copy && frame_w > 0 && frame_h > 0) {
        tmp_yuv->format = AV_PIX_FMT_YUV420P;
        tmp_yuv->width = frame_w;
        tmp_yuv->height = frame_h;
        int rc = av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize, frame_copy,
                                      AV_PIX_FMT_YUV420P, frame_w, frame_h, 1);
        if (rc < 0) die_ffmpeg("av_image_fill_arrays failed", rc);

      RtspVideoFrameYuv420p vf = {0};
      vf.size = (uint32_t)sizeof(vf);
      vf.y = tmp_yuv->data[0];
      vf.u = tmp_yuv->data[1];
      vf.v = tmp_yuv->data[2];
      vf.y_stride = tmp_yuv->linesize[0];
      vf.u_stride = tmp_yuv->linesize[1];
      vf.v_stride = tmp_yuv->linesize[2];
      vf.w = frame_w;
      vf.h = frame_h;
      vf.pts_ms = 0;
      if (argc < 2 && stream_from_list && camera_count > 0 && active_camera >= 0 &&
          active_camera < camera_count) {
        vf.camera_index = active_camera;
        vf.camera_name_utf8 = cameras[active_camera].title_utf8;
      } else {
        vf.camera_index = -1;
      }
      vf.on_screen = 1;
      last_display_feed_ticks = SDL_GetTicks();
      plugin_host_on_video_frame(&plugins, &vf, (zoom > 1.0) ? 1 : 0);

       if (SDL_UpdateYUVTexture(texture, NULL, tmp_yuv->data[0], tmp_yuv->linesize[0],
                                tmp_yuv->data[1], tmp_yuv->linesize[1],
                                tmp_yuv->data[2], tmp_yuv->linesize[2]) != 0) {
         die_sdl("SDL_UpdateYUVTexture failed");
       }

        // Cache the last good frame for instant preview when switching cameras.
        if (argc < 2 && stream_from_list && camera_count > 0 && active_camera >= 0 &&
            active_camera < camera_count) {
          if (cameras[active_camera].lock) SDL_LockMutex(cameras[active_camera].lock);
          camera_cache_update(&cameras[active_camera], frame_copy, frame_size, frame_w, frame_h);
          if (cameras[active_camera].lock) SDL_UnlockMutex(cameras[active_camera].lock);
        }
      }

    if (updated && is_loading) {
      is_loading = 0;
      last_buffer_pct = -1;
      destroy_text(&buffering_pct_text);

      if (argc < 2 && stream_from_list && camera_count > 0 && active_camera >= 0 && active_camera < camera_count) {
        SDL_LockMutex(shared.mutex);
        int est = shared.keyint_ms_est;
        SDL_UnlockMutex(shared.mutex);
        if (est > 0 && est <= 60000) cameras[active_camera].keyint_ms = est;
      }
    }

    if (updated && matrix_main_loading) {
      matrix_main_loading = 0;
      destroy_text(&matrix_buffering_pct_text);
      matrix_last_buffer_pct = -1;
    }

    if (updated) {
      big_main_pending = 0;
      last_frame_ticks = SDL_GetTicks();
      retry_delay_ms = 1000;
    }

    // Background plugin feed: hand every enabled camera's freshest tile frame to the
    // plugins (throttled to ~5 fps per camera), so face detection runs on all cameras,
    // not only the one on screen. The displayed camera keeps its full-rate on_screen
    // feed from the render paths above; it's only fed here if that path went quiet
    // (e.g. its main stream is still connecting).
    camera_plugin_feed_set_wanted(argc < 2 && camera_count > 0 && plugin_host_any_enabled(&plugins));
    if (argc < 2 && camera_ui && camera_count > 0 && plugin_host_any_enabled(&plugins)) {
      Uint32 fnow = SDL_GetTicks();
      for (int i = 0; i < camera_count; i++) {
        if (!cameras[i].enabled) continue;
        if (i == active_camera && last_display_feed_ticks && fnow - last_display_feed_ticks < 700)
          continue;
        if (camera_ui[i].plugin_feed_ticks && fnow - camera_ui[i].plugin_feed_ticks < 200) continue;
        int copied = 0;
        int fcw = 0, fch = 0;
        Uint32 fstamp = 0;
        if (cameras[i].lock) SDL_LockMutex(cameras[i].lock);
        // Prefer the full-res plugin feed (tile worker, ~5 fps). The 320x180 thumb is
        // only a fallback for the first moments before the feed buffer exists - faces
        // in it are too small for the detector.
        const uint8_t *fsrc = NULL;
        int fneed = 0;
        if (cameras[i].feed_ticks && cameras[i].feed_ticks != camera_ui[i].plugin_feed_stamp &&
            cameras[i].feed_yuv && cameras[i].feed_yuv_size > 0 && cameras[i].feed_w > 0 && cameras[i].feed_h > 0 &&
            fnow - cameras[i].feed_ticks < 2000) {
          fsrc = cameras[i].feed_yuv;
          fneed = cameras[i].feed_yuv_size;
          fcw = cameras[i].feed_w;
          fch = cameras[i].feed_h;
          fstamp = cameras[i].feed_ticks;
        } else if (!cameras[i].feed_ticks && cameras[i].thumb_last_update_ticks &&
                   cameras[i].thumb_last_update_ticks != camera_ui[i].plugin_feed_stamp &&
                   !cameras[i].thumb_from_cache && cameras[i].thumb_yuv && cameras[i].thumb_yuv_size > 0 &&
                   cameras[i].thumb_w > 0 && cameras[i].thumb_h > 0) {
          fsrc = cameras[i].thumb_yuv;
          fneed = cameras[i].thumb_yuv_size;
          fcw = cameras[i].thumb_w;
          fch = cameras[i].thumb_h;
          fstamp = cameras[i].thumb_last_update_ticks;
        }
        if (fsrc) {
          int need = fneed;
          if (need > plugin_feed_cap) {
            uint8_t *nb = (uint8_t *)SDL_realloc(plugin_feed_buf, (size_t)need);
            if (nb) {
              plugin_feed_buf = nb;
              plugin_feed_cap = need;
            }
          }
          if (plugin_feed_buf && plugin_feed_cap >= need) {
            memcpy(plugin_feed_buf, fsrc, (size_t)need);
            copied = 1;
          }
        }
        if (cameras[i].lock) SDL_UnlockMutex(cameras[i].lock);
        if (!copied) continue;
        camera_ui[i].plugin_feed_stamp = fstamp;
        camera_ui[i].plugin_feed_ticks = fnow;
        tmp_yuv->format = AV_PIX_FMT_YUV420P;
        tmp_yuv->width = fcw;
        tmp_yuv->height = fch;
        if (av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize, plugin_feed_buf,
                                 AV_PIX_FMT_YUV420P, fcw, fch, 1) >= 0) {
          RtspVideoFrameYuv420p vf = {0};
          vf.size = (uint32_t)sizeof(vf);
          vf.y = tmp_yuv->data[0];
          vf.u = tmp_yuv->data[1];
          vf.v = tmp_yuv->data[2];
          vf.y_stride = tmp_yuv->linesize[0];
          vf.u_stride = tmp_yuv->linesize[1];
          vf.v_stride = tmp_yuv->linesize[2];
          vf.w = fcw;
          vf.h = fch;
          vf.pts_ms = 0;
          vf.camera_index = i;
          vf.camera_name_utf8 = cameras[i].title_utf8;
          // The active camera only reaches this pass when no display path fed it (grid view,
          // or its main stream still connecting): then this IS its on-screen feed.
          vf.on_screen = (i == active_camera) ? 1 : 0;
          plugin_host_on_video_frame(&plugins, &vf, 0);
        }
      }
    }

    if (argc < 2 && camera_count == 0) {
      if (!no_cameras_text.tex) {
        no_cameras_text = make_text(renderer, tr(STR_NO_CAMERAS), text_pt, text_color);
      }
    } else {
      if (no_cameras_text.tex) destroy_text(&no_cameras_text);
    }

    int in_matrix_grid = 0;
    if (argc < 2 && view_mode == VIEW_MODE_MATRIX && camera_count > 1 && matrix_expanded < 0) {
      in_matrix_grid = 1;
    }

    // Tick the render loop when the active tile thumbnail updates and it is (or may be) what the
    // big view shows: tiles-stream-main mode, or any big view whose main texture isn't ready yet
    // (the thumbnail is its live fallback while the main stream connects).
    if (!in_matrix_grid && (stream_tiles_use_main || !texture) && camera_ui && active_camera >= 0 &&
        active_camera < camera_count) {
      int thumb_dirty = 0;
      SDL_mutex *lk = cameras[active_camera].lock;
      if (lk) SDL_LockMutex(lk);
      thumb_dirty = (cameras[active_camera].thumb_last_update_ticks &&
                     cameras[active_camera].thumb_last_update_ticks != camera_ui[active_camera].thumb_stamp &&
                     cameras[active_camera].thumb_yuv && cameras[active_camera].thumb_yuv_size > 0)
                         ? 1
                         : 0;
      if (lk) SDL_UnlockMutex(lk);
      if (thumb_dirty) updated = 1;
    }

    // In "Stream tiles source: Main URL" mode, the active tile thread also refreshes the full-res cache.
    // Upload it to the main view texture without blocking decode threads (copy under the camera's
    // own lock, upload unlocked). This is the only frame source for the big view in that mode, so
    // plugins are fed from here as well (the `shared` path above never fires). In the matrix grid the
    // frame is still handed to plugins (for the face indicator on the selected tile) but not uploaded.
    if (stream_tiles_use_main && camera_ui && active_camera >= 0 && active_camera < camera_count) {
      int copied = 0;
      int cw = 0, ch = 0;
      Uint32 cstamp = 0;
      SDL_mutex *lk = cameras[active_camera].lock;
      if (lk) SDL_LockMutex(lk);
      if (cameras[active_camera].last_seen_ticks && cameras[active_camera].last_seen_ticks != camera_ui[active_camera].main_stamp &&
          cameras[active_camera].last_yuv && cameras[active_camera].last_yuv_size > 0 && cameras[active_camera].last_w > 0 &&
          cameras[active_camera].last_h > 0) {
        copied = camera_ui_copy_main(&camera_ui[active_camera], &cameras[active_camera]);
      }
      if (lk) SDL_UnlockMutex(lk);
      if (copied) {
        cw = camera_ui[active_camera].main_copy_w;
        ch = camera_ui[active_camera].main_copy_h;
        cstamp = camera_ui[active_camera].main_copy_stamp;
        if (cw > 0 && ch > 0) {
          tmp_yuv->format = AV_PIX_FMT_YUV420P;
          tmp_yuv->width = cw;
          tmp_yuv->height = ch;
          int rc = av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize, camera_ui[active_camera].main_copy,
                                        AV_PIX_FMT_YUV420P, cw, ch, 1);
          if (rc >= 0) {
            RtspVideoFrameYuv420p vf = {0};
            vf.size = (uint32_t)sizeof(vf);
            vf.y = tmp_yuv->data[0];
            vf.u = tmp_yuv->data[1];
            vf.v = tmp_yuv->data[2];
            vf.y_stride = tmp_yuv->linesize[0];
            vf.u_stride = tmp_yuv->linesize[1];
            vf.v_stride = tmp_yuv->linesize[2];
            vf.w = cw;
            vf.h = ch;
            vf.pts_ms = 0;
            vf.camera_index = active_camera;
            vf.camera_name_utf8 = cameras[active_camera].title_utf8;
            vf.on_screen = 1;
            last_display_feed_ticks = SDL_GetTicks();
            plugin_host_on_video_frame(&plugins, &vf, (!in_matrix_grid && zoom > 1.0) ? 1 : 0);

            if (!in_matrix_grid) {
              if (!texture || tex_w != cw || tex_h != ch) {
                if (texture) SDL_DestroyTexture(texture);
                texture = recreate_texture(renderer, cw, ch);
                if (!texture) die_sdl("SDL_CreateTexture failed");
                tex_w = cw;
                tex_h = ch;
              }
              if (SDL_UpdateYUVTexture(texture, NULL, tmp_yuv->data[0], tmp_yuv->linesize[0], tmp_yuv->data[1],
                                       tmp_yuv->linesize[1], tmp_yuv->data[2], tmp_yuv->linesize[2]) != 0) {
                die_sdl("SDL_UpdateYUVTexture failed");
              }
              updated = 1;
            }
            camera_ui[active_camera].main_stamp = cstamp;
          }
        }
      }
    }

    // A big view fed by the tile pipeline (Stream tiles + Main URL source) has
    // texture == NULL and is_loading == 0 while it waits for the camera's tile
    // worker to deliver the first frame. For an offline camera that frame never
    // arrives — and without this term the gate goes all-false and the app stops
    // presenting entirely: frozen spinners, every stream apparently dead, while
    // the event loop runs fine. Keep repainting whenever a big view has no
    // frame yet (VSync paces it, so this costs nothing).
    int big_view_waiting = (!in_matrix_grid && argc < 2 && camera_count > 0 && texture == NULL);

    if (updated || texture || is_loading || has_error || no_cameras_text.tex || in_matrix_grid ||
        big_view_waiting || intro.is_open || settings.is_open || plugins_ui.is_open ||
        mgr.is_open || timeline.open) {
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);
      Uint32 now = SDL_GetTicks();
      int showing_thumb_main = 0;

      if (in_matrix_grid) {
        int text_budget = 4;
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);

        int rows = 1, cols = 1;
            choose_matrix_grid(camera_count, win_w, win_h, &rows, &cols);

            const int pad = 1;
            const int gap = 1;
            int grid_w = win_w - 2 * pad;
            int grid_h = win_h - 2 * pad;
            if (grid_w < 1) grid_w = 1;
            if (grid_h < 1) grid_h = 1;

        int cell_w = (cols > 0) ? (grid_w - gap * (cols - 1)) / cols : grid_w;
        int cell_h = (rows > 0) ? (grid_h - gap * (rows - 1)) / rows : grid_h;
        if (cell_w < 1) cell_w = 1;
        if (cell_h < 1) cell_h = 1;

        for (int i = 0; i < camera_count; i++) {
          int r = (cols > 0) ? (i / cols) : 0;
          int c = (cols > 0) ? (i % cols) : 0;
          int x = pad + c * (cell_w + gap);
          int y = pad + r * (cell_h + gap);
          SDL_Rect cell = {x, y, cell_w, cell_h};

          SDL_Color bg = {0, 0, 0, 140};
          SDL_Color border = {255, 255, 255, 120};
          if (i == active_camera) {
            bg = (SDL_Color){0, 0, 0, 180};
            border = (SDL_Color){255, 255, 255, 190};
          }
          SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
          SDL_RenderFillRect(renderer, &cell);

          int drew_main = 0;
          if (i == active_camera && matrix_main_idx == active_camera && !matrix_main_loading && th && texture &&
              tex_w > 0 && tex_h > 0) {
            SDL_RenderCopy(renderer, texture, NULL, &cell);
            drew_main = 1;
          }

          if (!drew_main) {
            // Thumbnail (cached). If absent, keep the black tile.
            int copied = 0;
            int has_feed = 0;
            if (cameras[i].lock) SDL_LockMutex(cameras[i].lock);
            if (cameras[i].thumb_last_update_ticks &&
                cameras[i].thumb_last_update_ticks != camera_ui[i].thumb_stamp &&
                cameras[i].thumb_yuv && cameras[i].thumb_yuv_size > 0) {
              copied = camera_ui_copy_thumb(&camera_ui[i], &cameras[i]);
            }
            has_feed = cameras[i].feed_ticks ? 1 : 0;
            if (cameras[i].lock) SDL_UnlockMutex(cameras[i].lock);
            if (copied) {
              camera_ui_update_thumb(renderer, &camera_ui[i], camera_ui[i].thumb_copy, camera_ui[i].thumb_copy_w,
                                     camera_ui[i].thumb_copy_h, camera_ui[i].thumb_copy_stamp);
              // In the grid the selected camera has no full-res display stream. Plugins get its
              // full-res plugin feed via the background pass (on_screen there) as soon as the tile
              // worker produced one; until then fall back to the thumbnail (320x180 - too small
              // for face detection, but enough to keep the tile indicator alive).
              if (i == active_camera && !has_feed && camera_ui[i].thumb_copy && camera_ui[i].thumb_copy_w > 0 &&
                  camera_ui[i].thumb_copy_h > 0) {
                tmp_yuv->format = AV_PIX_FMT_YUV420P;
                tmp_yuv->width = camera_ui[i].thumb_copy_w;
                tmp_yuv->height = camera_ui[i].thumb_copy_h;
                int prc = av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize, camera_ui[i].thumb_copy,
                                               AV_PIX_FMT_YUV420P, camera_ui[i].thumb_copy_w,
                                               camera_ui[i].thumb_copy_h, 1);
                if (prc >= 0) {
                  RtspVideoFrameYuv420p vf = {0};
                  vf.size = (uint32_t)sizeof(vf);
                  vf.y = tmp_yuv->data[0];
                  vf.u = tmp_yuv->data[1];
                  vf.v = tmp_yuv->data[2];
                  vf.y_stride = tmp_yuv->linesize[0];
                  vf.u_stride = tmp_yuv->linesize[1];
                  vf.v_stride = tmp_yuv->linesize[2];
                  vf.w = camera_ui[i].thumb_copy_w;
                  vf.h = camera_ui[i].thumb_copy_h;
                  vf.pts_ms = 0;
                  vf.camera_index = i;
                  vf.camera_name_utf8 = cameras[i].title_utf8;
                  vf.on_screen = 1;
                  last_display_feed_ticks = SDL_GetTicks();
                  plugin_host_on_video_frame(&plugins, &vf, 0);
                }
              }
            }

            if (camera_ui[i].thumb_tex && camera_ui[i].thumb_w > 0 && camera_ui[i].thumb_h > 0) {
              SDL_RenderCopy(renderer, camera_ui[i].thumb_tex, NULL, &cell);
            }
          }

          // A disabled camera keeps its last picture but is dimmed, so it reads
          // as "switched off" rather than "frozen".
          if (!cameras[i].enabled) {
            SDL_BlendMode pb = SDL_BLENDMODE_NONE;
            SDL_GetRenderDrawBlendMode(renderer, &pb);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 165);
            SDL_RenderFillRect(renderer, &cell);
            SDL_SetRenderDrawBlendMode(renderer, pb);
          }

          SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
          SDL_RenderDrawRect(renderer, &cell);

           // Title: left-bottom corner.
           camera_ui_ensure_text(renderer, &camera_ui[i], &cameras[i], text_pt, text_color, &text_budget);
           if (camera_ui[i].text_tex) {
             int max_tw = cell.w - 16;
             float scale = 1.0f;
             if (camera_ui[i].text_w > max_tw && camera_ui[i].text_w > 0) {
               scale = (float)max_tw / (float)camera_ui[i].text_w;
             }
             int tw = (int)((float)camera_ui[i].text_w * scale);
             int th = (int)((float)camera_ui[i].text_h * scale);
             SDL_Rect tr = {cell.x + 8, cell.y + cell.h - th - 6, tw, th};
             SDL_SetTextureAlphaMod(camera_ui[i].text_tex, 200);
             SDL_RenderCopy(renderer, camera_ui[i].text_tex, NULL, &tr);
             SDL_SetTextureAlphaMod(camera_ui[i].text_tex, 255);
           }

           // Buffering: centred spinner (only for selected tile main-stream prebuffer).
           if (i == active_camera && matrix_main_loading && cameras[i].enabled) {
             int sr = (cell.w < cell.h ? cell.w : cell.h) / 8;
             if (sr < 12) sr = 12;
             if (sr > 48) sr = 48;
             draw_spinner(renderer, cell.x + cell.w / 2, cell.y + cell.h / 2, sr, now);
           }

           // Stream tile buffering (no thumbnail yet / stalled): right-bottom corner.
           // Not gated on the worker existing: tiles spawn staggered, and the
           // gap before a camera's worker starts is exactly when there is no
           // stream and the spinner belongs on screen.
           if (preview_mode == PREVIEW_MODE_STREAM && !drew_main && !matrix_main_loading &&
               cameras[i].enabled) {
             Uint32 last = 0;
             int from_cache = 0;
             if (cameras[i].lock) SDL_LockMutex(cameras[i].lock);
             last = cameras[i].thumb_last_update_ticks;
             from_cache = cameras[i].thumb_from_cache;
             if (cameras[i].lock) SDL_UnlockMutex(cameras[i].lock);

             // A thumbnail restored from disk is a still, not a feed: keep the
             // spinner up until a live frame actually arrives.
             int show = (last == 0 || from_cache) ? 1 : ((now - last) > 2000u ? 1 : 0);
             if (show) {
               if (camera_ui[i].stream_buf_since_ticks == 0) {
                 camera_ui[i].stream_buf_since_ticks = now;
                 camera_ui[i].stream_buf_last_pct = -1;
                 destroy_text(&camera_ui[i].stream_buf_pct_text);
               }

               int sr = (cell.w < cell.h ? cell.w : cell.h) / 8;
               if (sr < 12) sr = 12;
               if (sr > 48) sr = 48;
               draw_spinner(renderer, cell.x + cell.w / 2, cell.y + cell.h / 2, sr, now);
             } else {
               if (camera_ui[i].stream_buf_since_ticks != 0) {
                 camera_ui[i].stream_buf_since_ticks = 0;
                 camera_ui[i].stream_buf_last_pct = -1;
                 destroy_text(&camera_ui[i].stream_buf_pct_text);
               }
             }
           }

           if (i == active_camera) {
             SDL_SetRenderDrawColor(renderer, 60, 180, 60, 220);
             SDL_RenderDrawRect(renderer, &cell);
            SDL_Rect c2 = {cell.x + 1, cell.y + 1, cell.w - 2, cell.h - 2};
            SDL_RenderDrawRect(renderer, &c2);
          }

          // Recording indicator: small red dot, top-right corner of the tile.
          if (cameras[i].record_enabled) {
            int rr_dot = (cell.w < cell.h ? cell.w : cell.h) / 16;
            if (rr_dot < 5) rr_dot = 5;
            if (rr_dot > 14) rr_dot = 14;
            draw_rec_dot(renderer, cell, rr_dot);
          }
        }
      } else {
        SDL_Texture *show_tex = texture;
        int show_w = tex_w;
        int show_h = tex_h;

        // Big view fallback (any mode): while the main-stream texture isn't ready, keep showing the
        // active camera's tile picture — the tile threads keep it live — instead of a black screen
        // or an error. The main stream connects in parallel and replaces it on its first frame.
        if (camera_ui && active_camera >= 0 && active_camera < camera_count) {
          // Ensure the active camera thumbnail texture is up to date, even when the bottom tile bar is hidden.
          int copied = 0;
          if (cameras[active_camera].lock) SDL_LockMutex(cameras[active_camera].lock);
          if (cameras[active_camera].thumb_last_update_ticks &&
              cameras[active_camera].thumb_last_update_ticks != camera_ui[active_camera].thumb_stamp &&
              cameras[active_camera].thumb_yuv && cameras[active_camera].thumb_yuv_size > 0) {
            copied = camera_ui_copy_thumb(&camera_ui[active_camera], &cameras[active_camera]);
          }
          if (cameras[active_camera].lock) SDL_UnlockMutex(cameras[active_camera].lock);
          if (copied) {
            camera_ui_update_thumb(renderer, &camera_ui[active_camera], camera_ui[active_camera].thumb_copy,
                                   camera_ui[active_camera].thumb_copy_w, camera_ui[active_camera].thumb_copy_h,
                                   camera_ui[active_camera].thumb_copy_stamp);
          }

          if (!show_tex && camera_ui[active_camera].thumb_tex) {
            show_tex = camera_ui[active_camera].thumb_tex;
            show_w = camera_ui[active_camera].thumb_w;
            show_h = camera_ui[active_camera].thumb_h;
            showing_thumb_main = 1;
          }
        }

        if (show_tex) {
          if (showing_thumb_main || zoom <= 1.0 || show_w <= 0 || show_h <= 0) {
            SDL_RenderCopy(renderer, show_tex, NULL, NULL);
          } else {
            int win_w = 0, win_h = 0;
            SDL_GetWindowSize(window, &win_w, &win_h);
            if (win_w > 0 && win_h > 0) {
              double src_w = (double)show_w / zoom;
              double src_h = (double)show_h / zoom;
              if (src_w < 1.0) src_w = 1.0;
              if (src_h < 1.0) src_h = 1.0;

              if (view_x < 0.0) view_x = 0.0;
              if (view_y < 0.0) view_y = 0.0;
              if (view_x > (double)show_w - src_w) view_x = (double)show_w - src_w;
              if (view_y > (double)show_h - src_h) view_y = (double)show_h - src_h;

              SDL_Rect src = {(int)(view_x + 0.5), (int)(view_y + 0.5), (int)(src_w + 0.5),
                              (int)(src_h + 0.5)};
              if (src.x < 0) src.x = 0;
              if (src.y < 0) src.y = 0;
              if (src.w < 1) src.w = 1;
              if (src.h < 1) src.h = 1;
              if (src.x + src.w > show_w) src.w = show_w - src.x;
              if (src.y + src.h > show_h) src.h = show_h - src.y;
              SDL_RenderCopy(renderer, show_tex, &src, NULL);
            } else {
              SDL_RenderCopy(renderer, show_tex, NULL, NULL);
            }
          }
        }
      }

      if (!in_matrix_grid && !showing_thumb_main && texture && tex_w > 0 && tex_h > 0) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        plugin_host_draw_overlays(renderer, &plugins, win_w, win_h, tex_w, tex_h, zoom, view_x,
                                  view_y);
      }

      int is_rec = 0;
      if (rec_mutex) SDL_LockMutex(rec_mutex);
      is_rec = rec_want ? 1 : 0;
      if (rec_mutex) SDL_UnlockMutex(rec_mutex);
      if (!in_matrix_grid && is_rec) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        (void)win_h;
        int radius = 16;
        int cx = win_w - 20 - radius;
        int cy = 20 + radius;
        SDL_Color red = {220, 30, 30, 180}; // ~70% alpha
        draw_filled_circle(renderer, cx, cy, radius, red);
      }

      if (view_mode == VIEW_MODE_TILES && now <= overlay_until) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);

        const int pad = 10;
        const int gap = 10;
        const int tile_w = 220;
        const int tile_h = 124;
        // Clamp scroll.
        int total_w = 0;
        if (camera_count > 0) total_w = camera_count * (tile_w + gap) - gap;
        int view_w = win_w - 2 * pad;
        int max_scroll = total_w - view_w;
        if (max_scroll < 0) max_scroll = 0;
        if (tile_scroll_px < 0) tile_scroll_px = 0;
        if (tile_scroll_px > max_scroll) tile_scroll_px = max_scroll;

        int x = pad - tile_scroll_px;
        int y = win_h - tile_h - pad;
        int text_budget = 4;
        for (int i = 0; i < camera_count; i++) {
          int rect_w = tile_w;
          int rect_h = tile_h;
          SDL_Rect r = {x, y, rect_w, rect_h};

          // Tile background transparency: "33% transparent" ~= 67% opacity -> alpha ~ 0.67 * 255 ~= 171.
          SDL_Color bg = {0, 0, 0, 171};
          SDL_Color border = {255, 255, 255, 120};
          if (i == active_camera) {
            bg = (SDL_Color){0, 0, 0, 190};
            border = (SDL_Color){255, 255, 255, 180};
          }

          SDL_BlendMode prev_blend = SDL_BLENDMODE_NONE;
          SDL_GetRenderDrawBlendMode(renderer, &prev_blend);
          SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
          SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
          if (r.x + r.w >= 0 && r.x <= win_w) {
            SDL_RenderFillRect(renderer, &r);

            // Thumbnail (or black tile if not cached yet).
            int copied = 0;
            if (cameras[i].lock) SDL_LockMutex(cameras[i].lock);
            if (cameras[i].thumb_last_update_ticks &&
                cameras[i].thumb_last_update_ticks != camera_ui[i].thumb_stamp &&
                cameras[i].thumb_yuv && cameras[i].thumb_yuv_size > 0) {
              copied = camera_ui_copy_thumb(&camera_ui[i], &cameras[i]);
            }
            if (cameras[i].lock) SDL_UnlockMutex(cameras[i].lock);
            if (copied) {
              camera_ui_update_thumb(renderer, &camera_ui[i], camera_ui[i].thumb_copy, camera_ui[i].thumb_copy_w,
                                     camera_ui[i].thumb_copy_h, camera_ui[i].thumb_copy_stamp);
            }
            if (camera_ui[i].thumb_tex) {
              // Keep video fully opaque/vivid; use translucent background instead.
              SDL_RenderCopy(renderer, camera_ui[i].thumb_tex, NULL, &r);
            }

            // Dim a disabled camera here too (see the matrix grid), so a frozen
            // thumbnail is never mistaken for a live feed.
            if (!cameras[i].enabled) {
              SDL_SetRenderDrawColor(renderer, 0, 0, 0, 165);
              SDL_RenderFillRect(renderer, &r);
            }

            SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
            SDL_RenderDrawRect(renderer, &r);

            // Title: left-bottom corner.
            camera_ui_ensure_text(renderer, &camera_ui[i], &cameras[i], text_pt, text_color, &text_budget);
            if (camera_ui[i].text_tex) {
              int max_tw = r.w - 20;
              float scale = 1.0f;
              if (camera_ui[i].text_w > max_tw && camera_ui[i].text_w > 0) {
                scale = (float)max_tw / (float)camera_ui[i].text_w;
              }
              int tw = (int)((float)camera_ui[i].text_w * scale);
              int th = (int)((float)camera_ui[i].text_h * scale);
              SDL_Rect tr = {r.x + 8, r.y + r.h - th - 6, tw, th};

              SDL_SetTextureAlphaMod(camera_ui[i].text_tex, 180);
              SDL_RenderCopy(renderer, camera_ui[i].text_tex, NULL, &tr);
              SDL_SetTextureAlphaMod(camera_ui[i].text_tex, 255);
            }

            // Active tile border.
            if (i == active_camera) {
              SDL_SetRenderDrawColor(renderer, 60, 180, 60, 220);
              SDL_RenderDrawRect(renderer, &r);
              SDL_Rect r2 = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
              SDL_RenderDrawRect(renderer, &r2);
            }

            // Recording indicator: small red dot, top-right corner of the tile.
            if (cameras[i].record_enabled) {
              int rr_dot = (r.w < r.h ? r.w : r.h) / 16;
              if (rr_dot < 5) rr_dot = 5;
              if (rr_dot > 14) rr_dot = 14;
              draw_rec_dot(renderer, r, rr_dot);
            }

            // Buffering: active tile right-bottom corner (Tiles mode).
            if (i == active_camera && is_loading) {
              int sr = (r.w < r.h ? r.w : r.h) / 8;
              if (sr < 12) sr = 12;
              if (sr > 48) sr = 48;
              draw_spinner(renderer, r.x + r.w / 2, r.y + r.h / 2, sr, now);
            }

            // Stream tile buffering (no thumbnail yet / stalled): right-bottom corner.
            // See the matrix-grid case above: shown before the worker spawns too.
            if (preview_mode == PREVIEW_MODE_STREAM && !(i == active_camera && is_loading) &&
                cameras[i].enabled) {
              Uint32 last = 0;
              int from_cache = 0;
              if (cameras[i].lock) SDL_LockMutex(cameras[i].lock);
              last = cameras[i].thumb_last_update_ticks;
              from_cache = cameras[i].thumb_from_cache;
              if (cameras[i].lock) SDL_UnlockMutex(cameras[i].lock);

              int show = (last == 0 || from_cache) ? 1 : ((now - last) > 2000u ? 1 : 0);
              if (show) {
                if (camera_ui[i].stream_buf_since_ticks == 0) {
                  camera_ui[i].stream_buf_since_ticks = now;
                  camera_ui[i].stream_buf_last_pct = -1;
                  destroy_text(&camera_ui[i].stream_buf_pct_text);
#ifdef _WIN32
                  if (base_dir[0]) {
                    if (!logged_any_tile_buf_begin) {
                      startup_tracef(base_dir, "tile_buffering_first_begin idx=%d last_thumb=%lu", i,
                                     (unsigned long)last);
                      logged_any_tile_buf_begin = 1;
                    } else {
                      startup_tracef(base_dir, "tile_buffering_begin idx=%d last_thumb=%lu", i, (unsigned long)last);
                    }
                  }
#endif
                }

                int sr = (r.w < r.h ? r.w : r.h) / 8;
                if (sr < 12) sr = 12;
                if (sr > 48) sr = 48;
                draw_spinner(renderer, r.x + r.w / 2, r.y + r.h / 2, sr, now);
              } else {
                if (camera_ui[i].stream_buf_since_ticks != 0) {
#ifdef _WIN32
                  if (base_dir[0]) {
                    Uint32 dt = (Uint32)(now - camera_ui[i].stream_buf_since_ticks);
                    startup_tracef(base_dir, "tile_buffering_end idx=%d dt_ms=%lu", i, (unsigned long)dt);
                  }
#endif
                  camera_ui[i].stream_buf_since_ticks = 0;
                  camera_ui[i].stream_buf_last_pct = -1;
                  destroy_text(&camera_ui[i].stream_buf_pct_text);
                }
              }
            }
          }
          SDL_SetRenderDrawBlendMode(renderer, prev_blend);

          x += rect_w + gap;
        }
      }

      // The big view starts by showing the cached still (warm start), which on
      // its own reads as a running stream. Spin until a live frame arrives, no
      // matter which pipeline is meant to feed it.
      int big_waiting_live = 0;
      if (!in_matrix_grid && camera_count > 0 && active_camera >= 0 &&
          active_camera < camera_count && cameras[active_camera].enabled) {
        if (cameras[active_camera].lock) SDL_LockMutex(cameras[active_camera].lock);
        big_waiting_live = (cameras[active_camera].last_seen_ticks == 0 &&
                            (cameras[active_camera].thumb_last_update_ticks == 0 ||
                             cameras[active_camera].thumb_from_cache));
        if (cameras[active_camera].lock) SDL_UnlockMutex(cameras[active_camera].lock);
      }

      // A picture is already on screen in the big view: either the live tile thumb
      // (showing_thumb_main) or a cached/substream still copied into `texture`.
      int big_have_picture = !in_matrix_grid && (showing_thumb_main || texture != NULL);

      if (((is_loading && !stream_tiles_use_main) || big_waiting_live) && !big_have_picture) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        // Centred spinner: neutral "connecting" icon in the middle of the view,
        // instead of a corner "Буферизация NN%" that sticks at 99% when offline.
        int sr = (win_w < win_h ? win_w : win_h) / 14;
        if (sr < 18) sr = 18;
        if (sr > 64) sr = 64;
        draw_spinner(renderer, win_w / 2, win_h / 2, sr, now);
      }

      // The view runs on the secondary stream (live thumb or cached still) while the
      // main stream (re)connects: keep the picture, but tell the user the main stream
      // is still loading — small corner spinner plus a text label, instead of a
      // full-screen loading/error state (or, previously, nothing at all).
      if (big_have_picture &&
          (is_loading || big_main_pending || big_waiting_live ||
           (showing_thumb_main && has_error)) &&
          !(has_error && !showing_thumb_main)) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        if (win_w > 0) {
          // Centred (spinner left of the label): the top-right corner belongs to the
          // REC dot and this badge used to cover it.
          if (main_loading_text.tex && main_loading_text_ver != i18n_version())
            destroy_text(&main_loading_text);
          if (!main_loading_text.tex) {
            main_loading_text = make_text(renderer, tr(STR_LOADING_MAIN_STREAM), buf_pt, text_color);
            main_loading_text_ver = i18n_version();
          }
          const int sp_r = 18, gap = 10;
          int group_w = sp_r * 2 + (main_loading_text.tex ? gap + main_loading_text.w : 0);
          int gx = (win_w - group_w) / 2;
          if (gx < 4) gx = 4;
          draw_spinner(renderer, gx + sp_r, win_h / 2, sp_r, now);
          if (main_loading_text.tex) {
            int tx = gx + sp_r * 2 + gap;
            int ty = win_h / 2 - main_loading_text.h / 2;
            SDL_Rect bg = {tx - 6, ty - 3, main_loading_text.w + 12, main_loading_text.h + 6};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            SDL_RenderFillRect(renderer, &bg);
            SDL_Rect tr_rect = {tx, ty, main_loading_text.w, main_loading_text.h};
            SDL_RenderCopy(renderer, main_loading_text.tex, NULL, &tr_rect);
          }
        }
      }

      if (has_error && !showing_thumb_main) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);

        SDL_SetRenderDrawColor(renderer, 80, 0, 0, 170);
        SDL_Rect shade = {0, 0, win_w, win_h};
        SDL_RenderFillRect(renderer, &shade);

        if (error_text.tex) {
          SDL_Rect r = {(win_w - error_text.w) / 2, (win_h - error_text.h) / 2, error_text.w,
                        error_text.h};
          SDL_RenderCopy(renderer, error_text.tex, NULL, &r);
        }
      }

      if (!texture && !is_loading && !has_error && no_cameras_text.tex) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
        SDL_Rect shade = {0, 0, win_w, win_h};
        SDL_RenderFillRect(renderer, &shade);

        SDL_Rect r = {(win_w - no_cameras_text.w) / 2, (win_h - no_cameras_text.h) / 2,
                      no_cameras_text.w, no_cameras_text.h};
        SDL_RenderCopy(renderer, no_cameras_text.tex, NULL, &r);
      }

      if (intro.is_open) {
        intro_render(&intro, renderer, window, text_pt, text_color);
      } else if (settings.is_open) {
        settings_render(&settings, renderer, window, text_pt, text_color);
      } else if (plugins_ui.is_open) {
        plugins_overlay_render(&plugins_ui, renderer, window, text_pt, text_color);
      } else if (mgr.is_open) {
        camera_manager_render(&mgr, text_pt, text_color);
      }

      // Phase 3+4: green/amber semi-transparent speaker icon while audio is playing
      // for the currently-selected camera, with a small NN% label below. Color shifts
      // amber above 100% to flag the software-boosted range; at 0% a red diagonal line
      // crosses the icon to indicate mute. Hidden inside modal overlays.
      //
      // Placement: anchored to the visual region of the camera that's actually
      // sounding — the selected cell in matrix grid view, otherwise the window's
      // bottom-right corner.
      {
        int vol = audio_volume_pct;
        // Icon appears only once the first audio sample has actually been queued
        // by the decoder (sticky 'audio_playing' flag), not during the RTSP-open
        // phase. Cleared on camera switch in the decode-thread restart path.
        int no_modal = !intro.is_open && !settings.is_open && !plugins_ui.is_open && !mgr.is_open;
        int show_speaker = SDL_AtomicGet(&audio_playing_sticky) && no_modal;
        // Face-plugin indicator: drawn in the slot left of the speaker whenever the
        // "Face Counter" plugin is enabled (independent of audio state).
        int show_face = no_modal && plugin_host_is_enabled_named(&plugins, "Face");
        if (show_speaker || show_face) {
          int win_w = 0, win_h = 0;
          SDL_GetWindowSize(window, &win_w, &win_h);
          if (win_w > 0 && win_h > 0) {
            // Pick an anchor rect: matrix grid → the selected tile; otherwise → the
            // full window. The icon is drawn in this rect's bottom-right corner.
            SDL_Rect anchor = {0, 0, win_w, win_h};
            int icon_in_tile = 0;
            if (argc < 2 && view_mode == VIEW_MODE_MATRIX && camera_count > 1 &&
                matrix_expanded < 0 && active_camera >= 0 && active_camera < camera_count) {
              int rows = 1, cols = 1;
              choose_matrix_grid(camera_count, win_w, win_h, &rows, &cols);
              const int pad_g = 1;
              const int gap_g = 1;
              int grid_w = win_w - 2 * pad_g;
              int grid_h = win_h - 2 * pad_g;
              if (grid_w < 1) grid_w = 1;
              if (grid_h < 1) grid_h = 1;
              int cell_w = (cols > 0) ? (grid_w - gap_g * (cols - 1)) / cols : grid_w;
              int cell_h = (rows > 0) ? (grid_h - gap_g * (rows - 1)) / rows : grid_h;
              if (cell_w < 1) cell_w = 1;
              if (cell_h < 1) cell_h = 1;
              int rr = (cols > 0) ? (active_camera / cols) : 0;
              int cc = (cols > 0) ? (active_camera % cols) : 0;
              anchor.x = pad_g + cc * (cell_w + gap_g);
              anchor.y = pad_g + rr * (cell_h + gap_g);
              anchor.w = cell_w;
              anchor.h = cell_h;
              icon_in_tile = 1;
            }

            int icon_size;
            int icon_min, icon_max;
            if (icon_in_tile) {
              icon_size = anchor.h / 10;
              icon_min = 16;
              icon_max = 44;
            } else {
              icon_size = win_h / 14;
              icon_min = 28;
              icon_max = 80;
            }
            if (icon_size < icon_min) icon_size = icon_min;
            if (icon_size > icon_max) icon_size = icon_max;
            int pad = icon_size / 3;
            int icon_x = anchor.x + anchor.w - icon_size - pad;
            int icon_y = anchor.y + anchor.h - icon_size - pad;

            if (show_face) {
              Uint32 tnow = SDL_GetTicks();
              if (plugin_host_overlay_count_named(&plugins, "Face") > 0) face_flash_until_ms = tnow + 1000;
              int fx = icon_x - icon_size - pad;
              if ((Sint32)(face_flash_until_ms - tnow) > 0) {
                SDL_Color fill = {255, 220, 0, 245};
                SDL_Color ink = {40, 30, 0, 255};
                draw_face_icon_filled(renderer, fx, icon_y, icon_size, ink, fill);
              } else {
                SDL_Color fc = {120, 200, 255, 235};
                draw_face_icon(renderer, fx, icon_y, icon_size, fc);
              }
            }

            if (show_speaker) {
            SDL_Color speaker_color;
            if (vol == 0) {
              speaker_color = (SDL_Color){210, 210, 210, 240};
            } else if (vol <= 100) {
              speaker_color = (SDL_Color){80, 240, 100, 245};
            } else {
              // 101..200: shift hue from green to amber as vol grows.
              int t = vol - 100; // 1..100
              int r = 80 + (255 - 80) * t / 100;
              int g = 240 - (240 - 180) * t / 100;
              int b = 100 - (100 - 30) * t / 100;
              speaker_color = (SDL_Color){(Uint8)r, (Uint8)g, (Uint8)b, 250};
            }
            draw_speaker_icon(renderer, icon_x, icon_y, icon_size, speaker_color);

            if (vol == 0) {
              SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
              SDL_SetRenderDrawColor(renderer, 230, 60, 60, 220);
              int inset = icon_size / 8;
              for (int t = -1; t <= 1; t++) {
                SDL_RenderDrawLine(renderer,
                                   icon_x + inset, icon_y + inset + t,
                                   icon_x + icon_size - inset, icon_y + icon_size - inset + t);
                SDL_RenderDrawLine(renderer,
                                   icon_x + icon_size - inset, icon_y + inset + t,
                                   icon_x + inset, icon_y + icon_size - inset + t);
              }
            }

            // (Re)build the percentage label only when the value changes.
            if (vol != volume_text_pct) {
              char vbuf[16];
              snprintf(vbuf, sizeof(vbuf), "%d%%", vol);
              SDL_Color vc = (vol > 100) ? (SDL_Color){255, 215, 100, 255}
                                         : (SDL_Color){220, 255, 220, 255};
              text_set(renderer, &volume_text, vbuf, text_pt, vc);
              volume_text_pct = vol;
            }
            if (volume_text.tex && volume_text.w > 0 && volume_text.h > 0) {
              SDL_Rect vr = {icon_x + (icon_size - volume_text.w) / 2,
                             icon_y - volume_text.h - 2,
                             volume_text.w, volume_text.h};
              if (vr.x < 0) vr.x = 0;
              if (vr.y < 0) vr.y = icon_y + icon_size + 2;
              SDL_RenderCopy(renderer, volume_text.tex, NULL, &vr);
            }
            } // show_speaker
          }
        }
      }

      // ---------------- PTZ pad ----------------
      // Drawn for a PTZ-capable camera shown big, and hidden behind any modal.
      if (!intro.is_open && !settings.is_open && !plugins_ui.is_open && !mgr.is_open) {
        int ci = ptz_pad_camera(cameras, camera_count, stream_from_list, view_mode,
                                matrix_expanded, active_camera);
        if (ci >= 0) {
          int win_w = 0, win_h = 0;
          SDL_GetWindowSize(window, &win_w, &win_h);
          ptz_pad_layout(win_w, win_h, &ptz_pad_rect, ptz_btn_rects);
          ptz_pad_render(renderer, &ptz_pad_rect, ptz_btn_rects, ptz_active_dir,
                         ptz_is_busy(ptz));

          // Surface the last PTZ failure under the pad (rebuilt only on change).
          char perr[256];
          if (ptz_last_error(ptz, perr, sizeof(perr))) {
            if (strcmp(perr, ptz_error_shown) != 0) {
              char line[320];
              snprintf(line, sizeof(line), "%s%s", tr(STR_PTZ_ERROR), perr);
              SDL_Color ec = {255, 140, 140, 255};
              text_set(renderer, &ptz_error_text, line, text_pt, ec);
              snprintf(ptz_error_shown, sizeof(ptz_error_shown), "%s", perr);
            }
            if (ptz_error_text.tex) {
              SDL_Rect r = {ptz_pad_rect.x + ptz_pad_rect.w - ptz_error_text.w,
                            ptz_pad_rect.y + ptz_pad_rect.h + 6, ptz_error_text.w,
                            ptz_error_text.h};
              if (r.x < 4) r.x = 4;
              SDL_RenderCopy(renderer, ptz_error_text.tex, NULL, &r);
            }
          } else if (ptz_error_shown[0]) {
            destroy_text(&ptz_error_text);
            ptz_error_shown[0] = 0;
          }
        }
      }

      // ---------------- Time machine: playback overdraw + bottom strip ----------------
      if (timeline.open) {
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);

        // Rebind to the active camera if the selection changed while open.
        if (tm_cam != active_camera && active_camera >= 0 && active_camera < camera_count) {
          time_machine_set_camera(tmach, cameras[active_camera].title_utf8);
          tm_cam = active_camera;
          timeline.initialized = 0;
        }

        int nseg = 0;
        double span_min = 0, span_max = 0;
        if (tm_view) nseg = time_machine_copy_segments(tmach, tm_view, TM_VIEW_MAX, &span_min, &span_max);

        // First-open view: ~6h wide, centered on the newest footage (or now).
        if (!timeline.initialized) {
          SDL_Rect s0 = ui_timeline_rect(win_w, win_h);
          timeline.pps = (s0.w > 0) ? (double)s0.w / 21600.0 : 0.05;
          timeline.center = (span_max > 0) ? span_max : (double)time(NULL);
          timeline.initialized = 1;
        }

        // Periodically rescan to pick up freshly written segments.
        if (SDL_GetTicks() - tm_last_rescan > 3000) {
          time_machine_request_rescan(tmach);
          tm_last_rescan = SDL_GetTicks();
        }

        // Pull the latest decoded historical frame into pb_tex.
        if (time_machine_fetch_frame(tmach, &pb_buf, &pb_cap, &pb_w, &pb_h, &pb_serial)) {
          if (!pb_tex || pb_tex_w != pb_w || pb_tex_h != pb_h) {
            if (pb_tex) SDL_DestroyTexture(pb_tex);
            pb_tex = recreate_texture(renderer, pb_w, pb_h);
            pb_tex_w = pb_w;
            pb_tex_h = pb_h;
          }
          if (pb_tex && pb_w > 0 && pb_h > 0) {
            tmp_yuv->format = AV_PIX_FMT_YUV420P;
            tmp_yuv->width = pb_w;
            tmp_yuv->height = pb_h;
            if (av_image_fill_arrays(tmp_yuv->data, tmp_yuv->linesize, pb_buf, AV_PIX_FMT_YUV420P,
                                     pb_w, pb_h, 1) >= 0) {
              SDL_UpdateYUVTexture(pb_tex, NULL, tmp_yuv->data[0], tmp_yuv->linesize[0],
                                   tmp_yuv->data[1], tmp_yuv->linesize[1], tmp_yuv->data[2],
                                   tmp_yuv->linesize[2]);
            }
          }
        }

        // While playing, overdraw the active camera's container with the history
        // frame (selected tile in the grid, full window when expanded/single).
        int playing_hist = time_machine_is_playing(tmach);
        int has_hist = time_machine_has_video(tmach);
        if (playing_hist) {
          SDL_Rect dst;
          int in_grid = (view_mode == VIEW_MODE_MATRIX && matrix_expanded < 0);
          if (in_grid && camera_count > 0) {
            int rows = 1, cols = 1;
            choose_matrix_grid(camera_count, win_w, win_h, &rows, &cols);
            int pad = 1, gap = 1;
            int grid_w = win_w - 2 * pad;
            int grid_h = win_h - 2 * pad;
            if (grid_w < 1) grid_w = 1;
            if (grid_h < 1) grid_h = 1;
            int cell_w = (cols > 0) ? (grid_w - gap * (cols - 1)) / cols : grid_w;
            int cell_h = (rows > 0) ? (grid_h - gap * (rows - 1)) / rows : grid_h;
            if (cell_w < 1) cell_w = 1;
            if (cell_h < 1) cell_h = 1;
            int rr = (cols > 0) ? (active_camera / cols) : 0;
            int cc = (cols > 0) ? (active_camera % cols) : 0;
            dst = (SDL_Rect){pad + cc * (cell_w + gap), pad + rr * (cell_h + gap), cell_w, cell_h};
          } else {
            dst = (SDL_Rect){0, 0, win_w, win_h - ui_timeline_rect(win_w, win_h).h};
            if (dst.h < 1) dst.h = win_h;
          }
          SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
          SDL_RenderFillRect(renderer, &dst);
          if (has_hist && pb_tex && pb_tex_w > 0 && pb_tex_h > 0) {
            SDL_RenderCopy(renderer, pb_tex, NULL, &dst);
          }
          if (in_grid) {
            SDL_SetRenderDrawColor(renderer, 255, 70, 70, 220);
            SDL_RenderDrawRect(renderer, &dst);
          }
        }

        SDL_Rect strip = ui_timeline_rect(win_w, win_h);
        const char *ct = (active_camera >= 0 && active_camera < camera_count &&
                          cameras[active_camera].title_utf8)
                             ? cameras[active_camera].title_utf8
                             : "";
        ui_timeline_render(renderer, &timeline, strip, ct, tm_view, nseg,
                           time_machine_playhead(tmach), has_hist, playing_hist, text_pt);
      }

#ifdef _WIN32
      if (!logged_first_present && base_dir[0]) {
        startup_tracef(base_dir, "first_present");
        logged_first_present = 1;
      }
#endif
      SDL_RenderPresent(renderer);
    } else {
      SDL_Delay(1);
    }
  }

  // Abandon any in-flight ONVIF probe (the worker frees itself on completion).
  if (onvif_probe_th) {
    onvif_probe_destroy(onvif_probe_th);
    onvif_probe_th = NULL;
  }

  // Stop the PTZ worker; it sends a final Stop if a move is still running, so
  // quitting mid-pan never leaves the camera turning.
  if (ptz) {
    if (ptz_active_dir >= 0) {
      ptz_release(ptz);
      ptz_active_dir = -1;
    }
    ptz_stop_and_free(ptz);
    ptz = NULL;
  }
  destroy_text(&ptz_error_text);

#ifdef _WIN32
  // Drain and stop the background settings writer first, so it isn't racing the
  // final synchronous save below for the same settings.ini.
  persist_flush_and_stop(g_persist);
  g_persist = NULL;

  // Persist cached previews only on exit (no runtime disk I/O).
  // Do this BEFORE stopping the decode thread so a hung network/thread won't prevent saving.
  // Also persist learned per-camera keyframe interval estimates.
  if (settings_path[0]) save_cameras_to_ini(settings_path, cameras, camera_count, active_camera);

  if (fast_exit) {
    // Hard-kill network threads on exit (Windows will tear down remaining threads).
    // Still persist cache/settings so the next startup is instant.
    SDL_AtomicSet(&shared.quit, 1);
    if (warm_threads) SDL_AtomicSet(&warm_quit, 1);
    if (tile_threads) SDL_AtomicSet(&tile_quit, 1);

    // Workers were only signalled (not joined) on this path, so guard the buffers
    // against concurrent writes by holding every camera's lock across the save.
    for (int i = 0; i < camera_count; i++) {
      if (cameras[i].lock) SDL_LockMutex(cameras[i].lock);
    }
    camera_cache_save_all(cameras, camera_count, base_dir);
    camera_thumb_save_all(cameras, camera_count, base_dir);
    for (int i = 0; i < camera_count; i++) {
      if (cameras[i].lock) SDL_UnlockMutex(cameras[i].lock);
    }

    ExitProcess(0);
  }

  // Bring down audio first so the worker stops touching SDL/AVCodec while
  // tile/decode threads are still being unwound.
  if (audio_pipe) {
    audio_pipe_signal_quit(audio_pipe);
    if (audio_worker_th) {
      SDL_WaitThread(audio_worker_th, NULL);
      audio_worker_th = NULL;
    }
    audio_pipe_destroy(audio_pipe);
    audio_pipe = NULL;
  }
  if (warm_threads) {
    warm_cams_stop(warm_threads, warm_args, warm_count, &warm_quit);
    warm_threads = NULL;
    warm_args = NULL;
    warm_count = 0;
  }
  if (tile_threads) {
    tile_streams_stop(tile_threads, tile_args, tile_count, &tile_quit);
    tile_threads = NULL;
    tile_args = NULL;
    tile_count = 0;
  }
#ifdef _WIN32
  if (startup_cache_th) {
    SDL_AtomicSet(&startup_cache_quit, 1);
    SDL_WaitThread(startup_cache_th, NULL);
    startup_cache_th = NULL;
    SDL_AtomicSet(&startup_cache_quit, 0);
  }
  SDL_AtomicSet(&watchdog_quit, 1);
  if (watchdog_th) SDL_WaitThread(watchdog_th, NULL);
  watchdog_th = NULL;
  SDL_AtomicSet(&archive_quit, 1);
  if (archive_th) SDL_WaitThread(archive_th, NULL);
  archive_th = NULL;
#endif
  camera_cache_save_all(cameras, camera_count, base_dir);
  camera_thumb_save_all(cameras, camera_count, base_dir);
#endif

  intro_close(&intro);
  settings_close(&settings);
  plugins_overlay_close(&plugins_ui);
  camera_manager_close(&mgr);
  if (th_join) SDL_WaitThread(th_join, NULL);
  stop_decode_thread(&shared, &th);
  SDL_AtomicSet(&audio.quit, 1);
  audio_state_clear_queue(&audio);
  {
    SDL_AudioDeviceID dev = 0;
    if (audio_state_snapshot(&audio, &dev, NULL) && dev != 0) {
      SDL_CloseAudioDevice(dev);
    }
  }
  if (audio.mutex) SDL_DestroyMutex(audio.mutex);
  audio.mutex = NULL;
  if (tmach) time_machine_destroy(tmach);
  if (pb_tex) SDL_DestroyTexture(pb_tex);
  SDL_free(pb_buf);
  SDL_free(tm_view);
  if (rec_mutex) SDL_DestroyMutex(rec_mutex);
  // Safety net: on non-Windows the queue is created but never drained above.
  if (g_persist) {
    persist_flush_and_stop(g_persist);
    g_persist = NULL;
  }
  destroy_text(&buffering_pct_text);
  destroy_text(&main_loading_text);
  destroy_text(&volume_text);
  if (plugin_feed_buf) SDL_free(plugin_feed_buf);

  av_frame_free(&tmp_yuv);
  SDL_free(frame_copy);
  SDL_free(shared.yuv);
  SDL_DestroyCond(shared.cond);
  SDL_DestroyMutex(shared.mutex);

  if (texture) SDL_DestroyTexture(texture);
  plugin_host_shutdown(&plugins);
  SDL_DestroyRenderer(renderer);
  tray_shutdown();
  single_instance_release();
  SDL_DestroyWindow(window);
  free_camera_ui(camera_ui, camera_count);
  if (error_text.tex) SDL_DestroyTexture(error_text.tex);
  if (no_cameras_text.tex) SDL_DestroyTexture(no_cameras_text.tex);
  if (loading_text.tex) SDL_DestroyTexture(loading_text.tex);
  if (loading_text_small.tex) SDL_DestroyTexture(loading_text_small.tex);

  SDL_Quit();
  avformat_network_deinit();

  free_cameras(cameras, camera_count);
  return 0;
}
