#include "ui/ui_render.h"

#include <math.h>
#include <string.h>

#include "core/util.h"

SDL_Texture *recreate_texture(SDL_Renderer *renderer, int w, int h) {
  SDL_Texture *tex =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);
  if (tex) {
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  }
  return tex;
}

void choose_matrix_grid(int count, int win_w, int win_h, int *out_rows, int *out_cols) {
  if (!out_rows || !out_cols) return;
  *out_rows = 1;
  *out_cols = 1;
  if (count <= 0 || win_w <= 0 || win_h <= 0) return;

  const float win_ratio = (float)win_w / (float)win_h;
  const float target_aspect = 16.0f / 9.0f;

  int best_r = 1;
  int best_c = count;
  float best_score = -1e30f;

  int max_r = count;
  if (max_r < 1) max_r = 1;
  for (int r = 1; r <= max_r; r++) {
    int c = (count + r - 1) / r;
    if (c < 1) c = 1;
    if (r * c < count) continue;

    float cell_w = (float)win_w / (float)c;
    float cell_h = (float)win_h / (float)r;
    float area = cell_w * cell_h;

    float cell_aspect = (cell_h > 0.0f) ? (cell_w / cell_h) : target_aspect;
    float aspect_pen = (cell_aspect > 0.0f) ? (float)fabs(log(cell_aspect / target_aspect)) : 10.0f;
    float empty = (float)(r * c - count) / (float)(r * c);

    float score = area * (1.0f - 0.35f * aspect_pen - 0.6f * empty);

    float eps = 0.0001f * (best_score != 0.0f ? (float)fabs(best_score) : 1.0f);
    int prefer = 0;
    if (fabs(score - best_score) <= eps) {
      if (win_ratio >= 1.0f) {
        if (c >= r && !(best_c >= best_r)) prefer = 1;
      } else {
        if (r >= c && !(best_r >= best_c)) prefer = 1;
      }
      if ((r * c) < (best_r * best_c)) prefer = 1;
    }

    if (score > best_score || prefer) {
      best_score = score;
      best_r = r;
      best_c = c;
    }
  }

  *out_rows = best_r;
  *out_cols = best_c;
}

void draw_speaker_icon(SDL_Renderer *r, int x, int y, int size, SDL_Color color) {
  if (!r || size < 8) return;
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);

  // Speaker body (rectangular slab) on the left ~20% of the bounding box.
  int body_w = size * 20 / 100;
  int body_h = size * 38 / 100;
  int body_x = x + size * 6 / 100;
  int body_y = y + (size - body_h) / 2;
  SDL_Rect body = {body_x, body_y, body_w, body_h};
  SDL_RenderFillRect(r, &body);

  // Cone: trapezoid extending right from speaker body. Narrow at the body edge,
  // tall at the far edge. Filled by vertical scan lines.
  int cone_left = body_x + body_w;
  int cone_right = x + size * 50 / 100;
  int cone_top_l = body_y;
  int cone_bot_l = body_y + body_h;
  int cone_top_r = y + size * 10 / 100;
  int cone_bot_r = y + size - size * 10 / 100;
  int span = cone_right - cone_left;
  if (span > 0) {
    for (int i = 0; i <= span; i++) {
      int t = cone_top_l + (cone_top_r - cone_top_l) * i / span;
      int b = cone_bot_l + (cone_bot_r - cone_bot_l) * i / span;
      SDL_RenderDrawLine(r, cone_left + i, t, cone_left + i, b);
    }
  }

  // Two sound-wave arcs on the right. Drawn as polylines with a small radial offset
  // to fake 2-pixel thickness (cheap; doesn't need a circle-anti-aliased rasterizer).
  int wave_cx = x + size * 50 / 100;
  int wave_cy = y + size / 2;
  for (int w = 0; w < 2; w++) {
    int radius = size * (24 + w * 14) / 100;
    int steps = 28;
    for (int thickness = 0; thickness < 2; thickness++) {
      int prev_x = -1, prev_y = -1;
      int r_eff = radius + thickness;
      for (int s = 0; s <= steps; s++) {
        double angle = -M_PI / 3.0 + (2.0 * M_PI / 3.0) * (double)s / (double)steps;
        int px = wave_cx + (int)(r_eff * cos(angle));
        int py = wave_cy + (int)(r_eff * sin(angle));
        if (prev_x >= 0) SDL_RenderDrawLine(r, prev_x, prev_y, px, py);
        prev_x = px;
        prev_y = py;
      }
    }
  }
}

