#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <libavutil/error.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void die_sdl(const char *msg) {
  fprintf(stderr, "%s: %s\n", msg, SDL_GetError());
  exit(1);
}

void die_ffmpeg(const char *msg, int err) {
  char buf[256];
  av_strerror(err, buf, (int)sizeof(buf));
  fprintf(stderr, "%s: %s\n", msg, buf);
  exit(1);
}

void log_ffmpeg(const char *msg, int err) {
  char buf[256];
  av_strerror(err, buf, (int)sizeof(buf));
  fprintf(stderr, "%s: %s\n", msg, buf);
}

void dirname_from_path(const char *path, char *out, size_t out_sz) {
  if (!out || out_sz == 0) return;
  out[0] = 0;
  if (!path || !*path) return;

  size_t n = strlen(path);
  size_t cut = n;
  while (cut > 0) {
    char c = path[cut - 1];
    if (c == '\\' || c == '/') break;
    cut--;
  }
  if (cut == 0) return;
  if (cut >= out_sz) cut = out_sz - 1;
  memcpy(out, path, cut);
  out[cut] = 0;
}

int starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

SDL_bool file_exists(const char *path) {
  SDL_RWops *rw = SDL_RWFromFile(path, "rb");
  if (!rw) return SDL_FALSE;
  SDL_RWclose(rw);
  return SDL_TRUE;
}

char *str_dup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *p = (char *)SDL_malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

uint64_t fnv1a64(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ull;
  }
  return h;
}

int sanitize_folder_name_utf8(const char *in_utf8, char *out, size_t out_cap) {
  if (!out || out_cap == 0) return -1;
  out[0] = 0;
  if (!in_utf8) return 0;

  size_t j = 0;
  for (size_t i = 0; in_utf8[i] && j + 1 < out_cap; i++) {
    unsigned char c = (unsigned char)in_utf8[i];
    if (c < 0x20 || c == 0x7F) c = '_';
    if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
        c == '?' || c == '*') {
      c = '_';
    }
    out[j++] = (char)c;
  }
  out[j] = 0;

  while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.')) {
    out[j - 1] = 0;
    j--;
  }

  if (j == 0) return 0;
  return 0;
}

#ifdef _WIN32
void ensure_dir_recursive_win32(const char *path) {
  if (!path || !*path) return;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (wlen <= 0) return;
  WCHAR *wtmp = (WCHAR *)SDL_malloc((size_t)wlen * sizeof(WCHAR));
  if (!wtmp) return;
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wtmp, wlen) <= 0) {
    SDL_free(wtmp);
    return;
  }

  for (int i = 0; wtmp[i]; i++) {
    if (wtmp[i] == L'/') wtmp[i] = L'\\';
  }

  for (int i = 0; wtmp[i]; i++) {
    if (wtmp[i] == L'\\') {
      WCHAR c = wtmp[i];
      wtmp[i] = 0;
      if (wtmp[0] && !(wtmp[1] == L':' && wtmp[2] == 0)) {
        CreateDirectoryW(wtmp, NULL);
      }
      wtmp[i] = c;
    }
  }
  CreateDirectoryW(wtmp, NULL);
  SDL_free(wtmp);
}
#endif
