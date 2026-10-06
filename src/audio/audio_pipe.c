#include "audio/audio_pipe.h"

#include <stdio.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "core/log.h"

#define AUDIO_PIPE_CAP 128  // ~2.5s at 20ms RTP cadence; way more than needed

// Decoded-sample ring drained by the SDL callback. Sized at ~1 s of 48 kHz
// stereo S16 = 192 KB, plenty of headroom for any plausible producer jitter.
#define AUDIO_RING_BYTES (192 * 1024 * 2)

struct AudioPipe {
  SDL_mutex *mutex;
  SDL_cond *cond;
  SDL_atomic_t quit;

  AudioState *audio;
  PluginHost *plugins;
  SDL_atomic_t *volume_pct;
  SDL_atomic_t *playing_sticky;

  // Source descriptor: which camera owns the pipe right now + how to decode
  // its bytes. source_token bumps every time we install new params; the worker
  // compares against its own copy to know when to re-init its decoder.
  int source_cam_idx;             // -1 = no source
  AVCodecParameters *source_codecpar;
  Uint64 source_token;

  // Cloned-packet ring. We accept packets only if their producer matches the
  // current source; on drop we just don't clone, so the producer's original is
  // untouched.
  AVPacket *pkts[AUDIO_PIPE_CAP];
  int pkt_head;
  int pkt_tail;
  int pkt_count;

  // Decoded-sample ring drained by the SDL callback. Worker writes; callback
  // reads. Protected by ring_mutex separate from the packet mutex so the SDL
  // audio thread never blocks on packet bookkeeping.
  SDL_mutex *ring_mutex;
  uint8_t *ring;
  int ring_cap;
  int ring_head;     // callback reads here
  int ring_tail;     // worker writes here
  int ring_filled;
  // Diagnostics for the callback (updated under ring_mutex):
  Uint64 cb_total_out;     // total bytes delivered to SDL
  Uint64 cb_total_silence; // bytes silence-filled because ring was dry
  Uint32 cb_underruns;     // number of callback invocations that needed silence-fill
};

int audio_pipe_init(AudioPipe **out, AudioState *audio, PluginHost *plugins,
                    SDL_atomic_t *volume_pct, SDL_atomic_t *playing_sticky) {
  if (!out) return -1;
  *out = NULL;
  AudioPipe *p = (AudioPipe *)SDL_calloc(1, sizeof(*p));
  if (!p) return -1;
  p->mutex = SDL_CreateMutex();
  p->cond = SDL_CreateCond();
  p->ring_mutex = SDL_CreateMutex();
  p->ring = (uint8_t *)SDL_calloc(1, AUDIO_RING_BYTES);
  if (!p->mutex || !p->cond || !p->ring_mutex || !p->ring) {
    if (p->mutex) SDL_DestroyMutex(p->mutex);
    if (p->cond) SDL_DestroyCond(p->cond);
    if (p->ring_mutex) SDL_DestroyMutex(p->ring_mutex);
    SDL_free(p->ring);
    SDL_free(p);
    return -1;
  }
  p->ring_cap = AUDIO_RING_BYTES;
  p->audio = audio;
  p->plugins = plugins;
  p->volume_pct = volume_pct;
  p->playing_sticky = playing_sticky;
  p->source_cam_idx = -1;
  p->source_codecpar = NULL;
  p->source_token = 0;
  SDL_AtomicSet(&p->quit, 0);
  *out = p;
  return 0;
}

static void drain_locked(AudioPipe *p) {
  while (p->pkt_count > 0) {
    AVPacket *pkt = p->pkts[p->pkt_head];
    p->pkts[p->pkt_head] = NULL;
    p->pkt_head = (p->pkt_head + 1) % AUDIO_PIPE_CAP;
    p->pkt_count--;
    if (pkt) av_packet_free(&pkt);
  }
}

