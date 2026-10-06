#include "cam/audio_stream.h"

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "core/log.h"

static int audio_stream_interrupt_cb(void *opaque) {
  AudioStreamArgs *a = (AudioStreamArgs *)opaque;
  return (a && SDL_AtomicGet(&a->quit)) ? 1 : 0;
}

#ifdef _WIN32
#define AUDIO_TRACE(...) startup_tracef(NULL, __VA_ARGS__)
#else
#define AUDIO_TRACE(...) ((void)0)
#endif

int audio_stream_thread_fn(void *userdata) {
  AudioStreamArgs *a = (AudioStreamArgs *)userdata;
  if (!a || !a->audio) return 0;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
  SDL_AtomicSet(&a->playing, 0);
  SDL_AtomicSet(&a->state, AUDIO_STREAM_CONNECTING);

  int backoff_ms = 500;

  while (!SDL_AtomicGet(&a->quit)) {
    if (!a->url[0]) {
      SDL_Delay(200);
      continue;
    }

    SDL_AtomicSet(&a->state, AUDIO_STREAM_CONNECTING);

    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec have = {0};
    if (!audio_state_snapshot(a->audio, &dev, &have) || dev == 0) {
      // Audio device isn't open yet (init runs on its own thread); wait and retry.
      SDL_Delay(150);
      continue;
    }

    AVDictionary *opts = NULL;
    if (a->type == CAMERA_TYPE_RTSP) av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
    av_dict_set(&opts, "rw_timeout", "5000000", 0);

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
      av_dict_free(&opts);
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }
    fmt->interrupt_callback.callback = audio_stream_interrupt_cb;
    fmt->interrupt_callback.opaque = a;
    // Cap stream-info probing tightly. RTSP SDP gives us everything we need, so
    // find_stream_info just confirms it and exits in tens of ms — without these
    // caps it would read several seconds of packets first.
    fmt->probesize = 8192;
    fmt->max_analyze_duration = 500000; // 500 ms — enough to confirm codec params
                                        // from SDP without reading seconds of audio

    Uint32 t_open = SDL_GetTicks();
    int err = avformat_open_input(&fmt, a->url, NULL, &opts);
    av_dict_free(&opts);
    if (err < 0 || !fmt) {
      if (fmt) avformat_free_context(fmt);
      AUDIO_TRACE("audio_stream_open_fail cam=%d err=%d", a->cam_idx, err);
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    // Run find_stream_info every time (bounded). Skipping it left codec params
    // sometimes only partially populated — sample_rate/channels could be 0 for
    // some servers, producing silence or "tiny bits" of audio after open.
    err = avformat_find_stream_info(fmt, NULL);
    if (err < 0) {
      AUDIO_TRACE("audio_stream_find_info_fail cam=%d err=%d", a->cam_idx, err);
      avformat_close_input(&fmt);
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }
    int astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    Uint32 dt_open = (Uint32)(SDL_GetTicks() - t_open);
    AUDIO_TRACE("audio_stream_opened cam=%d dt_ms=%u nb_streams=%u astream=%d",
                a->cam_idx, (unsigned)dt_open, fmt->nb_streams, astream);
    if (astream < 0) {
      SDL_AtomicSet(&a->state, AUDIO_STREAM_NO_AUDIO);
      // No audio stream on this camera — stay idle (no point reopening on a backoff
      // loop). We'll be killed when the user switches to another camera.
      avformat_close_input(&fmt);
      while (!SDL_AtomicGet(&a->quit)) SDL_Delay(250);
      break;
    }

    AVStream *st = fmt->streams[astream];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    const char *codec_name = avcodec_get_name(st->codecpar->codec_id);
    AVCodecContext *adec = NULL;
    if (codec) adec = avcodec_alloc_context3(codec);

    if (!codec || !adec ||
        avcodec_parameters_to_context(adec, st->codecpar) < 0 ||
        avcodec_open2(adec, codec, NULL) < 0) {
      AUDIO_TRACE("audio_stream_codec_open_fail cam=%d codec=%s codec_id=%d",
                  a->cam_idx, codec_name ? codec_name : "?",
                  (int)st->codecpar->codec_id);
      avcodec_free_context(&adec);
      avformat_close_input(&fmt);
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }
    AUDIO_TRACE("audio_stream_codec cam=%d codec=%s sr=%d ch=%d fmt=%s bitrate=%lld",
                a->cam_idx, codec_name ? codec_name : "?", adec->sample_rate,
                adec->ch_layout.nb_channels,
                av_get_sample_fmt_name(adec->sample_fmt) ? av_get_sample_fmt_name(adec->sample_fmt) : "?",
                (long long)adec->bit_rate);

    int out_ch_cnt = have.channels > 0 ? have.channels : 2;
    enum AVSampleFormat out_fmt = AV_SAMPLE_FMT_S16;
    int out_rate = have.freq > 0 ? have.freq : 48000;

    AVChannelLayout out_ch;
    av_channel_layout_default(&out_ch, out_ch_cnt);

    SwrContext *swr = NULL;
    if (swr_alloc_set_opts2(&swr, &out_ch, out_fmt, out_rate,
                            &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, NULL) < 0 ||
        !swr || swr_init(swr) < 0) {
      AUDIO_TRACE("audio_stream_swr_init_fail cam=%d in_sr=%d in_ch=%d out_sr=%d out_ch=%d",
                  a->cam_idx, adec->sample_rate, adec->ch_layout.nb_channels,
                  out_rate, out_ch_cnt);
      av_channel_layout_uninit(&out_ch);
      swr_free(&swr);
      avcodec_free_context(&adec);
      avformat_close_input(&fmt);
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }
    av_channel_layout_uninit(&out_ch);
    AUDIO_TRACE("audio_stream_ready cam=%d %s %dHz/%dch -> %dHz/%dch S16",
                a->cam_idx, codec_name ? codec_name : "?", adec->sample_rate,
                adec->ch_layout.nb_channels, out_rate, out_ch_cnt);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *aframe = av_frame_alloc();
    if (!pkt || !aframe) {
      av_packet_free(&pkt);
      av_frame_free(&aframe);
      swr_free(&swr);
      avcodec_free_context(&adec);
      avformat_close_input(&fmt);
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
      continue;
    }

    backoff_ms = 500;
    SDL_ClearQueuedAudio(dev);
    // Audio codec opened — the pipeline is live; future packets will produce sound.
    SDL_AtomicSet(&a->state, AUDIO_STREAM_READY);

    Uint32 t_heartbeat = SDL_GetTicks();
    Uint64 total_queued_bytes = 0;
    Uint32 frames_decoded = 0;
    Uint32 frames_dropped_full = 0;
    int read_err_logged = 0;
    int peak_since_hb = 0;          // max |sample| in the window (post-swr, post-vol)
    Uint64 nonzero_since_hb = 0;    // count of non-zero S16 samples
    int peak_in_since_hb = 0;       // max |sample| in raw decoded frames (pre-swr)

    while (!SDL_AtomicGet(&a->quit)) {
      Uint32 now = SDL_GetTicks();
      if (now - t_heartbeat >= 2000) {
        AUDIO_TRACE("audio_stream_heartbeat cam=%d queued=%u dec=%u dropped=%u sent_total=%llu peak_in=%d peak_out=%d nonzero=%llu",
                    a->cam_idx, (unsigned)SDL_GetQueuedAudioSize(dev),
                    (unsigned)frames_decoded, (unsigned)frames_dropped_full,
                    (unsigned long long)total_queued_bytes,
                    peak_in_since_hb, peak_since_hb,
                    (unsigned long long)nonzero_since_hb);
        t_heartbeat = now;
        peak_since_hb = 0;
        nonzero_since_hb = 0;
        peak_in_since_hb = 0;
      }

      err = av_read_frame(fmt, pkt);
      if (err < 0) {
        if (!read_err_logged) {
          AUDIO_TRACE("audio_stream_read_err cam=%d err=%d", a->cam_idx, err);
          read_err_logged = 1;
        }
        break;
      }

      if (pkt->stream_index != astream) {
        av_packet_unref(pkt);
        continue;
      }

      err = avcodec_send_packet(adec, pkt);
      av_packet_unref(pkt);
      if (err < 0) {
        if (err == AVERROR(EAGAIN)) continue;
        AUDIO_TRACE("audio_stream_send_err cam=%d err=%d", a->cam_idx, err);
        break;
      }

      while (!SDL_AtomicGet(&a->quit) && (err = avcodec_receive_frame(adec, aframe)) >= 0) {
        if (aframe->nb_samples <= 0) continue;
        frames_decoded++;

        Uint32 queued = SDL_GetQueuedAudioSize(dev);
        Uint32 max_queue = (Uint32)(out_rate * out_ch_cnt * 2); // ~1s of S16 frames
        // If the device is way behind, drop this frame instead of wiping the entire
        // queue — wiping caused mid-playback silence on streams whose demuxer briefly
        // outpaces the device (e.g., after find_stream_info drained buffered packets).
        if (queued > max_queue) {
          frames_dropped_full++;
          continue;
        }

        int64_t delay = swr_get_delay(swr, adec->sample_rate);
        int out_nb = (int)av_rescale_rnd(delay + aframe->nb_samples, out_rate,
                                         adec->sample_rate, AV_ROUND_UP);
        if (out_nb <= 0) continue;

        uint8_t *out_buf = NULL;
        int out_linesize = 0;
        int alloc_rc = av_samples_alloc(&out_buf, &out_linesize, out_ch_cnt, out_nb, out_fmt, 0);
        if (alloc_rc < 0 || !out_buf) continue;

        // Pre-swr amplitude inspection (assumes adec->sample_fmt == S16, which it is
        // for pcm_alaw — see audio_stream_codec trace). Reads only plane 0.
        if (adec->sample_fmt == AV_SAMPLE_FMT_S16 ||
            adec->sample_fmt == AV_SAMPLE_FMT_S16P) {
          const int16_t *is = (const int16_t *)aframe->extended_data[0];
          int in_n = aframe->nb_samples *
                     (adec->sample_fmt == AV_SAMPLE_FMT_S16 ? adec->ch_layout.nb_channels : 1);
          for (int i = 0; i < in_n; i++) {
            int v = is[i] < 0 ? -is[i] : is[i];
            if (v > peak_in_since_hb) peak_in_since_hb = v;
          }
        }

        int converted = swr_convert(swr, &out_buf, out_nb,
                                    (const uint8_t **)aframe->extended_data, aframe->nb_samples);
        if (converted > 0) {
          int data_size = av_samples_get_buffer_size(NULL, out_ch_cnt, converted, out_fmt, 1);
          if (data_size > 0) {
            // Software gain: 0..200%. 100% is a no-op fast path. Values >100 boost the
            // signal with hard-clamp to int16 range; this matches VLC's >100 behavior
            // (clipping rather than rescaling) — at 200% loud peaks will distort but
            // quiet content gains audibility.
            int vol = SDL_AtomicGet(&a->volume_pct);
            if (vol < 0) vol = 0; else if (vol > 200) vol = 200;
            if (vol == 0) {
              memset(out_buf, 0, (size_t)data_size);
            } else if (vol != 100) {
              int16_t *s = (int16_t *)out_buf;
              int sample_count = data_size / (int)sizeof(int16_t);
              for (int i = 0; i < sample_count; i++) {
                int32_t v = (int32_t)s[i] * vol / 100;
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                s[i] = (int16_t)v;
              }
            }
            if (a->plugins) {
              RtspAudioFrameS16 af = {0};
              af.size = (uint32_t)sizeof(af);
              af.samples = (const int16_t *)out_buf;
              af.sample_count = data_size / 2;
              af.channels = out_ch_cnt;
              af.sample_rate_hz = out_rate;
              af.pts_ms = 0;
              plugin_host_on_audio(a->plugins, &af);
            }
            // Sample-amplitude inspection so we can tell silence-on-the-wire from
            // silence-after-SDL when debugging "no sound" reports.
            {
              const int16_t *s = (const int16_t *)out_buf;
              int n = data_size / (int)sizeof(int16_t);
              for (int i = 0; i < n; i++) {
                int v = s[i] < 0 ? -s[i] : s[i];
                if (v > peak_since_hb) peak_since_hb = v;
                if (s[i] != 0) nonzero_since_hb++;
              }
            }
            SDL_QueueAudio(dev, out_buf, (Uint32)data_size);
            total_queued_bytes += (Uint64)data_size;
            // Sticky: once we've queued the first real sample, the icon stays visible
            // for the remainder of this stream's lifetime — silent gaps within the
            // audio track shouldn't make it flicker off.
            if (!SDL_AtomicGet(&a->playing)) {
              AUDIO_TRACE("audio_stream_first_sample cam=%d bytes=%d queued_before=%u",
                          a->cam_idx, data_size, (unsigned)SDL_GetQueuedAudioSize(dev) - (unsigned)data_size);
            }
            SDL_AtomicSet(&a->playing, 1);
          }
        }
        av_freep(&out_buf);
      }
    }

    SDL_AtomicSet(&a->playing, 0);
    SDL_ClearQueuedAudio(dev);

    av_packet_free(&pkt);
    av_frame_free(&aframe);
    swr_free(&swr);
    avcodec_free_context(&adec);
    avformat_close_input(&fmt);

    if (!SDL_AtomicGet(&a->quit)) {
      SDL_Delay((Uint32)backoff_ms);
      if (backoff_ms < 8000) backoff_ms *= 2;
    }
  }

  SDL_AtomicSet(&a->playing, 0);
  SDL_AtomicSet(&a->state, AUDIO_STREAM_IDLE);
  // Phase 6: args were heap-allocated per spawn so the click path could detach us
  // instead of waiting. Free them here at thread end.
  SDL_free(a);
  return 0;
}