void draw_filled_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color col) {
  if (!r || radius <= 0) return;
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
  int x = radius;
  int y = 0;
  int err = 1 - x;
  while (x >= y) {
    SDL_RenderDrawLine(r, cx - x, cy + y, cx + x, cy + y);
    SDL_RenderDrawLine(r, cx - x, cy - y, cx + x, cy - y);
    SDL_RenderDrawLine(r, cx - y, cy + x, cx + y, cy + x);
    SDL_RenderDrawLine(r, cx - y, cy - x, cx + y, cy - x);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

void draw_rec_dot(SDL_Renderer *r, SDL_Rect tile, int radius) {
  if (!r || radius <= 0) return;
  int margin = radius + 4;
  int cx = tile.x + tile.w - margin;
  int cy = tile.y + margin;
  SDL_Color red = {220, 30, 30, 180}; // matches the expanded-view indicator
  draw_filled_circle(r, cx, cy, radius, red);
}

uint64_t camera_ui_text_key_for_title(const char *title_utf8) {
  const char *t = (title_utf8 && *title_utf8) ? title_utf8 : "Camera";
  return fnv1a64(t, strlen(t));
}

void camera_ui_ensure_text(SDL_Renderer *renderer, CameraUiItem *ui, const CameraConfig *cam, int text_pt,
                           SDL_Color text_color, int *budget) {
  if (!renderer || !ui || !cam) return;
  if (budget && *budget <= 0) return;

  const char *title = (cam->title_utf8 && *cam->title_utf8) ? cam->title_utf8 : "Camera";
  uint64_t key = camera_ui_text_key_for_title(title);
  if (ui->text_tex && ui->text_key == key) return;

  if (ui->text_tex) {
    SDL_DestroyTexture(ui->text_tex);
    ui->text_tex = NULL;
  }
  ui->text_w = 0;
  ui->text_h = 0;
  ui->text_key = 0;

  TextUiItem t = make_text(renderer, title, text_pt, text_color);
  if (!t.tex) return;
  ui->text_tex = t.tex;
  ui->text_w = t.w;
  ui->text_h = t.h;
  ui->text_key = key;
  if (budget) (*budget)--;
}

int camera_ui_copy_thumb(CameraUiItem *ui, const CameraConfig *cam) {
  if (!ui || !cam) return 0;
  if (!cam->thumb_yuv || cam->thumb_yuv_size <= 0 || cam->thumb_w <= 0 || cam->thumb_h <= 0) return 0;
  if (cam->thumb_last_update_ticks == 0) return 0;
  if (cam->thumb_last_update_ticks == ui->thumb_stamp) return 0;

  if (!ui->thumb_copy || ui->thumb_copy_size != cam->thumb_yuv_size) {
    uint8_t *nb = (uint8_t *)SDL_realloc(ui->thumb_copy, (size_t)cam->thumb_yuv_size);
    if (!nb) return 0;
    ui->thumb_copy = nb;
    ui->thumb_copy_size = cam->thumb_yuv_size;
  }
  memcpy(ui->thumb_copy, cam->thumb_yuv, (size_t)cam->thumb_yuv_size);
  ui->thumb_copy_w = cam->thumb_w;
  ui->thumb_copy_h = cam->thumb_h;
  ui->thumb_copy_stamp = cam->thumb_last_update_ticks;
  return 1;
}

int camera_ui_copy_main(CameraUiItem *ui, const CameraConfig *cam) {
  if (!ui || !cam) return 0;
  if (!cam->last_yuv || cam->last_yuv_size <= 0 || cam->last_w <= 0 || cam->last_h <= 0) return 0;
  if (cam->last_seen_ticks == 0) return 0;
  if (cam->last_seen_ticks == ui->main_stamp) return 0;

  if (!ui->main_copy || ui->main_copy_size != cam->last_yuv_size) {
    uint8_t *nb = (uint8_t *)SDL_realloc(ui->main_copy, (size_t)cam->last_yuv_size);
    if (!nb) return 0;
    ui->main_copy = nb;
    ui->main_copy_size = cam->last_yuv_size;
  }
  memcpy(ui->main_copy, cam->last_yuv, (size_t)cam->last_yuv_size);
  ui->main_copy_w = cam->last_w;
  ui->main_copy_h = cam->last_h;
  ui->main_copy_stamp = cam->last_seen_ticks;
  return 1;
}

void camera_ui_update_thumb(SDL_Renderer *renderer, CameraUiItem *ui, const uint8_t *yuv, int w, int h,
                            Uint32 stamp) {
  if (!renderer || !ui) return;
  if (!yuv || w <= 0 || h <= 0 || stamp == 0) return;

  if (!ui->thumb_tex || ui->thumb_w != w || ui->thumb_h != h) {
    if (ui->thumb_tex) SDL_DestroyTexture(ui->thumb_tex);
    ui->thumb_tex = recreate_texture(renderer, w, h);
    if (!ui->thumb_tex) {
      ui->thumb_w = 0;
      ui->thumb_h = 0;
      return;
    }
    ui->thumb_w = w;
    ui->thumb_h = h;
  }

  const uint8_t *y = yuv;
  const uint8_t *u = y + (size_t)w * (size_t)h;
  const uint8_t *v = u + ((size_t)w * (size_t)h) / 4;
  SDL_UpdateYUVTexture(ui->thumb_tex, NULL, y, w, u, w / 2, v, w / 2);
  ui->thumb_stamp = stamp;
}

CameraUiItem *build_camera_ui_items(SDL_Renderer *renderer, const CameraConfig *cams,
                                    int count, int text_pt, SDL_Color text_color) {
  if (count <= 0) return NULL;
  CameraUiItem *ui = (CameraUiItem *)SDL_calloc((size_t)count, sizeof(CameraUiItem));
  if (!ui) return NULL;
  for (int i = 0; i < count; i++) {
    const char *title = (cams[i].title_utf8 && *cams[i].title_utf8) ? cams[i].title_utf8 : "Camera";
    TextUiItem t = make_text(renderer, title, text_pt, text_color);
    ui[i].text_tex = t.tex;
    ui[i].text_w = t.w;
    ui[i].text_h = t.h;
    ui[i].text_key = camera_ui_text_key_for_title(title);
    if (!ui[i].text_tex) {
      free_camera_ui(ui, count);
      return NULL;
    }

    ui[i].thumb_tex = NULL;
    ui[i].thumb_w = 0;
    ui[i].thumb_h = 0;
    ui[i].thumb_stamp = 0;
    ui[i].thumb_copy = NULL;
    ui[i].thumb_copy_size = 0;
    ui[i].thumb_copy_w = 0;
    ui[i].thumb_copy_h = 0;
    ui[i].thumb_copy_stamp = 0;
    if (cams[i].thumb_yuv && cams[i].thumb_yuv_size > 0 && cams[i].thumb_w > 0 && cams[i].thumb_h > 0) {
      camera_ui_update_thumb(renderer, &ui[i], cams[i].thumb_yuv, cams[i].thumb_w, cams[i].thumb_h,
                             cams[i].thumb_last_update_ticks);
    }
  }
  return ui;
}

void free_camera_ui(CameraUiItem *ui, int count) {
  if (!ui) return;
  for (int i = 0; i < count; i++) {
    if (ui[i].text_tex) SDL_DestroyTexture(ui[i].text_tex);
    if (ui[i].thumb_tex) SDL_DestroyTexture(ui[i].thumb_tex);
    SDL_free(ui[i].thumb_copy);
    SDL_free(ui[i].main_copy);
    destroy_text(&ui[i].stream_buf_pct_text);
  }
  SDL_free(ui);
}

static void draw_face_icon_ex(SDL_Renderer *r, int x, int y, int size, SDL_Color color, const SDL_Color *fill) {
  if (!r || size < 8) return;
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

  int cx = x + size / 2;
  int cy = y + size / 2;
  int radius = size * 44 / 100;

  if (fill) draw_filled_circle(r, cx, cy, radius, *fill);
  SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);

  // Head: 2px-thick circle outline as a polyline.
  const int steps = 40;
  for (int thickness = 0; thickness < 2; thickness++) {
    int r_eff = radius - thickness;
    int prev_x = 0, prev_y = 0;
    for (int s = 0; s <= steps; s++) {
      double a = 2.0 * M_PI * (double)s / (double)steps;
      int px = cx + (int)(r_eff * cos(a));
      int py = cy + (int)(r_eff * sin(a));
      if (s > 0) SDL_RenderDrawLine(r, prev_x, prev_y, px, py);
      prev_x = px;
      prev_y = py;
    }
  }

  // Eyes: two filled dots.
  int eye_r = size * 6 / 100;
  if (eye_r < 1) eye_r = 1;
  int eye_dx = size * 16 / 100;
  int eye_y = cy - size * 10 / 100;
  draw_filled_circle(r, cx - eye_dx, eye_y, eye_r, color);
  draw_filled_circle(r, cx + eye_dx, eye_y, eye_r, color);

  // Smile: lower arc, 2px thick.
  int smile_r = size * 26 / 100;
  int smile_cy = cy + size * 2 / 100;
  for (int thickness = 0; thickness < 2; thickness++) {
    int r_eff = smile_r + thickness;
    int prev_x = 0, prev_y = 0;
    const int ss = 20;
    for (int s = 0; s <= ss; s++) {
      double a = M_PI * 0.2 + M_PI * 0.6 * (double)s / (double)ss; // 36°..144° (below centre)
      int px = cx + (int)(r_eff * cos(a));
      int py = smile_cy + (int)(r_eff * sin(a));
      if (s > 0) SDL_RenderDrawLine(r, prev_x, prev_y, px, py);
      prev_x = px;
      prev_y = py;
    }
  }
}

void draw_face_icon(SDL_Renderer *r, int x, int y, int size, SDL_Color color) {
  draw_face_icon_ex(r, x, y, size, color, NULL);
}

void draw_face_icon_filled(SDL_Renderer *r, int x, int y, int size, SDL_Color color, SDL_Color fill) {
  draw_face_icon_ex(r, x, y, size, color, &fill);
}
