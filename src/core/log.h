#pragma once

void append_log_line_utf8(const char *path, const char *line);
void write_text_utf8(const char *path, const char *text);
void append_text_utf8(const char *path, const char *text);

#ifdef _WIN32
void append_startup_marker(const char *base_dir, const char *tag);
void startup_tracef(const char *base_dir, const char *fmt, ...);
#endif