void audio_pipe_signal_quit(AudioPipe *p) {
  if (!p) return;
  SDL_AtomicSet(&p->quit, 1);
  if (p->mutex) {
    SDL_LockMutex(p->mutex);
    SDL_CondBroadcast(p->cond);
    SDL_UnlockMutex(p->mutex);
  }
}

void audio_pipe_destroy(AudioPipe *p) {
  if (!p) return;
  if (p->mutex) SDL_LockMutex(p->mutex);
  drain_locked(p);
  if (p->source_codecpar) avcodec_parameters_free(&p->source_codecpar);
  if (p->mutex) SDL_UnlockMutex(p->mutex);
  if (p->cond) SDL_DestroyCond(p->cond);
  if (p->mutex) SDL_DestroyMutex(p->mutex);
  if (p->ring_mutex) SDL_DestroyMutex(p->ring_mutex);
  SDL_free(p->ring);
  SDL_free(p);
}

// ---- decoded-sample ring -----------------------------------------------

static void ring_clear_locked(AudioPipe *p) {
  p->ring_head = 0;
  p->ring_tail = 0;
  p->ring_filled = 0;
}

// Write decoded interleaved S16 samples into the ring. If the ring would
// overflow we drop the OLDEST samples (advance head) — keeps latency bounded
// and lets the most recent audio play.
static void ring_write(AudioPipe *p, const uint8_t *data, int len) {
  if (!p->ring || len <= 0) return;
  SDL_LockMutex(p->ring_mutex);
  int cap = p->ring_cap;
  int free_space = cap - p->ring_filled;
  if (len > free_space) {
    int overflow = len - free_space;
    p->ring_head = (p->ring_head + overflow) % cap;
    p->ring_filled -= overflow;
  }
  // Write in up to two segments around the wrap.
  int first = cap - p->ring_tail;
  if (first > len) first = len;
  memcpy(p->ring + p->ring_tail, data, (size_t)first);
  int rest = len - first;
  if (rest > 0) memcpy(p->ring, data + first, (size_t)rest);
  p->ring_tail = (p->ring_tail + len) % cap;
  p->ring_filled += len;
  SDL_UnlockMutex(p->ring_mutex);
}

void audio_pipe_sdl_callback(void *userdata, Uint8 *stream, int len) {
  AudioPipe *p = (AudioPipe *)userdata;
  if (!p || !stream || len <= 0) {
    if (stream && len > 0) memset(stream, 0, (size_t)len);
    return;
  }
  SDL_LockMutex(p->ring_mutex);
  int take = (len < p->ring_filled) ? len : p->ring_filled;
  if (take > 0) {
    int first = p->ring_cap - p->ring_head;
    if (first > take) first = take;
    memcpy(stream, p->ring + p->ring_head, (size_t)first);
    int rest = take - first;
    if (rest > 0) memcpy(stream + first, p->ring, (size_t)rest);
    p->ring_head = (p->ring_head + take) % p->ring_cap;
    p->ring_filled -= take;
  }
  if (take < len) {
    // Silence-fill the rest. SDL never gets a click — at worst a brief silence.
    memset(stream + take, 0, (size_t)(len - take));
    p->cb_total_silence += (Uint64)(len - take);
    p->cb_underruns++;
  }
  p->cb_total_out += (Uint64)len;
  SDL_UnlockMutex(p->ring_mutex);
}

