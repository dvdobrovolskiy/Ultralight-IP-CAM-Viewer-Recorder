#pragma once

// Two-locale i18n: English (default) and Russian. Strings are referenced by an
// enum and looked up in fixed tables. The current language is process-global,
// loaded once at startup from settings.ini, and changed via the F1 overlay.
//
// tr(id)            -> the string in the current language
// tr_in(lang, id)   -> the string in an explicit language (used by the F1
//                      settings overlay to preview the pending selection
//                      without committing it).

typedef enum LanguageCode {
  LANG_EN = 0,
  LANG_RU = 1,
  LANG__COUNT
} LanguageCode;

typedef enum StringId {
  STR_TIP_BAR,

  // F1 Settings
  STR_SETTINGS_TITLE,
  STR_RENDER_QUALITY,
  STR_VIEW_MODE,
  STR_TILES,
  STR_MATRIX,
  STR_PREVIEW_MODE,
  STR_PREVIEW_HELP1,
  STR_PREVIEW_HELP2,
  STR_STATIC,
  STR_WARM,
  STR_STREAM,
  STR_STREAM_TILES_SOURCE,
  STR_OPT_PREVIEW_URL,
  STR_OPT_MAIN_URL,
  STR_RECORDING_LIMIT,
  STR_OFF,
  STR_AUTO_DRIVE_LIMIT,
  STR_LANGUAGE,
  STR_LANG_ENGLISH,
  STR_LANG_RUSSIAN,
  STR_SAVE,
  STR_CANCEL,

  // F2 Camera Manager
  STR_CAMMGR_TITLE,
  STR_ADD,
  STR_EDIT,
  STR_DELETE,
  STR_CLOSE,
  STR_TYPE,
  STR_TITLE,
  STR_URL,
  STR_PREVIEW_URL_LABEL,
  STR_ONVIF_DEVICE_URL,
  STR_ONVIF_PROBING,
  STR_ONVIF_OK,
  STR_ONVIF_FAILED,
  STR_PTZ,
  STR_PTZ_URL,
  STR_PTZ_LOGIN,
  STR_PTZ_PASSWORD,
  STR_ON,
  STR_ENABLED,
  STR_DISABLED,
  STR_PTZ_HINT,
  STR_PTZ_ERROR,

  // F10 Plugins
  STR_PLUGINS_TITLE,
  STR_RESCAN,
  STR_TOGGLE,

  // Intro
  STR_WELCOME,
  STR_INTRO_F1,
  STR_INTRO_F2,
  STR_INTRO_F3,
  STR_INTRO_F5,
  STR_INTRO_F9,
  STR_INTRO_F10,
  STR_INTRO_F11,
  STR_INTRO_PLUS_MINUS,
  STR_INTRO_PTZ,
  STR_INTRO_TIP,
  STR_SHOW_AT_STARTUP,

  // Status / errors
  STR_CAMERA_UNAVAILABLE,
  STR_NO_CAMERAS,
  STR_LOADING_MAIN_STREAM,

  // Tray menu
  STR_TRAY_SHOW,
  STR_TRAY_EXIT,

  STR__COUNT
} StringId;

const char *tr(StringId id);
const char *tr_in(LanguageCode lang, StringId id);

LanguageCode i18n_get_language(void);
// Returns 1 if language changed, 0 if no-op. Bumping the version lets cached
// text textures detect they need a rebuild.
int i18n_set_language(LanguageCode lang);

// Monotonically-increasing counter, incremented every time the language is
// changed. UI code that caches text textures stores the version at render time
// and rebuilds when it differs from the current one.
int i18n_version(void);

LanguageCode language_from_string(const char *s);
const char *language_to_string(LanguageCode lang);
