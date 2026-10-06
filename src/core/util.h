#pragma once

#include <stddef.h>
#include <stdint.h>

#include <SDL.h>

void die_sdl(const char *msg);
void die_ffmpeg(const char *msg, int err);
void log_ffmpeg(const char *msg, int err);

void dirname_from_path(const char *path, char *out, size_t out_sz);

int starts_with(const char *s, const char *prefix);

SDL_bool file_exists(const char *path);
char *str_dup(const char *s);

uint64_t fnv1a64(const void *data, size_t n);

int sanitize_folder_name_utf8(const char *in_utf8, char *out, size_t out_cap);

#ifdef _WIN32
void ensure_dir_recursive_win32(const char *path);
#endif
