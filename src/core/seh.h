#pragma once

// Per-thread SEH crash containment.
//
// Phase 2 design:
//   - Install one global vectored exception handler at process startup. It catches
//     fatal SEH faults (access violation, illegal instruction, stack overflow,
//     divide-by-zero, privileged instruction) on ANY thread, logs the EXCEPTION_RECORD
//     details to <base_dir>cache/crash.log, marks the thread's TLS-registered "alive"
//     atomic to 0 (so a supervisor can detect it), and calls ExitThread() so only that
//     thread dies — the process and other threads keep running.
//   - Use seh_create_thread() instead of SDL_CreateThread() everywhere. The wrapper
//     sets *alive_out = 1 at entry, registers it in TLS so the vectored handler can
//     clear it on crash, and clears it itself on normal return.
//   - A supervisor (per-camera, in warm/tile_stream spawn-step) polls *alive on each
//     UI tick and respawns dead threads with exponential backoff.
//
// Non-Win32 builds compile a stub: handler install is a no-op, seh_create_thread is a
// thin pass-through to SDL_CreateThread that still tracks alive_out.

#include <SDL.h>

void seh_install_global_handler(const char *base_dir);

SDL_Thread *seh_create_thread(SDL_ThreadFunction fn, const char *name, void *arg,
                              SDL_atomic_t *alive_out);
