#pragma once

#include <SDL.h>

#include "ui/text.h"

typedef struct IntroOverlay {
  int is_open;
  TextUiItem header;
  TextUiItem lines[12];
  int line_count;
  TextUiItem tip;
  TextUiItem chk_label;
  // 1 == "Show at startup" is ticked (default). Main reads this after
  // intro_close and writes it directly to settings.ini[App].IntroAtStartup.
  int show_at_startup;
  // Bounding rect of the checkbox+label hit area in the last render, so the
  // main click handler can detect clicks on the tick without re-deriving the
  // layout. (-1 if not laid out yet.)
  int chk_rect_x, chk_rect_y, chk_rect_w, chk_rect_h;
} IntroOverlay;

int intro_open(IntroOverlay *in, SDL_Renderer *renderer, int text_pt, SDL_Color color,
               int show_at_startup);
void intro_close(IntroOverlay *in);
void intro_render(IntroOverlay *in, SDL_Renderer *renderer, SDL_Window *window, int text_pt,
                  SDL_Color text_color);
