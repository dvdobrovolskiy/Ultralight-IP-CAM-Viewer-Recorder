#pragma once
// Single-instance guard (Windows; no-ops elsewhere).
//   - the first process to start owns a named mutex for its lifetime
//   - a second copy sees the mutex, asks the running one to show its window
//     (it may be hidden in the tray), and exits without touching settings/cache
//
// The "show yourself" request is a registered window message broadcast to every
// top-level window; the running instance handles it in the tray subclass
// (app/tray.c) by restoring and raising its main window.

// Name of the registered window message used to ask the running instance to
// show its window. Shared with app/tray.c.
#define SINGLE_INSTANCE_SHOW_MSG_NAME L"UltralightIPCamViewerRecorder.ShowWindow"

// Returns 1 if this process is the only instance (and now holds the guard),
// 0 if another instance is already running (its window has been asked to show).
// Call first thing in main(), before any file/settings work.
int single_instance_acquire(void);

// Release the guard. Safe to call even if acquire returned 0.
void single_instance_release(void);