void audio_pipe_announce_source(AudioPipe *p, int cam_idx,
                                const AVCodecParameters *codecpar) {
  if (!p || !codecpar) return;
  SDL_LockMutex(p->mutex);
  int same_cam = (p->source_cam_idx == cam_idx);
  int same_codec = same_cam && p->source_codecpar &&
                   p->source_codecpar->codec_id == codecpar->codec_id &&
                   p->source_codecpar->sample_rate == codecpar->sample_rate &&
                   p->source_codecpar->ch_layout.nb_channels == codecpar->ch_layout.nb_channels;
  if (same_codec) {
    SDL_UnlockMutex(p->mutex);
    return;  // idempotent fast path
  }

  drain_locked(p);
  if (p->source_codecpar) avcodec_parameters_free(&p->source_codecpar);
  p->source_codecpar = avcodec_parameters_alloc();
  if (p->source_codecpar) avcodec_parameters_copy(p->source_codecpar, codecpar);
  p->source_cam_idx = cam_idx;
  p->source_token++;
  // Flush any stale decoded audio from the previous source so we don't briefly
  // play the wrong camera's tail end before the new source's samples arrive.
  SDL_LockMutex(p->ring_mutex);
  ring_clear_locked(p);
  SDL_UnlockMutex(p->ring_mutex);
#ifdef _WIN32
  startup_tracef(NULL, "audio_pipe_source cam=%d codec_id=%d sr=%d ch=%d token=%llu",
                 cam_idx, (int)codecpar->codec_id, codecpar->sample_rate,
                 codecpar->ch_layout.nb_channels, (unsigned long long)p->source_token);
#endif
  SDL_CondBroadcast(p->cond);
  SDL_UnlockMutex(p->mutex);
}

void audio_pipe_clear_source(AudioPipe *p) {
  if (!p) return;
  SDL_LockMutex(p->mutex);
  if (p->source_cam_idx != -1 || p->source_codecpar) {
    drain_locked(p);
    if (p->source_codecpar) avcodec_parameters_free(&p->source_codecpar);
    p->source_codecpar = NULL;
    p->source_cam_idx = -1;
    p->source_token++;
    SDL_CondBroadcast(p->cond);
#ifdef _WIN32
    startup_tracef(NULL, "audio_pipe_source_cleared token=%llu",
                   (unsigned long long)p->source_token);
#endif
  }
  SDL_UnlockMutex(p->mutex);
}

int audio_pipe_push(AudioPipe *p, int cam_idx, const AVPacket *pkt) {
  if (!p || !pkt) return 0;
  SDL_LockMutex(p->mutex);
  if (cam_idx != p->source_cam_idx) {
    SDL_UnlockMutex(p->mutex);
    return 0;
  }
  if (p->pkt_count >= AUDIO_PIPE_CAP) {
    // Worker is stuck. Drop new packets — keeps latency from ballooning further.
    SDL_UnlockMutex(p->mutex);
    return 0;
  }
  AVPacket *clone = av_packet_clone(pkt);
  if (!clone) {
    SDL_UnlockMutex(p->mutex);
    return 0;
  }
  p->pkts[p->pkt_tail] = clone;
  p->pkt_tail = (p->pkt_tail + 1) % AUDIO_PIPE_CAP;
  p->pkt_count++;
  SDL_CondSignal(p->cond);
  SDL_UnlockMutex(p->mutex);
  return 1;
}

// --- Worker --------------------------------------------------------------

typedef struct AudioWorkerState {
  AVCodecContext *adec;
  SwrContext *swr;
  AVFrame *aframe;
  Uint64 my_token;
  int out_rate;
  int out_ch_cnt;
  int primed;             // silence cushion queued for this source?
  // Heartbeat stats (windowed; reset every emit)
  Uint32 t_heartbeat;
  Uint32 dec;
  Uint32 dropped_full;
  Uint64 sent;
  int peak_in;
  int peak_out;
  Uint32 clipped;         // # of post-vol samples that hit ±32767
  Uint32 min_queued;      // min SDL queue size observed (bytes)
} AudioWorkerState;

static void worker_teardown_decoder(AudioWorkerState *s) {
  if (!s) return;
  swr_free(&s->swr);
  avcodec_free_context(&s->adec);
  // aframe is reused across decoder re-inits — just unref if there's lingering content
  if (s->aframe) av_frame_unref(s->aframe);
  s->out_rate = 0;
  s->out_ch_cnt = 0;
  s->primed = 0;
}

