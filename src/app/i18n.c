#include "app/i18n.h"

#include <stddef.h>

#include <SDL.h>

static LanguageCode g_lang = LANG_EN;
static int g_version = 1;

static const char *const en[STR__COUNT] = {
  [STR_TIP_BAR]              = "F1 Help|F2 Settings|F3 Manage cameras|F5 Recording|F9 Time machine|F10 Plugins|F11 Full screen",

  [STR_SETTINGS_TITLE]       = "Settings (F2/Esc close)",
  [STR_RENDER_QUALITY]       = "Render scale quality:",
  [STR_VIEW_MODE]            = "View mode:",
  [STR_TILES]                = "Tiles",
  [STR_MATRIX]               = "Matrix",
  [STR_PREVIEW_MODE]         = "Preview mode:",
  [STR_PREVIEW_HELP1]        = "Static (image in tile), Warm (keep cameras warm),",
  [STR_PREVIEW_HELP2]        = "Stream (stream in tiles; choose Preview URL or Main URL below)",
  [STR_STATIC]               = "Static",
  [STR_WARM]                 = "Warm",
  [STR_STREAM]               = "Stream",
  [STR_STREAM_TILES_SOURCE]  = "Stream tiles source:",
  [STR_OPT_PREVIEW_URL]      = "Preview URL",
  [STR_OPT_MAIN_URL]         = "Main URL",
  [STR_RECORDING_LIMIT]      = "Recording storage limit (GB):",
  [STR_OFF]                  = "Off",
  // Kept short: it renders inside the 200px value box next to the -/+ stepper.
  [STR_AUTO_DRIVE_LIMIT]     = "auto 90%",
  [STR_LANGUAGE]             = "Language:",
  [STR_LANG_ENGLISH]         = "English",
  [STR_LANG_RUSSIAN]         = "Russian",
  [STR_SAVE]                 = "Save",
  [STR_CANCEL]               = "Cancel",

  [STR_CAMMGR_TITLE]         = "Manage Cameras (Click fields, Ctrl+V paste, F3/Esc close)",
  [STR_ADD]                  = "Add",
  [STR_EDIT]                 = "Edit",
  [STR_DELETE]               = "Delete",
  [STR_CLOSE]                = "Close",
  [STR_TYPE]                 = "Type",
  [STR_TITLE]                = "Title",
  [STR_URL]                  = "URL",
  [STR_PREVIEW_URL_LABEL]    = "Preview URL",
  [STR_ONVIF_DEVICE_URL]     = "ONVIF device URL (http://user:pass@ip[:port])",
  [STR_ONVIF_PROBING]        = "ONVIF: querying device...",
  [STR_ONVIF_OK]             = "ONVIF: streams configured",
  [STR_ONVIF_FAILED]         = "ONVIF error: ",
  [STR_PTZ]                  = "PTZ (Space toggles)",
  [STR_PTZ_URL]              = "PTZ URL (http://ip:port/onvif/PTZ)",
  [STR_PTZ_LOGIN]            = "PTZ login",
  [STR_PTZ_PASSWORD]         = "PTZ password",
  [STR_ON]                   = "On",
  [STR_ENABLED]              = "Enabled (Space toggles)",
  [STR_DISABLED]             = "disabled",
  [STR_PTZ_HINT]             = "Arrows pan/tilt, PgUp/PgDn zoom",
  [STR_PTZ_ERROR]            = "PTZ error: ",

  [STR_PLUGINS_TITLE]        = "Plugins (F10/Esc close, Enter toggle, R rescan)",
  [STR_RESCAN]               = "Rescan",
  [STR_TOGGLE]               = "Toggle",

  [STR_WELCOME]              = "Welcome",
  [STR_INTRO_F1]             = "F1  - Show this help",
  [STR_INTRO_F2]             = "F2  - Settings",
  [STR_INTRO_F3]             = "F3  - Manage cameras",
  [STR_INTRO_F5]             = "F5  - Start / Stop recording",
  [STR_INTRO_F9]             = "F9  - Time machine (archive playback)",
  [STR_INTRO_F10]            = "F10 - Plugins",
  [STR_INTRO_F11]            = "F11 - Full screen",
  [STR_INTRO_PLUS_MINUS]     = "+/- - Volume up / down",
  [STR_INTRO_PTZ]            = "Arrows / PgUp / PgDn - PTZ (cameras with PTZ enabled)",
  [STR_INTRO_TIP]            = "Press Esc/Enter or click anywhere to close.",
  [STR_SHOW_AT_STARTUP]      = "Show at startup",

  [STR_CAMERA_UNAVAILABLE]   = "Camera unavailable",
  [STR_NO_CAMERAS]           = "No cameras configured (press F3 to add)",
  [STR_LOADING_MAIN_STREAM]  = "Loading main stream",
  [STR_TRAY_SHOW]            = "Show",
  [STR_TRAY_EXIT]            = "Exit",
};

