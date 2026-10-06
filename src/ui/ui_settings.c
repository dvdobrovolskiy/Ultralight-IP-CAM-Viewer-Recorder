#include "ui/ui_settings.h"

#include <stdio.h>

// (Re)build the archive-limit value texture to match pending_archive_gb.
// Called from settings_build_texts and lazily from settings_render whenever the
// value changes via the stepper. "Off" never means unlimited — the trimmer then
// caps the archive at a percentage of the drive — so the value says so.
static void settings_build_archive_value(SettingsOverlay *s, SDL_Renderer *renderer, int text_pt,
                                         SDL_Color color) {
  destroy_text(&s->val_archive);
  char buf[96];
  if (s->pending_archive_gb <= 0) {
    snprintf(buf, sizeof(buf), "%s (%s)", tr_in(s->pending_language, STR_OFF),
             tr_in(s->pending_language, STR_AUTO_DRIVE_LIMIT));
  } else {
    snprintf(buf, sizeof(buf), "%d GB", s->pending_archive_gb);
  }
  s->val_archive = make_text(renderer, buf, text_pt, color);
  s->rendered_archive_gb = s->pending_archive_gb;
}

void settings_preview_compute_layout(const SettingsOverlay *s, int preview_label_y, int pv_max_y,
                                     int *out_help_y, int *out_pv_y, int *out_gap_lines,
                                     int *out_gap_before_buttons) {
  int help_y_min = preview_label_y + 34;

  int gap_lines = 8;
  int gap_before_buttons = 10;

  int h1 = (s && s->lbl_preview_help1.tex) ? s->lbl_preview_help1.h : 0;
  int h2 = (s && s->lbl_preview_help2.tex) ? s->lbl_preview_help2.h : 0;

  int label_h = (s && s->lbl_preview_mode.tex) ? s->lbl_preview_mode.h : 0;
  int help_y_floor = preview_label_y + label_h + 6;
  if (help_y_min < help_y_floor) help_y_min = help_y_floor;

  int help_h = 0;
  if (h1 > 0) help_h += h1;
  if (h2 > 0) {
    if (help_h > 0) help_h += gap_lines;
    help_h += h2;
  }

  int help_y = help_y_min;
  int pv_y = help_y + help_h + gap_before_buttons;

  if (pv_y > pv_max_y) {
    gap_before_buttons = 2;
    if (h1 > 0 && h2 > 0) gap_lines = 2;

    help_h = 0;
    if (h1 > 0) help_h += h1;
    if (h2 > 0) {
      if (help_h > 0) help_h += gap_lines;
      help_h += h2;
    }
    pv_y = help_y + help_h + gap_before_buttons;

    if (pv_y > pv_max_y) {
      pv_y = pv_max_y;
      help_y = pv_y - (help_h + gap_before_buttons);
      if (help_y < help_y_min) {
        help_y = help_y_min;
        pv_y = help_y + help_h + gap_before_buttons;
        if (pv_y > pv_max_y) pv_y = pv_max_y;
      }
    }
  }

  if (out_help_y) *out_help_y = help_y;
  if (out_pv_y) *out_pv_y = pv_y;
  if (out_gap_lines) *out_gap_lines = gap_lines;
  if (out_gap_before_buttons) *out_gap_before_buttons = gap_before_buttons;
}