// Prime the decoded-sample ring with silence so the callback never has to
// scramble for samples in the first 200 ms. Sized big enough to ride out a
// producer (tile_stream / decode.c) stall on an H.264 keyframe decode.
static void worker_prime_silence(AudioPipe *p, AudioWorkerState *s) {
  if (!p || s->out_rate <= 0 || s->out_ch_cnt <= 0) return;
  int prime_ms = 200;
  int frames = (s->out_rate * prime_ms + 999) / 1000;
  size_t bytes = (size_t)frames * (size_t)s->out_ch_cnt * sizeof(int16_t);
  if (bytes == 0) return;
  uint8_t *zeros = (uint8_t *)SDL_calloc(1, bytes);
  if (!zeros) return;
  ring_write(p, zeros, (int)bytes);
  SDL_free(zeros);
#ifdef _WIN32
  startup_tracef(NULL, "audio_worker_primed bytes=%u ms=%d",
                 (unsigned)bytes, prime_ms);
#endif
}

// Holds the pipe mutex on entry; releases it. Returns 1 if a decoder is ready.
static int worker_init_decoder_locked(AudioPipe *p, AudioWorkerState *s) {
  AVCodecParameters *cpar_copy = NULL;
  if (p->source_codecpar) {
    cpar_copy = avcodec_parameters_alloc();
    if (cpar_copy) avcodec_parameters_copy(cpar_copy, p->source_codecpar);
  }
  Uint64 token = p->source_token;
  SDL_UnlockMutex(p->mutex);

  worker_teardown_decoder(s);
  s->my_token = token;

  if (!cpar_copy) return 0;

  const AVCodec *codec = avcodec_find_decoder(cpar_copy->codec_id);
  if (!codec) {
#ifdef _WIN32
    startup_tracef(NULL, "audio_worker_no_codec codec_id=%d", (int)cpar_copy->codec_id);
#endif
    avcodec_parameters_free(&cpar_copy);
    return 0;
  }
  AVCodecContext *adec = avcodec_alloc_context3(codec);
  if (!adec) {
    avcodec_parameters_free(&cpar_copy);
    return 0;
  }
  if (avcodec_parameters_to_context(adec, cpar_copy) < 0 ||
      avcodec_open2(adec, codec, NULL) < 0) {
#ifdef _WIN32
    startup_tracef(NULL, "audio_worker_open_fail codec=%s",
                   avcodec_get_name(cpar_copy->codec_id));
#endif
    avcodec_parameters_free(&cpar_copy);
    avcodec_free_context(&adec);
    return 0;
  }
  avcodec_parameters_free(&cpar_copy);

  SDL_AudioDeviceID adev = 0;
  SDL_AudioSpec have = {0};
  if (!audio_state_snapshot(p->audio, &adev, &have) || adev == 0) {
    avcodec_free_context(&adec);
    return 0;
  }
  int out_ch_cnt = have.channels > 0 ? have.channels : 2;
  int out_rate = have.freq > 0 ? have.freq : 48000;

  AVChannelLayout out_ch;
  av_channel_layout_default(&out_ch, out_ch_cnt);
  SwrContext *swr = NULL;
  if (swr_alloc_set_opts2(&swr, &out_ch, AV_SAMPLE_FMT_S16, out_rate,
                          &adec->ch_layout, adec->sample_fmt, adec->sample_rate,
                          0, NULL) < 0 ||
      !swr || swr_init(swr) < 0) {
    av_channel_layout_uninit(&out_ch);
    swr_free(&swr);
    avcodec_free_context(&adec);
    return 0;
  }
  av_channel_layout_uninit(&out_ch);

  s->adec = adec;
  s->swr = swr;
  s->out_rate = out_rate;
  s->out_ch_cnt = out_ch_cnt;
  if (!s->aframe) s->aframe = av_frame_alloc();
#ifdef _WIN32
  startup_tracef(NULL, "audio_worker_ready codec=%s sr=%d ch=%d -> %dHz/%dch S16",
                 avcodec_get_name(adec->codec_id), adec->sample_rate,
                 adec->ch_layout.nb_channels, out_rate, out_ch_cnt);
#endif
  return 1;
}

