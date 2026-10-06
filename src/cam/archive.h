#pragma once

#include <stdint.h>

#include <SDL.h>

// Recording archive size manager.
//
// A low-priority background thread that casually enforces a total size cap on
// the recordings directory. It does NOT run often or quickly: it wakes every
// few minutes, sums the size of all *.ts segment files, and if the total is
// over the configured limit it deletes the OLDEST segments (by last-write time)
// until the archive is back under the limit. Because checks are infrequent and
// recording continues between them, the archive may briefly exceed the limit by
// roughly one segment per active camera — that is acceptable by design; the goal
// is only to keep the disk from filling without limit, not to be exact.
//
// The limit is read live from `limit_gb` each cycle, so changing it in Settings
// takes effect on the next check without a restart.
//
// `limit_gb == 0` ("Off" in Settings) does NOT mean unlimited: it falls back to
// ARCHIVE_AUTO_LIMIT_PERCENT of the *total capacity* of the volume holding the
// recordings (not of its free space). Recordings would otherwise grow until the
// disk was full. The fallback is recomputed on every check, so replacing or
// resizing the drive is picked up without a restart.

typedef struct ArchiveTrimArgs {
  char recordings_dir[1024]; // absolute path to the "recordings" folder to police
  char base_dir[1024];       // app base dir (for cache\archive.log)
  SDL_atomic_t *limit_gb;    // live size cap in GB; 0 disables trimming
  SDL_atomic_t *quit;        // set to 1 to stop the thread
} ArchiveTrimArgs;

// seh_create_thread entry point. Windows-only; a no-op stub on other platforms.
int archive_trim_thread_fn(void *userdata);

#define ARCHIVE_AUTO_LIMIT_PERCENT 90

// Size cap to apply when the user's limit is "Off": ARCHIVE_AUTO_LIMIT_PERCENT
// of the total capacity of the volume containing `dir`. `dir` need not exist —
// the drive root is used as a fallback. Returns 0 if the volume can't be
// queried (caller should then skip trimming rather than guess).
uint64_t archive_auto_limit_bytes(const char *dir);