// Rebuild every text texture in `s` from the strings in s->pending_language.
// Called from settings_open initially, and from settings_render whenever the
// pending language has changed (live preview as the user toggles EN/RU).
static void settings_build_texts(SettingsOverlay *s, SDL_Renderer *renderer, int text_pt,
                                 SDL_Color color) {
  LanguageCode L = s->pending_language;
  destroy_text(&s->header);
  destroy_text(&s->lbl_quality);
  destroy_text(&s->opt_nearest);
  destroy_text(&s->opt_linear);
  destroy_text(&s->opt_best);
  destroy_text(&s->lbl_mode);
  destroy_text(&s->opt_mode_tiles);
  destroy_text(&s->opt_mode_matrix);
  destroy_text(&s->lbl_preview_mode);
  destroy_text(&s->lbl_preview_help1);
  destroy_text(&s->lbl_preview_help2);
  destroy_text(&s->opt_preview_static);
  destroy_text(&s->opt_preview_warm);
  destroy_text(&s->opt_preview_stream);
  destroy_text(&s->lbl_stream_tiles_source);
  destroy_text(&s->opt_stream_tiles_preview);
  destroy_text(&s->opt_stream_tiles_main);
  destroy_text(&s->lbl_archive);
  destroy_text(&s->val_archive);
  destroy_text(&s->opt_arch_minus);
  destroy_text(&s->opt_arch_plus);
  destroy_text(&s->lbl_language);
  destroy_text(&s->opt_lang_en);
  destroy_text(&s->opt_lang_ru);
  destroy_text(&s->btn_save);
  destroy_text(&s->btn_cancel);

  s->header                   = make_text(renderer, tr_in(L, STR_SETTINGS_TITLE),      text_pt, color);
  s->lbl_quality              = make_text(renderer, tr_in(L, STR_RENDER_QUALITY),      text_pt, color);
  // "nearest"/"linear"/"best" are not translated — they're INI option keys.
  s->opt_nearest              = make_text(renderer, "nearest",                         text_pt, color);
  s->opt_linear               = make_text(renderer, "linear",                          text_pt, color);
  s->opt_best                 = make_text(renderer, "best",                            text_pt, color);
  s->lbl_mode                 = make_text(renderer, tr_in(L, STR_VIEW_MODE),           text_pt, color);
  s->opt_mode_tiles           = make_text(renderer, tr_in(L, STR_TILES),               text_pt, color);
  s->opt_mode_matrix          = make_text(renderer, tr_in(L, STR_MATRIX),              text_pt, color);
  s->lbl_preview_mode         = make_text(renderer, tr_in(L, STR_PREVIEW_MODE),        text_pt, color);
  int help_pt = (text_pt > 12) ? (text_pt - 3) : text_pt;
  s->lbl_preview_help1        = make_text(renderer, tr_in(L, STR_PREVIEW_HELP1),       help_pt, color);
  s->lbl_preview_help2        = make_text(renderer, tr_in(L, STR_PREVIEW_HELP2),       help_pt, color);
  s->opt_preview_static       = make_text(renderer, tr_in(L, STR_STATIC),              text_pt, color);
  s->opt_preview_warm         = make_text(renderer, tr_in(L, STR_WARM),                text_pt, color);
  s->opt_preview_stream       = make_text(renderer, tr_in(L, STR_STREAM),              text_pt, color);
  s->lbl_stream_tiles_source  = make_text(renderer, tr_in(L, STR_STREAM_TILES_SOURCE), text_pt, color);
  s->opt_stream_tiles_preview = make_text(renderer, tr_in(L, STR_OPT_PREVIEW_URL),     text_pt, color);
  s->opt_stream_tiles_main    = make_text(renderer, tr_in(L, STR_OPT_MAIN_URL),        text_pt, color);
  s->lbl_archive              = make_text(renderer, tr_in(L, STR_RECORDING_LIMIT),     text_pt, color);
  s->opt_arch_minus           = make_text(renderer, "-",                               text_pt, color);
  s->opt_arch_plus            = make_text(renderer, "+",                               text_pt, color);
  settings_build_archive_value(s, renderer, text_pt, color);
  s->lbl_language             = make_text(renderer, tr_in(L, STR_LANGUAGE),            text_pt, color);
  s->opt_lang_en              = make_text(renderer, tr_in(L, STR_LANG_ENGLISH),        text_pt, color);
  s->opt_lang_ru              = make_text(renderer, tr_in(L, STR_LANG_RUSSIAN),        text_pt, color);
  s->btn_save                 = make_text(renderer, tr_in(L, STR_SAVE),                text_pt, color);
  s->btn_cancel               = make_text(renderer, tr_in(L, STR_CANCEL),              text_pt, color);

  s->rendered_lang = (int)L;
}

int settings_open(SettingsOverlay *s, SDL_Renderer *renderer, const char *cur_quality,
                  ViewMode cur_view_mode, PreviewMode cur_preview_mode,
                  int cur_tiles_use_preview_url, int cur_archive_gb, int text_pt, SDL_Color color) {
  if (!s || s->is_open) return 0;
  if (!renderer) return -1;

  s->is_open = 1;
  s->selected = 0;
  snprintf(s->pending_quality, sizeof(s->pending_quality), "%s",
           normalize_scale_quality(cur_quality));
  s->pending_view_mode = cur_view_mode;
  s->pending_preview_mode = cur_preview_mode;
  s->pending_tiles_use_preview_url = cur_tiles_use_preview_url ? 1 : 0;
  if (cur_archive_gb < 0) cur_archive_gb = 0;
  if (cur_archive_gb > SETTINGS_ARCHIVE_MAX_GB) cur_archive_gb = SETTINGS_ARCHIVE_MAX_GB;
  s->pending_archive_gb = cur_archive_gb;
  s->rendered_archive_gb = -1;
  s->pending_language = i18n_get_language();
  s->rendered_lang = -1;

  settings_build_texts(s, renderer, text_pt, color);
  return 0;
}