static void worker_decode_packet(AudioPipe *p, AudioWorkerState *s, AVPacket *pkt) {
  if (!s->adec || !s->swr || !s->aframe) return;
  int sp = avcodec_send_packet(s->adec, pkt);
  if (sp < 0 && sp != AVERROR(EAGAIN)) return;

  while (avcodec_receive_frame(s->adec, s->aframe) >= 0) {
    if (s->aframe->nb_samples <= 0) continue;
    s->dec++;

    // The ring drops oldest on overflow internally, so no max-queue check here.
    {
      SDL_LockMutex(p->ring_mutex);
      int filled = p->ring_filled;
      SDL_UnlockMutex(p->ring_mutex);
      if ((Uint32)filled < s->min_queued) s->min_queued = (Uint32)filled;
    }

    // Pre-swr peak (S16 packed mono is the common path)
    if (s->adec->sample_fmt == AV_SAMPLE_FMT_S16) {
      const int16_t *is = (const int16_t *)s->aframe->extended_data[0];
      int in_n = s->aframe->nb_samples * s->adec->ch_layout.nb_channels;
      for (int i = 0; i < in_n; i++) {
        int v = is[i] < 0 ? -is[i] : is[i];
        if (v > s->peak_in) s->peak_in = v;
      }
    }

    int64_t delay = swr_get_delay(s->swr, s->adec->sample_rate);
    int out_nb = (int)av_rescale_rnd(delay + s->aframe->nb_samples,
                                     s->out_rate, s->adec->sample_rate, AV_ROUND_UP);
    if (out_nb <= 0) continue;
    uint8_t *out_buf = NULL;
    int out_linesize = 0;
    if (av_samples_alloc(&out_buf, &out_linesize, s->out_ch_cnt, out_nb,
                         AV_SAMPLE_FMT_S16, 0) < 0 || !out_buf) continue;

    int converted = swr_convert(s->swr, &out_buf, out_nb,
                                (const uint8_t **)s->aframe->extended_data,
                                s->aframe->nb_samples);
    if (converted > 0) {
      int data_size = av_samples_get_buffer_size(NULL, s->out_ch_cnt, converted,
                                                  AV_SAMPLE_FMT_S16, 1);
      if (data_size > 0) {
        int vol = p->volume_pct ? SDL_AtomicGet(p->volume_pct) : 100;
        if (vol < 0) vol = 0; else if (vol > 200) vol = 200;
        if (vol == 0) {
          memset(out_buf, 0, (size_t)data_size);
        } else if (vol != 100) {
          // Soft-knee limiter: below ±28000 we apply the gain linearly; above
          // we asymptotically approach ±32767 instead of hard-clipping. That
          // kills the audible "cracks" people get when boosting a PCMA stream
          // that already fills the S16 range.
          // Curve: y = knee + over*K / (over + K), with K = 32767-knee = 4767.
          int16_t *sm = (int16_t *)out_buf;
          int sc = data_size / (int)sizeof(int16_t);
          const int32_t knee = 28000;
          const int32_t headroom = 32767 - knee;  // 4767
          for (int i = 0; i < sc; i++) {
            int32_t v = (int32_t)sm[i] * vol / 100;
            if (v > knee) {
              int32_t over = v - knee;
              v = knee + (over * headroom) / (over + headroom);
              s->clipped++;
            } else if (v < -knee) {
              int32_t over = -v - knee;
              v = -(knee + (over * headroom) / (over + headroom));
              s->clipped++;
            }
            sm[i] = (int16_t)v;
          }
        }
        // Post-vol peak
        const int16_t *ps = (const int16_t *)out_buf;
        int psn = data_size / (int)sizeof(int16_t);
        for (int i = 0; i < psn; i++) {
          int v = ps[i] < 0 ? -ps[i] : ps[i];
          if (v > s->peak_out) s->peak_out = v;
        }
        if (p->plugins) {
          RtspAudioFrameS16 af = {0};
          af.size = (uint32_t)sizeof(af);
          af.samples = (const int16_t *)out_buf;
          af.sample_count = data_size / 2;
          af.channels = s->out_ch_cnt;
          af.sample_rate_hz = s->out_rate;
          af.pts_ms = 0;
          plugin_host_on_audio(p->plugins, &af);
        }
        if (!s->primed) {
          worker_prime_silence(p, s);
          s->primed = 1;
        }
        ring_write(p, out_buf, data_size);
        s->sent += (Uint64)data_size;
        if (p->playing_sticky) SDL_AtomicSet(p->playing_sticky, 1);
      }
    }
    av_freep(&out_buf);
  }
}