static const char *const ru[STR__COUNT] = {
  [STR_TIP_BAR]              = "F1 Справка|F2 Настройки|F3 Камеры|F5 Запись|F9 Машина времени|F10 Плагины|F11 Полный экран",

  [STR_SETTINGS_TITLE]       = "Настройки (F2/Esc закрыть)",
  [STR_RENDER_QUALITY]       = "Качество масштабирования:",
  [STR_VIEW_MODE]            = "Режим отображения:",
  [STR_TILES]                = "Плитки",
  [STR_MATRIX]               = "Сетка",
  [STR_PREVIEW_MODE]         = "Режим превью:",
  [STR_PREVIEW_HELP1]        = "Static (картинка в плитке), Warm (поддерживать поток),",
  [STR_PREVIEW_HELP2]        = "Stream (видео в плитках; выберите источник ниже)",
  [STR_STATIC]               = "Статичный",
  [STR_WARM]                 = "Поддержка",
  [STR_STREAM]               = "Поток",
  [STR_STREAM_TILES_SOURCE]  = "Источник в плитках:",
  [STR_OPT_PREVIEW_URL]      = "Превью URL",
  [STR_OPT_MAIN_URL]         = "Основной URL",
  [STR_RECORDING_LIMIT]      = "Лимит хранения записей (ГБ):",
  [STR_OFF]                  = "Выкл",
  [STR_AUTO_DRIVE_LIMIT]     = "авто 90%",
  [STR_LANGUAGE]             = "Язык:",
  [STR_LANG_ENGLISH]         = "English",
  [STR_LANG_RUSSIAN]         = "Русский",
  [STR_SAVE]                 = "Сохранить",
  [STR_CANCEL]               = "Отмена",

  [STR_CAMMGR_TITLE]         = "Управление камерами (клик в поле, Ctrl+V вставить, F3/Esc закрыть)",
  [STR_ADD]                  = "Добавить",
  [STR_EDIT]                 = "Изменить",
  [STR_DELETE]               = "Удалить",
  [STR_CLOSE]                = "Закрыть",
  [STR_TYPE]                 = "Тип",
  [STR_TITLE]                = "Название",
  [STR_URL]                  = "URL",
  [STR_PREVIEW_URL_LABEL]    = "Превью URL",
  [STR_ONVIF_DEVICE_URL]     = "ONVIF адрес устройства (http://логин:пароль@ip[:порт])",
  [STR_ONVIF_PROBING]        = "ONVIF: опрос устройства...",
  [STR_ONVIF_OK]             = "ONVIF: потоки настроены",
  [STR_ONVIF_FAILED]         = "Ошибка ONVIF: ",
  [STR_PTZ]                  = "PTZ (Пробел переключает)",
  [STR_PTZ_URL]              = "PTZ URL (http://ip:порт/onvif/PTZ)",
  [STR_PTZ_LOGIN]            = "PTZ логин",
  [STR_PTZ_PASSWORD]         = "PTZ пароль",
  [STR_ON]                   = "Вкл",
  [STR_ENABLED]              = "Включена (Пробел переключает)",
  [STR_DISABLED]             = "выключена",
  [STR_PTZ_HINT]             = "Стрелки — поворот, PgUp/PgDn — зум",
  [STR_PTZ_ERROR]            = "Ошибка PTZ: ",

  [STR_PLUGINS_TITLE]        = "Плагины (F10/Esc закрыть, Enter переключить, R обновить)",
  [STR_RESCAN]               = "Обновить",
  [STR_TOGGLE]               = "Переключить",

  [STR_WELCOME]              = "Добро пожаловать",
  [STR_INTRO_F1]             = "F1  - Показать справку",
  [STR_INTRO_F2]             = "F2  - Настройки",
  [STR_INTRO_F3]             = "F3  - Управление камерами",
  [STR_INTRO_F5]             = "F5  - Запись",
  [STR_INTRO_F9]             = "F9  - Машина времени (просмотр архива)",
  [STR_INTRO_F10]            = "F10 - Плагины",
  [STR_INTRO_F11]            = "F11 - Полный экран",
  [STR_INTRO_PLUS_MINUS]     = "+/- - Громкость +/-",
  [STR_INTRO_PTZ]            = "Стрелки / PgUp / PgDn - PTZ (если включён у камеры)",
  [STR_INTRO_TIP]            = "Esc / Enter или клик чтобы закрыть.",
  [STR_SHOW_AT_STARTUP]      = "Показывать при запуске",

  [STR_CAMERA_UNAVAILABLE]   = "Камера недоступна",
  [STR_NO_CAMERAS]           = "Камеры не настроены (F3 чтобы добавить)",
  [STR_LOADING_MAIN_STREAM]  = "Загрузка основного потока",
  [STR_TRAY_SHOW]            = "Показать",
  [STR_TRAY_EXIT]            = "Выход",
};

static const char *const *tables[LANG__COUNT] = { en, ru };

const char *tr(StringId id) { return tr_in(g_lang, id); }

const char *tr_in(LanguageCode lang, StringId id) {
  if (id < 0 || id >= STR__COUNT) return "";
  if (lang < 0 || lang >= LANG__COUNT) lang = LANG_EN;
  const char *s = tables[lang][id];
  if (!s) s = tables[LANG_EN][id];   // Fall back to English on missing translation.
  return s ? s : "";
}

LanguageCode i18n_get_language(void) { return g_lang; }

int i18n_set_language(LanguageCode lang) {
  if (lang < 0 || lang >= LANG__COUNT) lang = LANG_EN;
  if (lang == g_lang) return 0;
  g_lang = lang;
  g_version++;
  return 1;
}

int i18n_version(void) { return g_version; }

LanguageCode language_from_string(const char *s) {
  if (!s || !*s) return LANG_EN;
  if (SDL_strcasecmp(s, "ru") == 0 || SDL_strcasecmp(s, "russian") == 0) return LANG_RU;
  return LANG_EN;
}

const char *language_to_string(LanguageCode lang) {
  switch (lang) {
    case LANG_RU: return "ru";
    case LANG_EN:
    default:      return "en";
  }
}
