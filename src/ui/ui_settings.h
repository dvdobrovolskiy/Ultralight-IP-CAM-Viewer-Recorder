#pragma once

#include <SDL.h>

#include "app/config.h"
#include "app/i18n.h"
#include "ui/text.h"

typedef struct SettingsOverlay {
  int is_open;
  int selected;
  char pending_quality[32];
  ViewMode pending_view_mode;
  PreviewMode pending_preview_mode;
  int pending_tiles_use_preview_url;

  // Recording storage limit in GB (0 = Off/unlimited), edited via a +/- stepper.
  // rendered_archive_gb tracks the value the cached val_archive texture shows so
  // it only rebuilds when the number actually changes.
  int pending_archive_gb;
  int rendered_archive_gb;

  // Phase 7 i18n: language picked in the overlay, applied on Save. `rendered_lang`
  // is what the cached text textures were generated in; when it differs from
  // pending_language the render path rebuilds them on the fly so language changes
  // preview live.
  LanguageCode pending_language;
  int rendered_lang;
  TextUiItem lbl_language;
  TextUiItem opt_lang_en;
  TextUiItem opt_lang_ru;

  TextUiItem header;
  TextUiItem lbl_quality;
  TextUiItem opt_nearest;
  TextUiItem opt_linear;
  TextUiItem opt_best;
  TextUiItem lbl_mode;
  TextUiItem opt_mode_tiles;
  TextUiItem opt_mode_matrix;
  TextUiItem lbl_preview_mode;
  TextUiItem lbl_preview_help1;
  TextUiItem lbl_preview_help2;
  TextUiItem opt_preview_static;
  TextUiItem opt_preview_warm;
  TextUiItem opt_preview_stream;
  TextUiItem lbl_stream_tiles_source;
  TextUiItem opt_stream_tiles_preview;
  TextUiItem opt_stream_tiles_main;
  TextUiItem lbl_archive;     // "Recording storage limit (GB):"
  TextUiItem val_archive;     // the current value, e.g. "100 GB" or "Off"
  TextUiItem opt_arch_minus;  // "-" stepper button glyph
  TextUiItem opt_arch_plus;   // "+" stepper button glyph
  TextUiItem btn_save;
  TextUiItem btn_cancel;
} SettingsOverlay;

int settings_open(SettingsOverlay *s, SDL_Renderer *renderer, const char *cur_quality,
                  ViewMode cur_view_mode, PreviewMode cur_preview_mode,
                  int cur_tiles_use_preview_url, int cur_archive_gb, int text_pt, SDL_Color color);

// Stepper bounds shared by the render geometry and the click handler.
#define SETTINGS_ARCHIVE_STEP_GB 50
#define SETTINGS_ARCHIVE_MAX_GB 100000
void settings_close(SettingsOverlay *s);
void settings_render(SettingsOverlay *s, SDL_Renderer *renderer, SDL_Window *window, int text_pt,
                     SDL_Color text_color);
int settings_save_to_ini(const char *settings_path, const SettingsOverlay *s);

void settings_preview_compute_layout(const SettingsOverlay *s, int preview_label_y, int pv_max_y,
                                     int *out_help_y, int *out_pv_y, int *out_gap_lines,
                                     int *out_gap_before_buttons);
