#pragma once

#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>

#include "audio/audio.h"
#include "plugin_host.h"

// Single-consumer packet pipe between the per-camera RTSP demuxers (producers)
// and a dedicated audio worker thread (consumer). The worker owns the decoder,
// resampler, and the SDL audio queue write — so audio timing is decoupled from
// any video decode work the producers are doing.
//
// Source selection is by camera index. main.c picks the active source by
// updating `audio_active_idx_atomic` (declared in main.c). Producers compare
// their own camera index to that atomic on every audio packet and, when they
// match, announce_source + push. Other producers drop their audio packets.
//
// announce_source is idempotent: a producer can call it on every packet — only
// the first one (per source change) does real work.
typedef struct AudioPipe AudioPipe;

int audio_pipe_init(AudioPipe **out, AudioState *audio, PluginHost *plugins,
                    SDL_atomic_t *audio_volume_pct, SDL_atomic_t *audio_playing);
// Wake the worker thread and tell it to return. After this returns the caller
// MUST SDL_WaitThread the worker before calling audio_pipe_destroy.
void audio_pipe_signal_quit(AudioPipe *p);
// Frees all pipe resources. The worker must have exited first.
void audio_pipe_destroy(AudioPipe *p);

// Announce that the given camera is now the audio source, with codec params
// copied from RTSP SDP. Idempotent. Triggers a decoder re-init in the worker.
void audio_pipe_announce_source(AudioPipe *p, int cam_idx,
                                const AVCodecParameters *codecpar);

// Push an audio packet from the given camera. Drops the packet (does not clone)
// if cam_idx isn't the current source. Returns 1 if queued, 0 if dropped.
// The caller retains ownership of pkt either way (we clone on accept).
int audio_pipe_push(AudioPipe *p, int cam_idx, const AVPacket *pkt);

// Optional: call from main.c when the active source goes to -1 (modal overlay
// open, no camera selected) so the worker tears down its decoder and any
// pending packets stop playing.
void audio_pipe_clear_source(AudioPipe *p);

int audio_worker_thread_fn(void *userdata);  // userdata = AudioPipe*

// SDL audio callback. Pulls samples from the pipe's internal ring buffer at
// the device clock rate. Underruns silence-fill — never click. Installed on
// SDL_OpenAudioDevice via SDL_AudioSpec.callback; userdata is the AudioPipe*.
void audio_pipe_sdl_callback(void *userdata, Uint8 *stream, int len);
