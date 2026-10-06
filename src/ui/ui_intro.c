#include "ui/ui_intro.h"

#include "app/i18n.h"

int intro_open(IntroOverlay *in, SDL_Renderer *renderer, int text_pt, SDL_Color color,
               int show_at_startup) {
  if (!in || in->is_open) return 0;
  if (!renderer) return -1;
  in->is_open = 1;
  in->line_count = 0;
  in->show_at_startup = show_at_startup ? 1 : 0;
  in->chk_rect_x = in->chk_rect_y = in->chk_rect_w = in->chk_rect_h = -1;

  in->header = make_text(renderer, tr(STR_WELCOME), text_pt, color);
  const StringId line_ids[] = {
      STR_INTRO_F1,
      STR_INTRO_F2,
      STR_INTRO_F3,
      STR_INTRO_F5,
      STR_INTRO_F9,
      STR_INTRO_F10,
      STR_INTRO_F11,
      STR_INTRO_PLUS_MINUS,
      STR_INTRO_PTZ,
  };
  for (int i = 0;
       i < (int)(sizeof(line_ids) / sizeof(line_ids[0])) &&
       i < (int)(sizeof(in->lines) / sizeof(in->lines[0]));
       i++) {
    in->lines[in->line_count++] = make_text(renderer, tr(line_ids[i]), text_pt, color);
  }
  in->chk_label = make_text(renderer, tr(STR_SHOW_AT_STARTUP), text_pt, color);
  in->tip = make_text(renderer, tr(STR_INTRO_TIP), text_pt, color);
  return 0;
}

void intro_close(IntroOverlay *in) {
  if (!in || !in->is_open) return;
  destroy_text(&in->header);
  for (int i = 0; i < in->line_count; i++) {
    destroy_text(&in->lines[i]);
  }
  in->line_count = 0;
  destroy_text(&in->chk_label);
  destroy_text(&in->tip);
  in->is_open = 0;
}

void intro_render(IntroOverlay *in, SDL_Renderer *renderer, SDL_Window *window, int text_pt,
                  SDL_Color text_color) {
  (void)text_pt;
  (void)text_color;
  if (!in || !in->is_open || !renderer || !window) return;
  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(window, &win_w, &win_h);
  if (win_w <= 0 || win_h <= 0) return;

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
  SDL_Rect shade = {0, 0, win_w, win_h};
  SDL_RenderFillRect(renderer, &shade);

  // Taller panel + flow-based vertical layout (tip placed *after* the checkbox
  // rather than bottom-aligned to the panel) so a longer line list or a larger
  // checkbox row can't push the tip onto the checkbox.
  SDL_Rect panel = {win_w / 2 - 360, win_h / 2 - 250, 720, 500};
  if (panel.x < 20) panel.x = 20;
  if (panel.y < 20) panel.y = 20;
  if (panel.x + panel.w > win_w - 20) panel.w = win_w - panel.x - 20;
  if (panel.y + panel.h > win_h - 20) panel.h = win_h - panel.y - 20;

  SDL_SetRenderDrawColor(renderer, 25, 25, 25, 245);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, 160, 160, 160, 255);
  SDL_RenderDrawRect(renderer, &panel);

  int x = panel.x + 40;
  int y = panel.y + 28;

  if (in->header.tex) {
    SDL_Rect r = {panel.x + (panel.w - in->header.w) / 2, y, in->header.w, in->header.h};
    SDL_RenderCopy(renderer, in->header.tex, NULL, &r);
  }
  y += 70;

  for (int i = 0; i < in->line_count; i++) {
    if (!in->lines[i].tex) continue;
    SDL_Rect r = {x, y, in->lines[i].w, in->lines[i].h};
    SDL_RenderCopy(renderer, in->lines[i].tex, NULL, &r);
    y += in->lines[i].h + 14;
  }

  // "[ ] Don't show at startup" — small filled square + label. The combined
  // rect (square + gap + label) is the click hit area for toggling the tick.
  {
    int box_size = (in->chk_label.h > 0) ? in->chk_label.h : 18;
    int box_x = x;
    int box_y = y + 6;
    int gap = 10;
    int label_w = in->chk_label.w;
    int hit_w  = box_size + gap + label_w;
    int hit_h  = (box_size > in->chk_label.h) ? box_size : in->chk_label.h;
    in->chk_rect_x = box_x;
    in->chk_rect_y = box_y;
    in->chk_rect_w = hit_w;
    in->chk_rect_h = hit_h;

    SDL_Rect box = {box_x, box_y, box_size, box_size};
    SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    SDL_RenderDrawRect(renderer, &box);
    if (in->show_at_startup) {
      // Two diagonals make a clear X mark inside the square.
      SDL_SetRenderDrawColor(renderer, 90, 230, 110, 255);
      int inset = box_size / 5;
      for (int t = -1; t <= 1; t++) {
        SDL_RenderDrawLine(renderer,
                           box.x + inset, box.y + inset + t,
                           box.x + box.w - inset, box.y + box.h - inset + t);
        SDL_RenderDrawLine(renderer,
                           box.x + box.w - inset, box.y + inset + t,
                           box.x + inset, box.y + box.h - inset + t);
      }
    }

    if (in->chk_label.tex) {
      SDL_Rect lr = {box_x + box_size + gap,
                     box_y + (box_size - in->chk_label.h) / 2,
                     in->chk_label.w, in->chk_label.h};
      SDL_RenderCopy(renderer, in->chk_label.tex, NULL, &lr);
    }
    // Advance y past the checkbox row so the tip sits clearly below it.
    y = box_y + ((box_size > in->chk_label.h) ? box_size : in->chk_label.h) + 22;
  }

  if (in->tip.tex) {
    SDL_Rect r = {x, y, in->tip.w, in->tip.h};
    SDL_RenderCopy(renderer, in->tip.tex, NULL, &r);
  }
}
