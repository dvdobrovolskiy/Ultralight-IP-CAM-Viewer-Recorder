#include "ui/ui_plugins.h"

#include <stdio.h>

#include "app/i18n.h"

void plugins_overlay_free_list(PluginsOverlay *p) {
  if (!p) return;
  free_camera_ui(p->list_ui, p->list_count);
  p->list_ui = NULL;
  p->list_count = 0;
}

int plugins_overlay_rebuild_list(PluginsOverlay *p, SDL_Renderer *renderer, PluginHost *host,
                                 int text_pt, SDL_Color color) {
  if (!p || !renderer || !host) return -1;
  plugins_overlay_free_list(p);

  if (host->mutex) SDL_LockMutex(host->mutex);
  int n = host->row_count;
  if (n <= 0) {
    if (host->mutex) SDL_UnlockMutex(host->mutex);
    p->selected = 0;
    p->scroll = 0;
    return 0;
  }

  CameraUiItem *ui = (CameraUiItem *)SDL_calloc((size_t)n, sizeof(CameraUiItem));
  if (!ui) {
    if (host->mutex) SDL_UnlockMutex(host->mutex);
    return -1;
  }

  for (int i = 0; i < n; i++) {
    PluginRow *r = &host->rows[i];
    const char *st = r->is_valid ? (r->enabled ? "ON" : "OFF") : "INVALID";
    const char *nm = (r->name_utf8[0] ? r->name_utf8 : r->filename_utf8);
    char label[512];
    if (r->version_utf8[0]) {
      snprintf(label, sizeof(label), "%s - %s (v%s)", st, nm, r->version_utf8);
    } else {
      snprintf(label, sizeof(label), "%s - %s", st, nm);
    }

    TextUiItem t = make_text(renderer, label, text_pt, color);
    ui[i].text_tex = t.tex;
    ui[i].text_w = t.w;
    ui[i].text_h = t.h;
    if (!ui[i].text_tex) {
      if (host->mutex) SDL_UnlockMutex(host->mutex);
      free_camera_ui(ui, n);
      return -1;
    }
  }
  if (host->mutex) SDL_UnlockMutex(host->mutex);

  p->list_ui = ui;
  p->list_count = n;
  if (p->selected < 0) p->selected = 0;
  if (p->selected >= n) p->selected = n - 1;
  if (p->scroll < 0) p->scroll = 0;
  if (p->scroll > n - 1) p->scroll = n - 1;
  return 0;
}

int plugins_overlay_open(PluginsOverlay *p, SDL_Renderer *renderer, PluginHost *host,
                         int text_pt, SDL_Color color) {
  if (!p || p->is_open) return 0;
  if (!renderer || !host) return -1;
  p->is_open = 1;
  p->selected = 0;
  p->scroll = 0;

  p->header     = make_text(renderer, tr(STR_PLUGINS_TITLE), text_pt, color);
  p->btn_rescan = make_text(renderer, tr(STR_RESCAN),         text_pt, color);
  p->btn_toggle = make_text(renderer, tr(STR_TOGGLE),         text_pt, color);
  p->btn_close  = make_text(renderer, tr(STR_CLOSE),          text_pt, color);

  if (plugins_overlay_rebuild_list(p, renderer, host, text_pt, color) != 0) return -1;
  return 0;
}

void plugins_overlay_close(PluginsOverlay *p) {
  if (!p || !p->is_open) return;
  plugins_overlay_free_list(p);
  destroy_text(&p->header);
  destroy_text(&p->btn_rescan);
  destroy_text(&p->btn_toggle);
  destroy_text(&p->btn_close);
  p->is_open = 0;
}

