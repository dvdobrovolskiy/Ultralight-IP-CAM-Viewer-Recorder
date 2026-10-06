#pragma once

#include <SDL.h>

#include "cam/time_machine.h"

// Bottom time strip for the "time machine". Pure view-state + rendering helpers;
// the actual data + playback live in TimeMachine. main.c owns one UiTimeline,
// toggles `open` with F9, and feeds wheel/click input through the helpers below.
typedef struct UiTimeline {
  int open;
  double center;  // unix seconds at the horizontal center of the strip
  double pps;     // zoom: pixels per second
  int initialized;
} UiTimeline;

// The strip's screen rect (bottom of the window). Height is fixed.
SDL_Rect ui_timeline_rect(int win_w, int win_h);

// Map a screen x within the strip to an absolute unix time, and back.
double ui_timeline_x_to_time(const UiTimeline *tl, SDL_Rect rect, int x);

// Pan by mouse-wheel notches (positive = wheel up = back in time).
void ui_timeline_pan(UiTimeline *tl, SDL_Rect rect, int notches);

// Zoom around screen x `focus_x` by wheel notches (positive = zoom in).
void ui_timeline_zoom(UiTimeline *tl, int focus_x, SDL_Rect rect, int notches);

// Draw the strip: dark background, green coverage spans, time ticks/labels, the
// red playhead at `playhead`, and a header line (camera + clock + state).
void ui_timeline_render(SDL_Renderer *r, const UiTimeline *tl, SDL_Rect rect, const char *cam_title,
                        const TmSeg *segs, int nseg, double playhead, int has_video, int playing,
                        int text_pt);
