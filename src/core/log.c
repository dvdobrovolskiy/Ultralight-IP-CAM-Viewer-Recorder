#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void append_log_line_utf8(const char *path, const char *line) {
  if (!path || !*path || !line) return;
  SDL_RWops *rw = SDL_RWFromFile(path, "ab");
  if (!rw) return;
  SDL_RWwrite(rw, line, 1, strlen(line));
  SDL_RWwrite(rw, "\r\n", 1, 2);
  SDL_RWclose(rw);
}

void write_text_utf8(const char *path, const char *text) {
  if (!path || !*path || !text) return;
  SDL_RWops *rw = SDL_RWFromFile(path, "wb");
  if (!rw) return;
  SDL_RWwrite(rw, text, 1, strlen(text));
  SDL_RWclose(rw);
}

void append_text_utf8(const char *path, const char *text) {
  if (!path || !*path || !text) return;
  SDL_RWops *rw = SDL_RWFromFile(path, "ab");
  if (!rw) return;
  SDL_RWwrite(rw, text, 1, strlen(text));
  SDL_RWclose(rw);
}

#ifdef _WIN32
void append_startup_marker(const char *base_dir, const char *tag) {
  if (!tag) tag = "";
  char log_path[1024] = {0};
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  snprintf(log_path, sizeof(log_path), "%scache\\cache.log", base_use);
  char line[1024] = {0};
  snprintf(line, sizeof(line), "startup_marker: t=%lu tag=%s", (unsigned long)GetTickCount(), tag);
  append_log_line_utf8(log_path, line);
}

void startup_tracef(const char *base_dir, const char *fmt, ...) {
  const char *base_use = (base_dir && *base_dir) ? base_dir : ".\\";
  char path[1024] = {0};
  snprintf(path, sizeof(path), "%scache\\startup_trace.log", base_use);

  char msg[1600] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
  va_end(ap);

  char line[1800] = {0};
  snprintf(line, sizeof(line), "t=%lu tid=%lu %s", (unsigned long)GetTickCount(),
           (unsigned long)GetCurrentThreadId(), msg);
  append_log_line_utf8(path, line);
}
#endif