void settings_close(SettingsOverlay *s) {
  if (!s || !s->is_open) return;
  destroy_text(&s->header);
  destroy_text(&s->lbl_quality);
  destroy_text(&s->opt_nearest);
  destroy_text(&s->opt_linear);
  destroy_text(&s->opt_best);
  destroy_text(&s->lbl_mode);
  destroy_text(&s->opt_mode_tiles);
  destroy_text(&s->opt_mode_matrix);
  destroy_text(&s->lbl_preview_mode);
  destroy_text(&s->lbl_preview_help1);
  destroy_text(&s->lbl_preview_help2);
  destroy_text(&s->opt_preview_static);
  destroy_text(&s->opt_preview_warm);
  destroy_text(&s->opt_preview_stream);
  destroy_text(&s->lbl_stream_tiles_source);
  destroy_text(&s->opt_stream_tiles_preview);
  destroy_text(&s->opt_stream_tiles_main);
  destroy_text(&s->lbl_archive);
  destroy_text(&s->val_archive);
  destroy_text(&s->opt_arch_minus);
  destroy_text(&s->opt_arch_plus);
  destroy_text(&s->lbl_language);
  destroy_text(&s->opt_lang_en);
  destroy_text(&s->opt_lang_ru);
  destroy_text(&s->btn_save);
  destroy_text(&s->btn_cancel);
  s->is_open = 0;
}