void plugins_overlay_render(PluginsOverlay *p, SDL_Renderer *renderer, SDL_Window *window,
                            int text_pt, SDL_Color color) {
  (void)text_pt;
  (void)color;
  if (!p || !p->is_open || !renderer || !window) return;
  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(window, &win_w, &win_h);
  if (win_w <= 0 || win_h <= 0) return;

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
  SDL_Rect shade = {0, 0, win_w, win_h};
  SDL_RenderFillRect(renderer, &shade);

  SDL_Rect panel = {win_w / 2 - 450, win_h / 2 - 320, 900, 640};
  if (panel.x < 20) panel.x = 20;
  if (panel.y < 20) panel.y = 20;
  if (panel.x + panel.w > win_w - 20) panel.w = win_w - panel.x - 20;
  if (panel.y + panel.h > win_h - 20) panel.h = win_h - panel.y - 20;

  SDL_SetRenderDrawColor(renderer, 25, 25, 25, 245);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, 160, 160, 160, 255);
  SDL_RenderDrawRect(renderer, &panel);

  if (p->header.tex) {
    SDL_Rect r = {panel.x + (panel.w - p->header.w) / 2, panel.y + 18, p->header.w, p->header.h};
    SDL_RenderCopy(renderer, p->header.tex, NULL, &r);
  }

  SDL_Rect list_rect = {panel.x + 20, panel.y + 70, panel.w - 40, panel.h - 70 - 44 - 26 - 10};
  SDL_SetRenderDrawColor(renderer, 15, 15, 15, 255);
  SDL_RenderFillRect(renderer, &list_rect);
  SDL_SetRenderDrawColor(renderer, 120, 120, 120, 255);
  SDL_RenderDrawRect(renderer, &list_rect);

  const int row_h = 34;
  int visible = (list_rect.h / row_h);
  if (visible < 1) visible = 1;
  int max_scroll = (p->list_count > visible) ? (p->list_count - visible) : 0;
  if (p->scroll < 0) p->scroll = 0;
  if (p->scroll > max_scroll) p->scroll = max_scroll;

  for (int i = 0; i < visible; i++) {
    int idx = p->scroll + i;
    if (idx >= p->list_count) break;
    int y = list_rect.y + i * row_h;
    SDL_Rect row = {list_rect.x + 2, y + 2, list_rect.w - 4, row_h - 4};
    if (idx == p->selected) SDL_SetRenderDrawColor(renderer, 40, 120, 40, 220);
    else SDL_SetRenderDrawColor(renderer, 35, 35, 35, 255);
    SDL_RenderFillRect(renderer, &row);
    if (p->list_ui && p->list_ui[idx].text_tex) {
      SDL_Rect tr = {row.x + 10, row.y + (row.h - p->list_ui[idx].text_h) / 2, p->list_ui[idx].text_w,
                     p->list_ui[idx].text_h};
      SDL_RenderCopy(renderer, p->list_ui[idx].text_tex, NULL, &tr);
    }
  }

  const int btn_h = 44;
  const int btn_w = 160;
  const int gap = 12;
  SDL_Rect btn_rescan = {panel.x + panel.w / 2 - (btn_w * 3 + gap * 2) / 2, panel.y + panel.h - btn_h - 26,
                         btn_w, btn_h};
  SDL_Rect btn_toggle = {btn_rescan.x + btn_w + gap, btn_rescan.y, btn_w, btn_h};
  SDL_Rect btn_close = {btn_toggle.x + btn_w + gap, btn_rescan.y, btn_w, btn_h};

  SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
  SDL_RenderFillRect(renderer, &btn_rescan);
  SDL_RenderFillRect(renderer, &btn_toggle);
  SDL_RenderFillRect(renderer, &btn_close);
  SDL_SetRenderDrawColor(renderer, 160, 160, 160, 255);
  SDL_RenderDrawRect(renderer, &btn_rescan);
  SDL_RenderDrawRect(renderer, &btn_toggle);
  SDL_RenderDrawRect(renderer, &btn_close);

  TextUiItem *labels[] = {&p->btn_rescan, &p->btn_toggle, &p->btn_close};
  SDL_Rect *btns[] = {&btn_rescan, &btn_toggle, &btn_close};
  for (int i = 0; i < 3; i++) {
    if (labels[i]->tex) {
      SDL_Rect tr = {btns[i]->x + (btns[i]->w - labels[i]->w) / 2,
                     btns[i]->y + (btns[i]->h - labels[i]->h) / 2, labels[i]->w, labels[i]->h};
      SDL_RenderCopy(renderer, labels[i]->tex, NULL, &tr);
    }
  }
}
