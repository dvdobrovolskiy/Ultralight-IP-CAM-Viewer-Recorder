#include "cam/archive.h"

#ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/log.h"

// One recorded segment found on disk: its full wide path, byte size, and
// last-write time (FILETIME packed into a uint64 for cheap ordering).
typedef struct FileEnt {
  wchar_t *wpath;
  uint64_t size;
  uint64_t mtime;
} FileEnt;

// Accumulator threaded through the recursive directory walk. In "size only"
// mode it just sums bytes; in collect mode it also records each *.ts file so
// the oldest can be deleted.
typedef struct WalkCtx {
  uint64_t total;
  FileEnt *ents;
  size_t count;
  size_t cap;
  int collect;
} WalkCtx;

static wchar_t *utf8_to_wide(const char *s) {
  if (!s) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
  if (!w) return NULL;
  if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

// Total capacity of the volume holding `wdir`, in bytes. GetDiskFreeSpaceExW
// wants an existing directory, so fall back to the path's drive root (the
// recordings folder may not have been created yet on a first run).
static uint64_t volume_total_bytes_w(const wchar_t *wdir) {
  if (!wdir || !*wdir) return 0;
  ULARGE_INTEGER avail_to_caller, total, total_free;
  avail_to_caller.QuadPart = total.QuadPart = total_free.QuadPart = 0;
  if (GetDiskFreeSpaceExW(wdir, &avail_to_caller, &total, &total_free)) {
    return (uint64_t)total.QuadPart;
  }

  // "X:\..." -> "X:\" ; UNC and other forms simply fail and are reported as 0.
  if (wdir[0] && wdir[1] == L':') {
    wchar_t root[4] = {wdir[0], L':', L'\\', 0};
    avail_to_caller.QuadPart = total.QuadPart = total_free.QuadPart = 0;
    if (GetDiskFreeSpaceExW(root, &avail_to_caller, &total, &total_free)) {
      return (uint64_t)total.QuadPart;
    }
  }
  return 0;
}

uint64_t archive_auto_limit_bytes(const char *dir) {
  wchar_t *w = utf8_to_wide(dir);
  if (!w) return 0;
  uint64_t total = volume_total_bytes_w(w);
  free(w);
  if (total == 0) return 0;
  // Percentage of the total first, to keep the intermediate well clear of
  // overflow on very large volumes.
  return (total / 100ull) * (uint64_t)ARCHIVE_AUTO_LIMIT_PERCENT;
}

static int is_ts_file(const wchar_t *name) {
  size_t n = wcslen(name);
  if (n < 3) return 0;
  return _wcsicmp(name + n - 3, L".ts") == 0;
}

// dir: wide path with no trailing backslash. Recurses; *.ts files are summed
// (and, in collect mode, appended to ctx->ents with ownership of the path).
static void walk_dir(const wchar_t *dir, WalkCtx *ctx, SDL_atomic_t *quit) {
  if (quit && SDL_AtomicGet(quit)) return;

  size_t dl = wcslen(dir);
  wchar_t *pat = (wchar_t *)malloc((dl + 3) * sizeof(wchar_t));
  if (!pat) return;
  swprintf(pat, dl + 3, L"%ls\\*", dir);

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pat, &fd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) return;

  do {
    if (quit && SDL_AtomicGet(quit)) break;
    const wchar_t *nm = fd.cFileName;
    if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;

    size_t cl = dl + 1 + wcslen(nm);
    wchar_t *child = (wchar_t *)malloc((cl + 1) * sizeof(wchar_t));
    if (!child) continue;
    swprintf(child, cl + 1, L"%ls\\%ls", dir, nm);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      walk_dir(child, ctx, quit);
      free(child);
    } else if (is_ts_file(nm)) {
      uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
      ctx->total += sz;
      if (ctx->collect) {
        if (ctx->count == ctx->cap) {
          size_t ncap = ctx->cap ? ctx->cap * 2 : 256;
          FileEnt *ne = (FileEnt *)realloc(ctx->ents, ncap * sizeof(FileEnt));
          if (!ne) {
            free(child);
            continue;
          }
          ctx->ents = ne;
          ctx->cap = ncap;
        }
        ctx->ents[ctx->count].wpath = child;
        ctx->ents[ctx->count].size = sz;
        ctx->ents[ctx->count].mtime =
            ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | (uint64_t)fd.ftLastWriteTime.dwLowDateTime;
        ctx->count++;
        child = NULL; // ownership transferred to the entry
      }
      if (child) free(child);
    } else {
      free(child);
    }
  } while (FindNextFileW(h, &fd));

  FindClose(h);
}

static int cmp_ent_oldest_first(const void *a, const void *b) {
  const FileEnt *x = (const FileEnt *)a;
  const FileEnt *y = (const FileEnt *)b;
  if (x->mtime < y->mtime) return -1;
  if (x->mtime > y->mtime) return 1;
  return 0;
}

