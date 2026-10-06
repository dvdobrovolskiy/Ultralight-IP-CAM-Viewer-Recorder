#pragma once

#include <stdint.h>

#include <SDL.h>

// "Time machine" — historical playback of recorded .ts segments for one camera.
//
// Philosophy (same as the rest of the app): fully async + isolated. A TimeMachine
// owns ONE dedicated thread that both scans the recordings folder for coverage and
// decodes/paces playback. The UI thread never blocks on disk or decode: it reads a
// snapshot of the coverage index for the strip, and pulls the latest decoded YUV
// frame (compatible with the live SDL_UpdateYUVTexture path) under a short lock.
//
// On-disk layout it reads (written by async_recorder.c):
//   <base>recordings/<safe_title>/<dd.MM.yyyy>/<HH-MM-SS>.ts
// Times are local wall-clock; the index uses absolute unix seconds throughout.

typedef struct TmSeg {
  double start;          // unix seconds (from folder day + file HH-MM-SS)
  double end;            // unix seconds (file last-write time, capped)
  char path[1024];
} TmSeg;

typedef struct TimeMachine TimeMachine;

TimeMachine *time_machine_create(const char *recordings_base_dir);
void time_machine_destroy(TimeMachine *tm);

// Select the camera to browse (raw display title; sanitized internally to match
// the recorder's folder name). Triggers a rescan. Pass the same title the camera
// records under. No-op if unchanged.
void time_machine_set_camera(TimeMachine *tm, const char *title_utf8);

// Ask the worker to rescan the coverage index (e.g. periodically while open).
void time_machine_request_rescan(TimeMachine *tm);

// Seek to an absolute unix time and play from there.
void time_machine_seek(TimeMachine *tm, double unix_time);

void time_machine_set_playing(TimeMachine *tm, int playing);
int time_machine_is_playing(TimeMachine *tm);

// Current playhead (unix seconds). 0 if nothing selected yet.
double time_machine_playhead(TimeMachine *tm);

// 1 while the worker is presenting frames inside coverage; 0 in a gap / no data.
int time_machine_has_video(TimeMachine *tm);

// Copies up to `max` coverage segments into `out` (sorted by start). Returns the
// number copied. If out_min/out_max are non-NULL they receive the overall
// earliest start / latest end across ALL segments (not just the copied ones), or
// 0 when there is no coverage.
int time_machine_copy_segments(TimeMachine *tm, TmSeg *out, int max, double *out_min,
                               double *out_max);

// Pulls the latest decoded frame if newer than *serial. On a new frame it grows
// *buf to fit (SDL_realloc), copies YUV420P planes packed, sets *w/*h/*serial and
// returns 1. Returns 0 when there is no newer frame. *buf may start NULL/*cap 0.
int time_machine_fetch_frame(TimeMachine *tm, uint8_t **buf, int *cap, int *w, int *h,
                             uint32_t *serial);
