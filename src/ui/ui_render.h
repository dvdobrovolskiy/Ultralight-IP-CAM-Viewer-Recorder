#pragma once

#include <stdint.h>

#include <SDL.h>

#include "app/camera.h"
#include "ui/text.h"

typedef struct CameraUiItem {
  SDL_Texture *text_tex;
  int text_w;
  int text_h;
  uint64_t text_key;

  SDL_Texture *thumb_tex; // IYUV
  int thumb_w;
  int thumb_h;
  Uint32 thumb_stamp;

  uint8_t *thumb_copy;
  int thumb_copy_size;
  int thumb_copy_w;
  int thumb_copy_h;
  Uint32 thumb_copy_stamp;

  uint8_t *main_copy;
  int main_copy_size;
  int main_copy_w;
  int main_copy_h;
  Uint32 main_copy_stamp;
  Uint32 main_stamp;

  Uint32 stream_buf_since_ticks;
  int stream_buf_last_pct;
  TextUiItem stream_buf_pct_text;

  // Background plugin feed: last thumb stamp handed to plugins and when — so every
  // camera's stream reaches plugins (face detection) even while not displayed.
  Uint32 plugin_feed_stamp;
  Uint32 plugin_feed_ticks;
} CameraUiItem;

SDL_Texture *recreate_texture(SDL_Renderer *renderer, int w, int h);
void choose_matrix_grid(int count, int win_w, int win_h, int *out_rows, int *out_cols);
void draw_filled_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color col);

// Recording indicator: a translucent red dot inset into the top-right corner of
// `tile`. `radius` sizes the dot (use a small value for grid/tiles, ~16 for the
// expanded view). Drawn only as a visual marker; callers gate on record state.
void draw_rec_dot(SDL_Renderer *r, SDL_Rect tile, int radius);

// Audio-active indicator. `size` is the bounding box edge in pixels (typical 36-48).
// Drawn semi-transparently using `color`. (x, y) is the top-left of the bounding box;
// pick e.g. win_w-size-pad, win_h-size-pad for the bottom-right corner.
void draw_speaker_icon(SDL_Renderer *r, int x, int y, int size, SDL_Color color);
// Smiley-style face glyph (head outline, eyes, smile) — "face plugin active" indicator.
void draw_face_icon(SDL_Renderer *r, int x, int y, int size, SDL_Color color);
// Same glyph with the head filled with `fill` (features drawn in `color` on top) —
// "face detected right now" state.
void draw_face_icon_filled(SDL_Renderer *r, int x, int y, int size, SDL_Color color, SDL_Color fill);

uint64_t camera_ui_text_key_for_title(const char *title_utf8);
void camera_ui_ensure_text(SDL_Renderer *renderer, CameraUiItem *ui, const CameraConfig *cam,
                           int text_pt, SDL_Color text_color, int *budget);
int camera_ui_copy_thumb(CameraUiItem *ui, const CameraConfig *cam);
int camera_ui_copy_main(CameraUiItem *ui, const CameraConfig *cam);
void camera_ui_update_thumb(SDL_Renderer *renderer, CameraUiItem *ui, const uint8_t *yuv, int w, int h,
                            Uint32 stamp);

CameraUiItem *build_camera_ui_items(SDL_Renderer *renderer, const CameraConfig *cams,
                                    int count, int text_pt, SDL_Color text_color);
void free_camera_ui(CameraUiItem *ui, int count);