// Bottom-up removal of directories left empty by trimming. RemoveDirectoryW
// only succeeds on empty dirs, so non-empty day/camera folders are left intact.
// The root itself is never removed.
static void prune_empty_dirs(const wchar_t *dir, SDL_atomic_t *quit) {
  if (quit && SDL_AtomicGet(quit)) return;

  size_t dl = wcslen(dir);
  wchar_t *pat = (wchar_t *)malloc((dl + 3) * sizeof(wchar_t));
  if (!pat) return;
  swprintf(pat, dl + 3, L"%ls\\*", dir);

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pat, &fd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) return;

  do {
    const wchar_t *nm = fd.cFileName;
    if (nm[0] == L'.' && (nm[1] == 0 || (nm[1] == L'.' && nm[2] == 0))) continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;

    size_t cl = dl + 1 + wcslen(nm);
    wchar_t *child = (wchar_t *)malloc((cl + 1) * sizeof(wchar_t));
    if (!child) continue;
    swprintf(child, cl + 1, L"%ls\\%ls", dir, nm);
    prune_empty_dirs(child, quit);
    RemoveDirectoryW(child); // no-op if still non-empty
    free(child);
  } while (FindNextFileW(h, &fd));

  FindClose(h);
}

// Returns the number of bytes deleted (0 if already under the limit).
static uint64_t archive_trim_once(const char *recordings_dir_utf8, uint64_t limit_bytes,
                                  SDL_atomic_t *quit) {
  wchar_t *root = utf8_to_wide(recordings_dir_utf8);
  if (!root) return 0;

  // Pass 1: cheap size sum. Most cycles end here with no allocation.
  WalkCtx sz = {0};
  walk_dir(root, &sz, quit);
  if (sz.total <= limit_bytes || (quit && SDL_AtomicGet(quit))) {
    free(root);
    return 0;
  }

  // Pass 2: collect the segment list so we can delete the oldest first.
  WalkCtx col = {0};
  col.collect = 1;
  walk_dir(root, &col, quit);
  free(root);

  uint64_t freed = 0;
  if (col.count > 0) {
    qsort(col.ents, col.count, sizeof(FileEnt), cmp_ent_oldest_first);

    // Trim a little below the limit (~1.5%) so we don't re-trim on every cycle
    // while hovering right at the cap.
    uint64_t target = limit_bytes - (limit_bytes / 64);
    uint64_t total = col.total;
    for (size_t i = 0; i < col.count && total > target; i++) {
      if (quit && SDL_AtomicGet(quit)) break;
      if (DeleteFileW(col.ents[i].wpath)) {
        total -= col.ents[i].size;
        freed += col.ents[i].size;
      }
    }
  }

  for (size_t i = 0; i < col.count; i++) free(col.ents[i].wpath);
  free(col.ents);

  if (freed > 0) {
    wchar_t *root2 = utf8_to_wide(recordings_dir_utf8);
    if (root2) {
      prune_empty_dirs(root2, quit);
      free(root2);
    }
  }
  return freed;
}

int archive_trim_thread_fn(void *userdata) {
  ArchiveTrimArgs *a = (ArchiveTrimArgs *)userdata;
  if (!a || !a->quit || !a->limit_gb) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

  const char *base_use = (a->base_dir[0] ? a->base_dir : ".\\");
  char log_path[1100] = {0};
  snprintf(log_path, sizeof(log_path), "%scache\\archive.log", base_use);

  // Wake every second to stay responsive to quit, but only actually scan every
  // few minutes. First scan happens shortly after startup so an already-oversize
  // archive gets pulled back down promptly.
  const Uint32 CHECK_INTERVAL_MS = 5u * 60u * 1000u;
  Uint32 next_check = SDL_GetTicks() + 20000u;

  for (;;) {
    if (SDL_AtomicGet(a->quit)) break;
    SDL_Delay(1000);
    if (SDL_AtomicGet(a->quit)) break;

    Uint32 now = SDL_GetTicks();
    if ((Sint32)(now - next_check) < 0) continue;
    next_check = now + CHECK_INTERVAL_MS;

    int gb = SDL_AtomicGet(a->limit_gb);
    uint64_t limit_bytes = 0;
    int auto_limit = 0;
    if (gb > 0) {
      limit_bytes = (uint64_t)gb * 1024ull * 1024ull * 1024ull;
    } else {
      // "Off" means "no limit chosen", not "let it fill the disk". Recomputed
      // every cycle so a resized or replaced drive is picked up automatically.
      limit_bytes = archive_auto_limit_bytes(a->recordings_dir);
      if (limit_bytes == 0) continue;  // volume unknown: never guess, never trim
      auto_limit = 1;
    }

    uint64_t freed = archive_trim_once(a->recordings_dir, limit_bytes, a->quit);
    if (freed > 0) {
      char line[256];
      double limit_gb_eff = (double)limit_bytes / (1024.0 * 1024.0 * 1024.0);
      if (auto_limit) {
        snprintf(line, sizeof(line), "t=%lu trimmed=%.1f MB limit=%.1f GB (auto: %d%% of drive)",
                 (unsigned long)GetTickCount(), (double)freed / (1024.0 * 1024.0), limit_gb_eff,
                 ARCHIVE_AUTO_LIMIT_PERCENT);
      } else {
        snprintf(line, sizeof(line), "t=%lu trimmed=%.1f MB limit=%.1f GB",
                 (unsigned long)GetTickCount(), (double)freed / (1024.0 * 1024.0), limit_gb_eff);
      }
      append_log_line_utf8(log_path, line);
    }
  }
  return 0;
}

#else
int archive_trim_thread_fn(void *userdata) {
  (void)userdata;
  return 0;
}

uint64_t archive_auto_limit_bytes(const char *dir) {
  (void)dir;
  return 0;
}
#endif
