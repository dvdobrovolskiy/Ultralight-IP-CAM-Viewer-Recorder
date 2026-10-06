#pragma once

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

// An asynchronous, fully isolated per-camera recorder.
//
// The whole project's recording philosophy: a decode worker must never block on
// disk I/O. An AsyncRecorder owns its own thread and a bounded packet queue. The
// worker only does a cheap av_packet_ref + enqueue (async_recorder_feed); the
// recorder thread performs every blocking operation — directory creation, muxer
// open/close, packet writes, periodic flush, hourly roll — on its own schedule.
// It is decoupled from the worker's live AVFormatContext: it records from a
// snapshot of the source video stream's codec parameters + time_base, so the
// worker can reconnect or tear down its context at any time.
//
// If the disk stalls, the queue is bounded (oldest packets are dropped) so the
// worker is never throttled and memory stays bounded — recording integrity is
// sacrificed before UI fluidity, by design.
typedef struct AsyncRecorder AsyncRecorder;

// Creates the recorder and starts its thread. recordings_base_dir may be empty
// (falls back to the executable directory). Never returns NULL on success only;
// returns NULL on allocation failure.
AsyncRecorder *async_recorder_create(int cam_index, const char *recordings_base_dir);

// Stops the thread (draining any queued packets to disk), closes the open file,
// and frees everything. Safe on NULL.
void async_recorder_free(AsyncRecorder *ar);

// Provides/refreshes the source video stream parameters. Call once after the
// stream opens and again after each reconnect — each call rolls a fresh file so
// timestamps stay continuous within a segment. Snapshots par (deep copy).
void async_recorder_set_source(AsyncRecorder *ar, const AVCodecParameters *par, AVRational tb);

// Marks the source as gone (e.g. the worker is about to free its context on a
// disconnect). The recorder closes the current file promptly.
void async_recorder_invalidate_source(AsyncRecorder *ar);

// Per-video-packet hand-off from the decode worker. Non-blocking:
//   want  - desired recording state (cams[i].record_enabled, read by the worker)
//   title - camera display name for the on-disk folder
//   pkt   - the demuxed video packet (cloned into the queue when want is set)
// When want is 0 the recorder closes any open file. pkt may be NULL.
void async_recorder_feed(AsyncRecorder *ar, int want, const char *title, const AVPacket *pkt);
