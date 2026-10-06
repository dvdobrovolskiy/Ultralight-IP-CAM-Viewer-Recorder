#include "ui/ui_timeline.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "ui/text.h"

#define TL_HEIGHT 72
#define TL_HEADER 20  // top band for tick labels / header text

SDL_Rect ui_timeline_rect(int win_w, int win_h) {
  SDL_Rect r = {0, win_h - TL_HEIGHT, win_w, TL_HEIGHT};
  if (r.y < 0) r.y = 0;
  return r;
}

static double tl_view_start(const UiTimeline *tl, SDL_Rect rect) {
  double half = (rect.w * 0.5) / (tl->pps > 0 ? tl->pps : 1.0);
  return tl->center - half;
}

static int tl_time_to_x(const UiTimeline *tl, SDL_Rect rect, double t) {
  double vs = tl_view_start(tl, rect);
  return rect.x + (int)((t - vs) * tl->pps + 0.5);
}

double ui_timeline_x_to_time(const UiTimeline *tl, SDL_Rect rect, int x) {
  double vs = tl_view_start(tl, rect);
  return vs + (double)(x - rect.x) / (tl->pps > 0 ? tl->pps : 1.0);
}

void ui_timeline_pan(UiTimeline *tl, SDL_Rect rect, int notches) {
  double visible = rect.w / (tl->pps > 0 ? tl->pps : 1.0);
  tl->center -= notches * visible * 0.15;  // ~15% of the window per notch
}

void ui_timeline_zoom(UiTimeline *tl, int focus_x, SDL_Rect rect, int notches) {
  double t_at_focus = ui_timeline_x_to_time(tl, rect, focus_x);
  double factor = (notches > 0) ? 1.0 : -1.0;
  for (int i = 0; i < (notches > 0 ? notches : -notches); i++) {
    tl->pps *= (factor > 0) ? 1.25 : (1.0 / 1.25);
  }
  // Clamp: from ~2 minutes across the strip to ~10 days across the strip.
  double min_pps = rect.w / (10.0 * 86400.0);
  double max_pps = rect.w / 120.0;
  if (tl->pps < min_pps) tl->pps = min_pps;
  if (tl->pps > max_pps) tl->pps = max_pps;
  // Keep the time under the cursor fixed while zooming.
  double half = (rect.w * 0.5) / tl->pps;
  double new_vs = t_at_focus - (double)(focus_x - rect.x) / tl->pps;
  tl->center = new_vs + half;
}

static double tl_nice_interval(double pps) {
  static const double cand[] = {60,    300,   900,    1800,   3600,
                                10800, 21600, 43200,  86400,  7 * 86400.0};
  int n = (int)(sizeof(cand) / sizeof(cand[0]));
  for (int i = 0; i < n; i++) {
    if (cand[i] * pps >= 90.0) return cand[i];
  }
  return cand[n - 1];
}

static void tl_label(SDL_Renderer *r, int x, int y, const char *s, int pt, SDL_Color c) {
  TextUiItem t = make_text(r, s, pt, c);
  if (t.tex) {
    SDL_Rect dr = {x, y, t.w, t.h};
    SDL_RenderCopy(r, t.tex, NULL, &dr);
  }
  destroy_text(&t);
}

