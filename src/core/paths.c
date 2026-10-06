#include "core/paths.h"

#include <stdio.h>
#include <string.h>

#include "core/util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int get_settings_ini_path(char *out, size_t out_size) {
  if (!out || out_size == 0) return -1;

  // Get the exe path via the wide API and convert to UTF-8 — the ANSI variant
  // mangles Cyrillic / accented usernames and folder names, which downstream
  // breaks SDL_RWFromFile, CreateDirectory, etc.
  WCHAR exe_path_w[MAX_PATH * 2] = {0};
  DWORD n = GetModuleFileNameW(NULL, exe_path_w, (DWORD)(sizeof(exe_path_w) / sizeof(WCHAR)));
  char exe_path[1024] = {0};
  if (n > 0 && n < (DWORD)(sizeof(exe_path_w) / sizeof(WCHAR))) {
    WideCharToMultiByte(CP_UTF8, 0, exe_path_w, -1, exe_path, (int)sizeof(exe_path), NULL, NULL);
  }
  n = (DWORD)strlen(exe_path);
  if (n > 0 && n < (DWORD)sizeof(exe_path)) {
    char dir[1024] = {0};
    dirname_from_path(exe_path, dir, sizeof(dir));
    if (dir[0]) {
      char cand1[1024] = {0};
      snprintf(cand1, sizeof(cand1), "%ssettings.ini", dir);
      if (file_exists(cand1)) {
        snprintf(out, out_size, "%s", cand1);
        return 0;
      }

      // Dev builds often run from a build directory (e.g. dist\build-min\Ultralight_IP_CAM_Viewer_Recorder.exe).
      // If a settings.ini exists in the parent directory, prefer it so settings/cache stay next to the shipped exe.
      char dir_no_trail[1024] = {0};
      snprintf(dir_no_trail, sizeof(dir_no_trail), "%s", dir);
      size_t len = strlen(dir_no_trail);
      while (len > 0 && (dir_no_trail[len - 1] == '\\' || dir_no_trail[len - 1] == '/')) {
        dir_no_trail[len - 1] = 0;
        len--;
      }
      char parent[1024] = {0};
      dirname_from_path(dir_no_trail, parent, sizeof(parent));
      if (parent[0]) {
        char cand2[1024] = {0};
        snprintf(cand2, sizeof(cand2), "%ssettings.ini", parent);
        if (file_exists(cand2)) {
          snprintf(out, out_size, "%s", cand2);
          return 0;
        }
      }

      snprintf(out, out_size, "%s", cand1);
      return 0;
    }
  }

  snprintf(out, out_size, "settings.ini");
  return 0;
}
#endif