int audio_worker_thread_fn(void *userdata) {
  AudioPipe *p = (AudioPipe *)userdata;
  if (!p) return 0;
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

  AudioWorkerState s = {0};
  s.t_heartbeat = SDL_GetTicks();
  s.min_queued = 0xFFFFFFFFu;

  while (!SDL_AtomicGet(&p->quit)) {
    SDL_LockMutex(p->mutex);

    // Wait until there's something to do: quit, source change, or a packet.
    while (!SDL_AtomicGet(&p->quit) &&
           p->source_token == s.my_token &&
           p->pkt_count == 0) {
      SDL_CondWait(p->cond, p->mutex);
    }
    if (SDL_AtomicGet(&p->quit)) {
      SDL_UnlockMutex(p->mutex);
      break;
    }

    if (p->source_token != s.my_token) {
      // Re-init decoder (or tear down if source cleared). Releases the mutex.
      worker_init_decoder_locked(p, &s);
      continue;  // loop back, may now find queued packets
    }

    // Pop one packet
    AVPacket *pkt = NULL;
    if (p->pkt_count > 0) {
      pkt = p->pkts[p->pkt_head];
      p->pkts[p->pkt_head] = NULL;
      p->pkt_head = (p->pkt_head + 1) % AUDIO_PIPE_CAP;
      p->pkt_count--;
    }
    SDL_UnlockMutex(p->mutex);

    if (pkt) {
      worker_decode_packet(p, &s, pkt);
      av_packet_free(&pkt);
    }

#ifdef _WIN32
    Uint32 now = SDL_GetTicks();
    if (now - s.t_heartbeat >= 2000) {
      SDL_LockMutex(p->ring_mutex);
      int ring_now = p->ring_filled;
      Uint64 cb_out = p->cb_total_out;
      Uint64 cb_silence = p->cb_total_silence;
      Uint32 cb_under = p->cb_underruns;
      p->cb_total_out = 0;
      p->cb_total_silence = 0;
      p->cb_underruns = 0;
      SDL_UnlockMutex(p->ring_mutex);
      Uint32 min_r = (s.min_queued == 0xFFFFFFFFu) ? (Uint32)ring_now : s.min_queued;
      startup_tracef(NULL,
                     "audio_worker_hb ring=%d min_ring=%u dec=%u clipped=%u sent=%llu peak_in=%d peak_out=%d cb_out=%llu cb_silence=%llu cb_under=%u",
                     ring_now, min_r,
                     (unsigned)s.dec, (unsigned)s.clipped,
                     (unsigned long long)s.sent, s.peak_in, s.peak_out,
                     (unsigned long long)cb_out, (unsigned long long)cb_silence,
                     (unsigned)cb_under);
      s.t_heartbeat = now;
      s.dec = 0;
      s.dropped_full = 0;
      s.clipped = 0;
      s.sent = 0;
      s.peak_in = 0;
      s.peak_out = 0;
      s.min_queued = 0xFFFFFFFFu;
    }
#endif
  }

  worker_teardown_decoder(&s);
  av_frame_free(&s.aframe);
  return 0;
}