void settings_render(SettingsOverlay *s, SDL_Renderer *renderer, SDL_Window *window, int text_pt,
                     SDL_Color text_color) {
  if (!s || !s->is_open || !renderer || !window) return;

  // Live-preview language switch: if the user just clicked EN/RU the click
  // handler bumped pending_language; rebuild every cached text texture now so
  // the panel redraws in the new language immediately.
  if (s->rendered_lang != (int)s->pending_language) {
    settings_build_texts(s, renderer, text_pt, text_color);
  }
  // The archive value text is dynamic (changes as the stepper is clicked); rebuild
  // it only when the number actually changed to avoid per-frame texture churn.
  if (s->rendered_archive_gb != s->pending_archive_gb) {
    settings_build_archive_value(s, renderer, text_pt, text_color);
  }
  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(window, &win_w, &win_h);
  if (win_w <= 0 || win_h <= 0) return;

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
  SDL_Rect shade = {0, 0, win_w, win_h};
  SDL_RenderFillRect(renderer, &shade);

  // Larger panel with its own row for the language picker — fits the longer
  // Russian strings without the help/preview/stream-source/Save rows piling on
  // top of each other.
  SDL_Rect panel = {win_w / 2 - 380, win_h / 2 - 450, 760, 900};
  if (panel.x < 20) panel.x = 20;
  if (panel.y < 20) panel.y = 20;
  if (panel.x + panel.w > win_w - 20) panel.w = win_w - panel.x - 20;
  if (panel.y + panel.h > win_h - 20) panel.h = win_h - panel.y - 20;

  SDL_SetRenderDrawColor(renderer, 25, 25, 25, 245);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, 160, 160, 160, 255);
  SDL_RenderDrawRect(renderer, &panel);

  if (s->header.tex) {
    SDL_Rect r = {panel.x + (panel.w - s->header.w) / 2, panel.y + 18, s->header.w, s->header.h};
    SDL_RenderCopy(renderer, s->header.tex, NULL, &r);
  }

  // Language picker on its own row directly below the header — no risk of
  // overlapping the (variable-width) localized header text.
  {
    int lang_btn_h = 30;
    int lang_btn_w = 110;
    int lang_y = panel.y + 18 + 28 + 10;   // header_h ≈ 28, then 10 px gap
    int total_w = lang_btn_w * 2 + 12;
    int en_x = panel.x + (panel.w - total_w) / 2;
    SDL_Rect btn_en = {en_x,                       lang_y, lang_btn_w, lang_btn_h};
    SDL_Rect btn_ru = {en_x + lang_btn_w + 12,     lang_y, lang_btn_w, lang_btn_h};

    SDL_Color bg_sel = {40, 120, 40, 220};
    SDL_Color bg_unsel = {45, 45, 45, 255};
    SDL_Color bd_sel = {200, 200, 200, 255};
    SDL_Color bd_unsel = {140, 140, 140, 255};

    SDL_Color en_bg = (s->pending_language == LANG_EN) ? bg_sel : bg_unsel;
    SDL_Color en_bd = (s->pending_language == LANG_EN) ? bd_sel : bd_unsel;
    SDL_Color ru_bg = (s->pending_language == LANG_RU) ? bg_sel : bg_unsel;
    SDL_Color ru_bd = (s->pending_language == LANG_RU) ? bd_sel : bd_unsel;

    SDL_SetRenderDrawColor(renderer, en_bg.r, en_bg.g, en_bg.b, en_bg.a);
    SDL_RenderFillRect(renderer, &btn_en);
    SDL_SetRenderDrawColor(renderer, en_bd.r, en_bd.g, en_bd.b, en_bd.a);
    SDL_RenderDrawRect(renderer, &btn_en);
    if (s->opt_lang_en.tex) {
      SDL_Rect tr = {btn_en.x + (btn_en.w - s->opt_lang_en.w) / 2,
                     btn_en.y + (btn_en.h - s->opt_lang_en.h) / 2,
                     s->opt_lang_en.w, s->opt_lang_en.h};
      SDL_RenderCopy(renderer, s->opt_lang_en.tex, NULL, &tr);
    }

    SDL_SetRenderDrawColor(renderer, ru_bg.r, ru_bg.g, ru_bg.b, ru_bg.a);
    SDL_RenderFillRect(renderer, &btn_ru);
    SDL_SetRenderDrawColor(renderer, ru_bd.r, ru_bd.g, ru_bd.b, ru_bd.a);
    SDL_RenderDrawRect(renderer, &btn_ru);
    if (s->opt_lang_ru.tex) {
      SDL_Rect tr = {btn_ru.x + (btn_ru.w - s->opt_lang_ru.w) / 2,
                     btn_ru.y + (btn_ru.h - s->opt_lang_ru.h) / 2,
                     s->opt_lang_ru.w, s->opt_lang_ru.h};
      SDL_RenderCopy(renderer, s->opt_lang_ru.tex, NULL, &tr);
    }
  }

  int x = panel.x + 40;
  // Quality section starts after the language row (header 28 + gap 10 + buttons 30 + gap 20 = 88).
  int y = panel.y + 110;
  if (s->lbl_quality.tex) {
    SDL_Rect r = {x, y, s->lbl_quality.w, s->lbl_quality.h};
    SDL_RenderCopy(renderer, s->lbl_quality.tex, NULL, &r);
  }

  y += 60;
  SDL_Rect opt_box = {x, y, panel.w - 80, 44};
  const int gap = 12;
  const int btn_h = 44;
  const int btn_w = 200;   // wider for longer Russian "Сохранить" / "Отмена"
  SDL_Rect btn_save = {panel.x + panel.w / 2 - btn_w - gap / 2, panel.y + panel.h - btn_h - 26, btn_w, btn_h};
  SDL_Rect btn_cancel = {panel.x + panel.w / 2 + gap / 2, panel.y + panel.h - btn_h - 26, btn_w, btn_h};

  struct {
    const char *key;
    TextUiItem *t;
  } opts[] = {
      {"nearest", &s->opt_nearest},
      {"linear", &s->opt_linear},
      {"best", &s->opt_best},
  };

  for (int i = 0; i < 3; i++) {
    SDL_Color bg = {45, 45, 45, 255};
    SDL_Color border = {140, 140, 140, 255};
    if (SDL_strcasecmp(s->pending_quality, opts[i].key) == 0) {
      bg = (SDL_Color){40, 120, 40, 220};
      border = (SDL_Color){200, 200, 200, 255};
    }
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(renderer, &opt_box);
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(renderer, &opt_box);

    if (opts[i].t->tex) {
      SDL_Rect tr = {opt_box.x + 14, opt_box.y + (opt_box.h - opts[i].t->h) / 2, opts[i].t->w,
                     opts[i].t->h};
      SDL_RenderCopy(renderer, opts[i].t->tex, NULL, &tr);
    }
    opt_box.y += opt_box.h + gap;
  }

  // Recording storage limit: label + [-] <value> [+] stepper row.
  {
    int arch_label_y = opt_box.y + 10;
    if (s->lbl_archive.tex) {
      SDL_Rect r = {x, arch_label_y, s->lbl_archive.w, s->lbl_archive.h};
      SDL_RenderCopy(renderer, s->lbl_archive.tex, NULL, &r);
    }
    int arch_row_y = arch_label_y + 34;
    SDL_Rect arch_minus = {x, arch_row_y, 44, 44};
    SDL_Rect arch_value = {x + 44 + gap, arch_row_y, btn_w, 44};
    SDL_Rect arch_plus = {arch_value.x + arch_value.w + gap, arch_row_y, 44, 44};

    SDL_Color step_bg = {45, 45, 45, 255};
    SDL_Color step_bd = {140, 140, 140, 255};
    SDL_Color val_bg = {30, 30, 30, 255};

    SDL_SetRenderDrawColor(renderer, step_bg.r, step_bg.g, step_bg.b, step_bg.a);
    SDL_RenderFillRect(renderer, &arch_minus);
    SDL_RenderFillRect(renderer, &arch_plus);
    SDL_SetRenderDrawColor(renderer, val_bg.r, val_bg.g, val_bg.b, val_bg.a);
    SDL_RenderFillRect(renderer, &arch_value);
    SDL_SetRenderDrawColor(renderer, step_bd.r, step_bd.g, step_bd.b, step_bd.a);
    SDL_RenderDrawRect(renderer, &arch_minus);
    SDL_RenderDrawRect(renderer, &arch_plus);
    SDL_RenderDrawRect(renderer, &arch_value);

    if (s->opt_arch_minus.tex) {
      SDL_Rect tr = {arch_minus.x + (arch_minus.w - s->opt_arch_minus.w) / 2,
                     arch_minus.y + (arch_minus.h - s->opt_arch_minus.h) / 2, s->opt_arch_minus.w,
                     s->opt_arch_minus.h};
      SDL_RenderCopy(renderer, s->opt_arch_minus.tex, NULL, &tr);
    }
    if (s->opt_arch_plus.tex) {
      SDL_Rect tr = {arch_plus.x + (arch_plus.w - s->opt_arch_plus.w) / 2,
                     arch_plus.y + (arch_plus.h - s->opt_arch_plus.h) / 2, s->opt_arch_plus.w,
                     s->opt_arch_plus.h};
      SDL_RenderCopy(renderer, s->opt_arch_plus.tex, NULL, &tr);
    }
    if (s->val_archive.tex) {
      SDL_Rect tr = {arch_value.x + (arch_value.w - s->val_archive.w) / 2,
                     arch_value.y + (arch_value.h - s->val_archive.h) / 2, s->val_archive.w,
                     s->val_archive.h};
      SDL_RenderCopy(renderer, s->val_archive.tex, NULL, &tr);
    }

    y = arch_row_y + 44 + 16;
  }

  if (s->lbl_mode.tex) {
    SDL_Rect r = {x, y, s->lbl_mode.w, s->lbl_mode.h};
    SDL_RenderCopy(renderer, s->lbl_mode.tex, NULL, &r);
  }
  y += 34;
  SDL_Rect mode_tiles = {x, y, btn_w, 44};
  SDL_Rect mode_matrix = {x + btn_w + gap, y, btn_w, 44};
  SDL_Color bg_tiles = (s->pending_view_mode == VIEW_MODE_TILES) ? (SDL_Color){40, 120, 40, 220}
                                                                  : (SDL_Color){45, 45, 45, 255};
  SDL_Color bg_matrix = (s->pending_view_mode == VIEW_MODE_MATRIX) ? (SDL_Color){40, 120, 40, 220}
                                                                    : (SDL_Color){45, 45, 45, 255};
  SDL_Color bd_tiles = {140, 140, 140, 255};
  SDL_Color bd_matrix = {140, 140, 140, 255};
  if (s->pending_view_mode == VIEW_MODE_TILES) bd_tiles = (SDL_Color){200, 200, 200, 255};
  if (s->pending_view_mode == VIEW_MODE_MATRIX) bd_matrix = (SDL_Color){200, 200, 200, 255};

  SDL_SetRenderDrawColor(renderer, bg_tiles.r, bg_tiles.g, bg_tiles.b, bg_tiles.a);
  SDL_RenderFillRect(renderer, &mode_tiles);
  SDL_SetRenderDrawColor(renderer, bd_tiles.r, bd_tiles.g, bd_tiles.b, bd_tiles.a);
  SDL_RenderDrawRect(renderer, &mode_tiles);
  if (s->opt_mode_tiles.tex) {
    SDL_Rect tr = {mode_tiles.x + (mode_tiles.w - s->opt_mode_tiles.w) / 2,
                   mode_tiles.y + (mode_tiles.h - s->opt_mode_tiles.h) / 2, s->opt_mode_tiles.w,
                   s->opt_mode_tiles.h};
    SDL_RenderCopy(renderer, s->opt_mode_tiles.tex, NULL, &tr);
  }

  SDL_SetRenderDrawColor(renderer, bg_matrix.r, bg_matrix.g, bg_matrix.b, bg_matrix.a);
  SDL_RenderFillRect(renderer, &mode_matrix);
  SDL_SetRenderDrawColor(renderer, bd_matrix.r, bd_matrix.g, bd_matrix.b, bd_matrix.a);
  SDL_RenderDrawRect(renderer, &mode_matrix);
  if (s->opt_mode_matrix.tex) {
    SDL_Rect tr = {mode_matrix.x + (mode_matrix.w - s->opt_mode_matrix.w) / 2,
                   mode_matrix.y + (mode_matrix.h - s->opt_mode_matrix.h) / 2, s->opt_mode_matrix.w,
                   s->opt_mode_matrix.h};
    SDL_RenderCopy(renderer, s->opt_mode_matrix.tex, NULL, &tr);
  }

  y = mode_tiles.y + mode_tiles.h + 24;
  if (s->lbl_preview_mode.tex) {
    SDL_Rect r = {x, y, s->lbl_preview_mode.w, s->lbl_preview_mode.h};
    SDL_RenderCopy(renderer, s->lbl_preview_mode.tex, NULL, &r);
  }
  int preview_label_y = y;
  const int stream_switch_reserved =
      (s->pending_preview_mode == PREVIEW_MODE_STREAM) ? (44 + 18 + 34 + 44) : 0;
  int pv_max_y = btn_save.y - btn_h - 14 - stream_switch_reserved;
  int help_y = preview_label_y + 34;
  int pv_y = help_y;
  int gap_lines = 8;
  int gap_before_buttons = 10;
  settings_preview_compute_layout(s, preview_label_y, pv_max_y, &help_y, &pv_y, &gap_lines,
                                 &gap_before_buttons);

  y = help_y;
  if (s->lbl_preview_help1.tex) {
    SDL_Rect r = {x, y, s->lbl_preview_help1.w, s->lbl_preview_help1.h};
    SDL_RenderCopy(renderer, s->lbl_preview_help1.tex, NULL, &r);
    y += s->lbl_preview_help1.h;
    if (s->lbl_preview_help2.tex) y += gap_lines;
  }
  if (s->lbl_preview_help2.tex) {
    SDL_Rect r = {x, y, s->lbl_preview_help2.w, s->lbl_preview_help2.h};
    SDL_RenderCopy(renderer, s->lbl_preview_help2.tex, NULL, &r);
    y += s->lbl_preview_help2.h;
  }

  SDL_Rect pv_static = {x, pv_y, btn_w, 44};
  SDL_Rect pv_warm = {x + btn_w + gap, pv_y, btn_w, 44};
  SDL_Rect pv_stream = {x + 2 * (btn_w + gap), pv_y, btn_w, 44};

  SDL_Color bg_static = (s->pending_preview_mode == PREVIEW_MODE_STATIC) ? (SDL_Color){40, 120, 40, 220}
                                                                         : (SDL_Color){45, 45, 45, 255};
  SDL_Color bg_warm = (s->pending_preview_mode == PREVIEW_MODE_WARM) ? (SDL_Color){40, 120, 40, 220}
                                                                     : (SDL_Color){45, 45, 45, 255};
  SDL_Color bg_stream = (s->pending_preview_mode == PREVIEW_MODE_STREAM) ? (SDL_Color){40, 120, 40, 220}
                                                                         : (SDL_Color){45, 45, 45, 255};
  SDL_Color border_static = {140, 140, 140, 255};
  SDL_Color border_warm = {140, 140, 140, 255};
  SDL_Color border_stream = {140, 140, 140, 255};
  if (s->pending_preview_mode == PREVIEW_MODE_STATIC) border_static = (SDL_Color){200, 200, 200, 255};
  if (s->pending_preview_mode == PREVIEW_MODE_WARM) border_warm = (SDL_Color){200, 200, 200, 255};
  if (s->pending_preview_mode == PREVIEW_MODE_STREAM) border_stream = (SDL_Color){200, 200, 200, 255};

  SDL_SetRenderDrawColor(renderer, bg_static.r, bg_static.g, bg_static.b, bg_static.a);
  SDL_RenderFillRect(renderer, &pv_static);
  SDL_SetRenderDrawColor(renderer, border_static.r, border_static.g, border_static.b, border_static.a);
  SDL_RenderDrawRect(renderer, &pv_static);
  if (s->opt_preview_static.tex) {
    SDL_Rect tr = {pv_static.x + (pv_static.w - s->opt_preview_static.w) / 2,
                   pv_static.y + (pv_static.h - s->opt_preview_static.h) / 2, s->opt_preview_static.w,
                   s->opt_preview_static.h};
    SDL_RenderCopy(renderer, s->opt_preview_static.tex, NULL, &tr);
  }

  SDL_SetRenderDrawColor(renderer, bg_warm.r, bg_warm.g, bg_warm.b, bg_warm.a);
  SDL_RenderFillRect(renderer, &pv_warm);
  SDL_SetRenderDrawColor(renderer, border_warm.r, border_warm.g, border_warm.b, border_warm.a);
  SDL_RenderDrawRect(renderer, &pv_warm);
  if (s->opt_preview_warm.tex) {
    SDL_Rect tr = {pv_warm.x + (pv_warm.w - s->opt_preview_warm.w) / 2,
                   pv_warm.y + (pv_warm.h - s->opt_preview_warm.h) / 2, s->opt_preview_warm.w,
                   s->opt_preview_warm.h};
    SDL_RenderCopy(renderer, s->opt_preview_warm.tex, NULL, &tr);
  }

  SDL_SetRenderDrawColor(renderer, bg_stream.r, bg_stream.g, bg_stream.b, bg_stream.a);
  SDL_RenderFillRect(renderer, &pv_stream);
  SDL_SetRenderDrawColor(renderer, border_stream.r, border_stream.g, border_stream.b, border_stream.a);
  SDL_RenderDrawRect(renderer, &pv_stream);
  if (s->opt_preview_stream.tex) {
    SDL_Rect tr = {pv_stream.x + (pv_stream.w - s->opt_preview_stream.w) / 2,
                   pv_stream.y + (pv_stream.h - s->opt_preview_stream.h) / 2, s->opt_preview_stream.w,
                   s->opt_preview_stream.h};
    SDL_RenderCopy(renderer, s->opt_preview_stream.tex, NULL, &tr);
  }

  if (s->pending_preview_mode == PREVIEW_MODE_STREAM) {
    int sy = pv_y + 44 + 18;
    if (s->lbl_stream_tiles_source.tex) {
      SDL_Rect r = {x, sy, s->lbl_stream_tiles_source.w, s->lbl_stream_tiles_source.h};
      SDL_RenderCopy(renderer, s->lbl_stream_tiles_source.tex, NULL, &r);
    }
    sy += 34;

    SDL_Rect src_preview = {x, sy, btn_w, 44};
    SDL_Rect src_main = {x + btn_w + gap, sy, btn_w, 44};

    SDL_Color bg_preview = (s->pending_tiles_use_preview_url) ? (SDL_Color){40, 120, 40, 220}
                                                              : (SDL_Color){45, 45, 45, 255};
    SDL_Color bg_main = (!s->pending_tiles_use_preview_url) ? (SDL_Color){40, 120, 40, 220}
                                                            : (SDL_Color){45, 45, 45, 255};
    SDL_Color bd_preview = (s->pending_tiles_use_preview_url) ? (SDL_Color){200, 200, 200, 255}
                                                              : (SDL_Color){140, 140, 140, 255};
    SDL_Color bd_main = (!s->pending_tiles_use_preview_url) ? (SDL_Color){200, 200, 200, 255}
                                                            : (SDL_Color){140, 140, 140, 255};

    SDL_SetRenderDrawColor(renderer, bg_preview.r, bg_preview.g, bg_preview.b, bg_preview.a);
    SDL_RenderFillRect(renderer, &src_preview);
    SDL_SetRenderDrawColor(renderer, bd_preview.r, bd_preview.g, bd_preview.b, bd_preview.a);
    SDL_RenderDrawRect(renderer, &src_preview);
    if (s->opt_stream_tiles_preview.tex) {
      SDL_Rect tr = {src_preview.x + (src_preview.w - s->opt_stream_tiles_preview.w) / 2,
                     src_preview.y + (src_preview.h - s->opt_stream_tiles_preview.h) / 2,
                     s->opt_stream_tiles_preview.w, s->opt_stream_tiles_preview.h};
      SDL_RenderCopy(renderer, s->opt_stream_tiles_preview.tex, NULL, &tr);
    }

    SDL_SetRenderDrawColor(renderer, bg_main.r, bg_main.g, bg_main.b, bg_main.a);
    SDL_RenderFillRect(renderer, &src_main);
    SDL_SetRenderDrawColor(renderer, bd_main.r, bd_main.g, bd_main.b, bd_main.a);
    SDL_RenderDrawRect(renderer, &src_main);
    if (s->opt_stream_tiles_main.tex) {
      SDL_Rect tr = {src_main.x + (src_main.w - s->opt_stream_tiles_main.w) / 2,
                     src_main.y + (src_main.h - s->opt_stream_tiles_main.h) / 2, s->opt_stream_tiles_main.w,
                     s->opt_stream_tiles_main.h};
      SDL_RenderCopy(renderer, s->opt_stream_tiles_main.tex, NULL, &tr);
    }
  }

  SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
  SDL_RenderFillRect(renderer, &btn_save);
  SDL_RenderFillRect(renderer, &btn_cancel);
  SDL_SetRenderDrawColor(renderer, 160, 160, 160, 255);
  SDL_RenderDrawRect(renderer, &btn_save);
  SDL_RenderDrawRect(renderer, &btn_cancel);

  if (s->btn_save.tex) {
    SDL_Rect tr = {btn_save.x + (btn_save.w - s->btn_save.w) / 2,
                   btn_save.y + (btn_save.h - s->btn_save.h) / 2, s->btn_save.w, s->btn_save.h};
    SDL_RenderCopy(renderer, s->btn_save.tex, NULL, &tr);
  }
  if (s->btn_cancel.tex) {
    SDL_Rect tr = {btn_cancel.x + (btn_cancel.w - s->btn_cancel.w) / 2,
                   btn_cancel.y + (btn_cancel.h - s->btn_cancel.h) / 2, s->btn_cancel.w,
                   s->btn_cancel.h};
    SDL_RenderCopy(renderer, s->btn_cancel.tex, NULL, &tr);
  }
}

