#pragma once

#include <SDL.h>

typedef struct TextUiItem {
  SDL_Texture *tex;
  int w;
  int h;
} TextUiItem;

TextUiItem make_text(SDL_Renderer *renderer, const char *utf8, int pt_size, SDL_Color color);
void destroy_text(TextUiItem *t);
void text_set(SDL_Renderer *renderer, TextUiItem *t, const char *utf8, int text_pt, SDL_Color color);

int camui_measure_px(int pt_size, const char *utf8, int bytes);
int camui_utf8_prev(const char *s, int i);
int camui_utf8_next(const char *s, int i);
int camui_cursor_from_x(const char *s, int text_pt, int x_px);
int camui_insert_at(char *buf, size_t cap, int *cursor, int text_pt, const char *ins);
void camui_backspace_at(char *buf, int *cursor);
void camui_delete_at(char *buf, int *cursor);
