#pragma once

#include "app/camera.h"

// Background settings persistence.
//
// Writing settings.ini is surprisingly expensive: save_cameras_to_ini() issues
// ~6*N+32 individual WritePrivateProfileString calls, each of which flushes to
// disk. Doing that on the UI thread — which happens on every camera switch —
// stalls rendering. PersistQueue moves the actual disk I/O onto a dedicated
// worker thread. The UI thread only builds a cheap in-memory snapshot and hands
// it over; the worker serializes the writes so settings.ini always has a single
// writer.
//
// Requests are coalesced (latest-wins per target) so spamming a control — e.g.
// holding the volume key — collapses to one write instead of a backlog.
typedef struct PersistQueue PersistQueue;

// Start the worker thread. Returns NULL on failure (callers should treat a NULL
// queue as "persistence unavailable" and fall back to a direct synchronous write
// if they must). Safe to pass the returned handle to all functions below.
PersistQueue *persist_start(void);

// Enqueue a full camera-list save (deep-copies the persisted fields, so the
// caller may mutate/free `cams` immediately after this returns).
void persist_save_cameras(PersistQueue *q, const char *path, const CameraConfig *cams, int count,
                          int active_camera);

// Enqueue a single integer key write (e.g. App/AudioVolumePct).
void persist_set_int(PersistQueue *q, const char *path, const char *section, const char *key, int v);

// Drain all pending writes synchronously, stop the worker, and free the queue.
// Call once at shutdown before any final synchronous save and before the camera
// strings the snapshots referenced are freed.
void persist_flush_and_stop(PersistQueue *q);
