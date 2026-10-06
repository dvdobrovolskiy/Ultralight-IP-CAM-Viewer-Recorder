#pragma once

#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>

// A single-video-stream MPEG-TS recorder. It is deliberately decoupled from any
// live input AVFormatContext: it is opened from a snapshot of the source video
// stream's codec parameters + time_base, so it can run on a dedicated thread
// (see async_recorder.h) without ever touching the decode worker's context.
typedef struct Recorder {
  int active;
  AVFormatContext *out_ctx;   // single video out stream at index 0
  AVBSFContext *bsf;          // optional h264_mp4toannexb, NULL if not needed
  AVRational src_time_base;   // source video stream time_base (snapshot)
} Recorder;

// Length of one recording file. Recordings are rolled into a new .ts every
// RECORDER_SEGMENT_MS, on a keyframe boundary so each file decodes cleanly.
// One file per hour: opening/closing a file (mkdir + header + trailer + close)
// is the expensive, blocking part of recording, so we do it rarely. Durability
// between rolls comes from RECORDER_FLUSH_MS (below), not from short segments.
#define RECORDER_SEGMENT_MS 3600000u /* 1 hour */

// How often to flush the open .ts to the OS while recording. MPEG-TS has no
// trailer/index, so a periodically-flushed file is fully playable even if the
// program is killed mid-segment — this is what makes hour-long files safe.
// Cheap (a write() of the buffered bytes), so it never stalls the recorder.
#define RECORDER_FLUSH_MS 5000u /* 5 seconds */

void recorder_close(Recorder *rec);

// Opens a single-video-stream MPEG-TS file at out_path from a snapshot of the
// source stream's codec parameters and time_base. Returns 0 on success or a
// negative ffmpeg error code.
int recorder_open_video(Recorder *rec, const AVCodecParameters *par, AVRational in_tb,
                        const char *out_path);

// Writes one source video packet to the open recorder, rescaling timestamps
// from the snapshot time_base. The caller guarantees pkt is a video packet.
void recorder_write_video(Recorder *rec, const AVPacket *pkt);

// Pushes buffered bytes of the open segment to the OS so they survive a crash
// or normal close. No-op when not recording.
void recorder_flush(Recorder *rec);

// Opens one recording segment using the shared on-disk layout:
//   <base_use>recordings/<safe_title>/<dd.MM.yyyy>/<HH-MM-SS>.ts
// where the day folder and file name are taken from the current local time
// (the segment start). 'safe_title' must already be a filesystem-safe name.
// Creates the day directory, opens an MPEG-TS recorder, and on success copies
// the full file path into out_path (if non-NULL). Returns 0 on success or a
// negative ffmpeg error code. Windows-only; returns -1 on other platforms.
int recorder_open_dated_segment(Recorder *rec, const AVCodecParameters *par, AVRational in_tb,
                                const char *base_use, const char *safe_title, char *out_path,
                                size_t out_path_cap);