void ui_timeline_render(SDL_Renderer *r, const UiTimeline *tl, SDL_Rect rect, const char *cam_title,
                        const TmSeg *segs, int nseg, double playhead, int has_video, int playing,
                        int text_pt) {
  // Background.
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, 18, 18, 26, 235);
  SDL_RenderFillRect(r, &rect);

  SDL_Rect band = {rect.x, rect.y + TL_HEADER, rect.w, rect.h - TL_HEADER};

  // Coverage band background (so "no video" reads as a distinct dark strip).
  SDL_SetRenderDrawColor(r, 34, 34, 44, 255);
  SDL_RenderFillRect(r, &band);

  double vs = tl_view_start(tl, rect);
  double ve = vs + rect.w / (tl->pps > 0 ? tl->pps : 1.0);

  // Coverage spans (green).
  SDL_SetRenderDrawColor(r, 46, 170, 80, 255);
  for (int i = 0; i < nseg; i++) {
    if (segs[i].end < vs || segs[i].start > ve) continue;
    int x0 = tl_time_to_x(tl, rect, segs[i].start);
    int x1 = tl_time_to_x(tl, rect, segs[i].end);
    if (x1 <= x0) x1 = x0 + 1;
    if (x0 < rect.x) x0 = rect.x;
    if (x1 > rect.x + rect.w) x1 = rect.x + rect.w;
    SDL_Rect g = {x0, band.y + 2, x1 - x0, band.h - 4};
    SDL_RenderFillRect(r, &g);
  }

  // Ticks + labels.
  double interval = tl_nice_interval(tl->pps);
  SDL_Color tickc = {150, 150, 165, 255};
  double first = (vs > 0) ? (floor(vs / interval) * interval) : 0;
  for (double t = first; t <= ve; t += interval) {
    int x = tl_time_to_x(tl, rect, t);
    if (x < rect.x - 1 || x > rect.x + rect.w) continue;
    time_t tt = (time_t)t;
    struct tm *lt = localtime(&tt);
    int is_midnight = lt && lt->tm_hour == 0 && lt->tm_min == 0;
    SDL_SetRenderDrawColor(r, is_midnight ? 200 : 70, is_midnight ? 200 : 70,
                           is_midnight ? 210 : 80, 255);
    SDL_RenderDrawLine(r, x, rect.y + TL_HEADER - 2, x, rect.y + rect.h);
    char buf[48];
    if (lt) {
      if (interval >= 86400.0) {
        strftime(buf, sizeof(buf), "%d.%m", lt);
      } else if (is_midnight) {
        strftime(buf, sizeof(buf), "%d.%m %H:%M", lt);
      } else {
        strftime(buf, sizeof(buf), "%H:%M", lt);
      }
      tl_label(r, x + 3, rect.y + 2, buf, text_pt > 13 ? 12 : text_pt, tickc);
    }
  }

  // Playhead.
  if (playhead > 0 && playhead >= vs && playhead <= ve) {
    int x = tl_time_to_x(tl, rect, playhead);
    SDL_SetRenderDrawColor(r, 255, 70, 70, 255);
    SDL_RenderDrawLine(r, x, rect.y + 2, x, rect.y + rect.h);
    SDL_RenderDrawLine(r, x - 1, rect.y + 2, x - 1, rect.y + rect.h);
    // Top marker triangle.
    for (int dy = 0; dy < 6; dy++) {
      SDL_RenderDrawLine(r, x - dy, rect.y + 2 + dy, x + dy, rect.y + 2 + dy);
    }
  }

  // Header text: camera + playhead clock + state.
  char header[256];
  char clock[32] = "--:--:--";
  char date[24] = "";
  if (playhead > 0) {
    time_t tt = (time_t)playhead;
    struct tm *lt = localtime(&tt);
    if (lt) {
      strftime(clock, sizeof(clock), "%H:%M:%S", lt);
      strftime(date, sizeof(date), "%d.%m.%Y", lt);
    }
  }
  snprintf(header, sizeof(header), "TIME MACHINE  %s   %s %s   %s%s",
           cam_title ? cam_title : "", date, clock, has_video ? "" : "[no video] ",
           playing ? "(playing)" : "(paused)");
  SDL_Color hc = {235, 235, 245, 255};
  tl_label(r, rect.x + 8, rect.y + 2, header, text_pt > 14 ? 13 : text_pt, hc);

  if (nseg == 0) {
    SDL_Color nc = {200, 180, 120, 255};
    tl_label(r, rect.x + rect.w / 2 - 70, band.y + band.h / 2 - 8, "No recordings for this camera",
             text_pt > 14 ? 13 : text_pt, nc);
  }

  // Top border.
  SDL_SetRenderDrawColor(r, 90, 90, 110, 255);
  SDL_RenderDrawLine(r, rect.x, rect.y, rect.x + rect.w, rect.y);
}
