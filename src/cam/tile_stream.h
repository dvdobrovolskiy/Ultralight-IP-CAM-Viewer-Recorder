#pragma once

#include <SDL.h>

#include "app/camera.h"
#include "audio/audio.h"
#include "audio/audio_pipe.h"
#include "plugin_host.h"

typedef struct TileStreamArgs {
  int index;
  CameraType type;
  char url[2048];
  char base_dir[1024];
  CameraConfig *cams;
  int cam_count;
  SDL_atomic_t *quit;
  SDL_atomic_t *hires_index;
  int min_update_ms;

  // Hard ceiling for the current connect/handshake, as an absolute
  // SDL_GetTicks() value; 0 = none. Written by the worker just before
  // avformat_open_input and cleared once the handshake completes; read by the
  // FFmpeg interrupt callback on the same thread (no locking needed). Bounds
  // the phases stimeout/rw_timeout do NOT cover — DNS, and a TCP connect to a
  // host that silently drops packets (a camera cut from power) — which were
  // observed holding a connection slot for up to 11 minutes.
  Uint32 open_deadline_ticks;

  // Phase 2: per-thread liveness + restart bookkeeping (see WarmCamArgs).
  SDL_atomic_t alive;
  Uint32 spawn_ticks;
  Uint32 next_spawn_ticks;
  int backoff_ms;
  int restart_count;

  // Phase 8: per-tile recording. When cams[index].record_enabled flips to 1
  // the worker opens an MPEG-TS recorder writing 5-minute segments to
  //   recordings_base_dir/recordings/<safe_title>/<dd.MM.yyyy>/<HH-MM-SS>.ts
  // (start-time file names, rolled on keyframes). The worker reads
  // record_enabled under rec_mutex on each packet.
  char recordings_base_dir[1024];
  SDL_mutex *rec_mutex;

  // Audio handoff: tile_streams demux their own camera's RTSP session but the
  // decode+resample+SDL queue lives in a single dedicated worker thread (see
  // audio_pipe.h). When this tile's index matches *audio_active_idx, the
  // tile_stream clones audio packets to the pipe; the worker decodes them on
  // its own schedule, decoupled from video work.
  AudioPipe *audio_pipe;
  SDL_atomic_t *audio_active_idx;  // shared with main.c; -1 = no tile audible
} TileStreamArgs;

// Limits how many tile workers may run avformat_open_input concurrently. Opening
// all camera RTSP streams at the same instant overwhelms some NVRs/cameras: a
// subset times out (AVERROR(ETIMEDOUT)) and a *different* subset spins on each
// restart. Gating opens to a few at a time lets every camera get a fair turn.
// Call once at startup before spawning tiles. max_concurrent <= 0 disables it.
void tile_stream_init_open_gate(int max_concurrent);

int tile_stream_thread_fn(void *userdata);

void tile_streams_stop(SDL_Thread **ths, TileStreamArgs *args, int count, SDL_atomic_t *quit);
int tile_streams_prepare(SDL_Thread ***out_ths, TileStreamArgs **out_args, SDL_atomic_t *quit,
                         CameraConfig *cams, int cam_count,
                         int use_preview_url, SDL_atomic_t *hires_index, const char *base_dir,
                         const char *recordings_base_dir, SDL_mutex *rec_mutex,
                         AudioPipe *audio_pipe, SDL_atomic_t *audio_active_idx);
void tile_streams_spawn_step(SDL_Thread **ths, TileStreamArgs *args, int count, int *io_next,
                             int max_spawn);

#ifdef _WIN32
void startup_trace_tile_sources(const char *base_dir, const TileStreamArgs *args,
                                const CameraConfig *cams, int cam_count);
#endif
