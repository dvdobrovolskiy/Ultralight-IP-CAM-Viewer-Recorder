#pragma once

#include <stddef.h>

// ONVIF PTZ control (ContinuousMove / Stop) for a single camera at a time.
//
// All network I/O happens on a dedicated worker thread: the UI thread only ever
// drops a command into a one-slot mailbox (latest wins), so a slow or dead
// camera can never stall rendering. A watchdog stops the camera automatically
// if a release is somehow missed, so the head cannot keep panning forever.

typedef struct PtzTarget {
  // Sized to match the camera-manager URL field, so a long endpoint is never
  // silently truncated into a different address.
  char url[2048];      // PTZ service endpoint; a missing path defaults to /onvif/PTZ
  char user[128];
  char pass[128];
  char profile[128];   // media profile token; discovered from the device when empty
} PtzTarget;

typedef struct PtzController PtzController;

PtzController *ptz_start(void);
void ptz_stop_and_free(PtzController *p);

// Begin moving. vx = pan (+right), vy = tilt (+up), vz = zoom (+in), each in
// [-1, 1]. Non-blocking; supersedes any pending command.
void ptz_move(PtzController *p, const PtzTarget *t, float vx, float vy, float vz);
// Stop all motion. Non-blocking.
void ptz_release(PtzController *p);

// 1 while a command is in flight (for a small activity indicator).
int ptz_is_busy(PtzController *p);
// Copies the most recent error ("" when none). Returns 1 if non-empty.
int ptz_last_error(PtzController *p, char *out, size_t out_sz);
