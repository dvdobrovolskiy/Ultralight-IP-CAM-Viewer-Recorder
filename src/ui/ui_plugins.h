#pragma once

#include <SDL.h>

#include "plugin_host.h"
#include "ui/text.h"
#include "ui/ui_render.h"

typedef struct PluginsOverlay {
  int is_open;
  int selected;
  int scroll;

  TextUiItem header;
  TextUiItem btn_rescan;
  TextUiItem btn_toggle;
  TextUiItem btn_close;

  CameraUiItem *list_ui;
  int list_count;
} PluginsOverlay;

void plugins_overlay_free_list(PluginsOverlay *p);
int plugins_overlay_rebuild_list(PluginsOverlay *p, SDL_Renderer *renderer, PluginHost *host,
                                 int text_pt, SDL_Color color);
int plugins_overlay_open(PluginsOverlay *p, SDL_Renderer *renderer, PluginHost *host,
                         int text_pt, SDL_Color color);
void plugins_overlay_close(PluginsOverlay *p);
void plugins_overlay_render(PluginsOverlay *p, SDL_Renderer *renderer, SDL_Window *window,
                            int text_pt, SDL_Color color);