int settings_save_to_ini(const char *settings_path, const SettingsOverlay *s) {
#ifdef _WIN32
  if (!settings_path || !*settings_path || !s) return -1;
  if (ini_set_string(settings_path, "App", "RenderScaleQuality", s->pending_quality) != 0) return -1;
  if (ini_set_string(settings_path, "App", "ViewMode", view_mode_to_string(s->pending_view_mode)) != 0) return -1;
  if (ini_set_string(settings_path, "App", "PreviewMode",
                     preview_mode_to_string(s->pending_preview_mode)) != 0) {
    return -1;
  }
  if (ini_set_int(settings_path, "App", "TilesUsePreviewURL", s->pending_tiles_use_preview_url ? 1 : 0) != 0) {
    return -1;
  }
  if (ini_set_int(settings_path, "Recording", "MaxArchiveGB", s->pending_archive_gb) != 0) {
    return -1;
  }
  if (ini_set_string(settings_path, "App", "Language",
                     language_to_string(s->pending_language)) != 0) {
    return -1;
  }
  // Commit the language globally so the rest of the UI picks up the new locale
  // on its next refresh; main.c also re-renders its cached localizable strings.
  i18n_set_language(s->pending_language);
  return 0;
#else
  (void)settings_path;
  (void)s;
  return -1;
#endif
}
