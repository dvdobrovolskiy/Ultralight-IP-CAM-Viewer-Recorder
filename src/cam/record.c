#include "cam/record.h"

#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <libavutil/error.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/util.h"

void recorder_close(Recorder *rec) {
  if (!rec) return;
  if (rec->bsf) {
    av_bsf_free(&rec->bsf);
    rec->bsf = NULL;
  }
  if (rec->out_ctx) {
    av_write_trailer(rec->out_ctx);
    if (rec->out_ctx->pb) avio_closep(&rec->out_ctx->pb);
    avformat_free_context(rec->out_ctx);
    rec->out_ctx = NULL;
  }
  rec->active = 0;
}

void recorder_flush(Recorder *rec) {
  if (!rec || !rec->active || !rec->out_ctx || !rec->out_ctx->pb) return;
  avio_flush(rec->out_ctx->pb);
}

int recorder_open_video(Recorder *rec, const AVCodecParameters *par, AVRational in_tb,
                        const char *out_path) {
  if (!rec || !par || !out_path || !*out_path) return -1;
  recorder_close(rec);

  if (!av_guess_format("mpegts", NULL, NULL)) {
    return AVERROR_MUXER_NOT_FOUND;
  }

  AVFormatContext *out = NULL;
  int rc = avformat_alloc_output_context2(&out, NULL, "mpegts", out_path);
  if (rc < 0 || !out) return (rc < 0) ? rc : AVERROR_UNKNOWN;

  AVStream *out_st = avformat_new_stream(out, NULL);
  if (!out_st) {
    avformat_free_context(out);
    return AVERROR(ENOMEM);
  }
  rc = avcodec_parameters_copy(out_st->codecpar, par);
  if (rc < 0) {
    avformat_free_context(out);
    return rc;
  }
  out_st->codecpar->codec_tag = 0;
  out_st->time_base = in_tb;

  AVBSFContext *bsf = NULL;
  if (par->codec_id == AV_CODEC_ID_H264) {
    const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
    if (f) {
      AVBSFContext *ctx = NULL;
      rc = av_bsf_alloc(f, &ctx);
      if (rc >= 0 && ctx) {
        rc = avcodec_parameters_copy(ctx->par_in, par);
        if (rc >= 0) {
          ctx->time_base_in = in_tb;
          rc = av_bsf_init(ctx);
        }
        if (rc >= 0) {
          bsf = ctx;
        } else {
          av_bsf_free(&ctx);
        }
      }
    }
  }

  if (!(out->oformat->flags & AVFMT_NOFILE)) {
    rc = avio_open(&out->pb, out_path, AVIO_FLAG_WRITE);
    if (rc < 0) {
      if (bsf) av_bsf_free(&bsf);
      avformat_free_context(out);
      return rc;
    }
  }

  rc = avformat_write_header(out, NULL);
  if (rc < 0) {
    if (bsf) av_bsf_free(&bsf);
    if (out->pb) avio_closep(&out->pb);
    avformat_free_context(out);
    return rc;
  }

  rec->out_ctx = out;
  rec->bsf = bsf;
  rec->src_time_base = in_tb;
  rec->active = 1;
  return 0;
}

void recorder_write_video(Recorder *rec, const AVPacket *pkt) {
  if (!rec || !rec->active || !rec->out_ctx || !pkt) return;
  AVStream *out_st = rec->out_ctx->streams[0];

  AVPacket opkt;
  memset(&opkt, 0, sizeof(opkt));
  if (av_packet_ref(&opkt, pkt) < 0) return;

  if (rec->bsf) {
    int rc = av_bsf_send_packet(rec->bsf, &opkt);
    if (rc < 0) {
      av_packet_unref(&opkt);
      log_ffmpeg("Recording (bsf send) failed", rc);
      recorder_close(rec);
      return;
    }
    AVPacket fpkt;
    while ((rc = av_bsf_receive_packet(rec->bsf, &fpkt)) == 0) {
      AVRational src_tb =
          (rec->bsf->time_base_out.num && rec->bsf->time_base_out.den) ? rec->bsf->time_base_out
                                                                       : rec->src_time_base;
      fpkt.stream_index = 0;
      if (fpkt.pts != AV_NOPTS_VALUE) {
        fpkt.pts = av_rescale_q_rnd(fpkt.pts, src_tb, out_st->time_base,
                                    AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      }
      if (fpkt.dts != AV_NOPTS_VALUE) {
        fpkt.dts = av_rescale_q_rnd(fpkt.dts, src_tb, out_st->time_base,
                                    AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      }
      if (fpkt.duration > 0) {
        fpkt.duration = av_rescale_q(fpkt.duration, src_tb, out_st->time_base);
      }
      fpkt.pos = -1;

      int wrc = av_interleaved_write_frame(rec->out_ctx, &fpkt);
      av_packet_unref(&fpkt);
      if (wrc < 0) {
        log_ffmpeg("Recording write failed", wrc);
        recorder_close(rec);
        break;
      }
    }
    if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF && rc < 0) {
      log_ffmpeg("Recording (bsf receive) failed", rc);
      recorder_close(rec);
    }
    return;
  }

  opkt.stream_index = 0;
  if (opkt.pts != AV_NOPTS_VALUE) {
    opkt.pts = av_rescale_q_rnd(opkt.pts, rec->src_time_base, out_st->time_base,
                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  }
  if (opkt.dts != AV_NOPTS_VALUE) {
    opkt.dts = av_rescale_q_rnd(opkt.dts, rec->src_time_base, out_st->time_base,
                                AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  }
  if (opkt.duration > 0) {
    opkt.duration = av_rescale_q(opkt.duration, rec->src_time_base, out_st->time_base);
  }
  opkt.pos = -1;

  int rc = av_interleaved_write_frame(rec->out_ctx, &opkt);
  if (rc < 0) {
    log_ffmpeg("Recording write failed", rc);
    recorder_close(rec);
  }
  av_packet_unref(&opkt);
}

#ifdef _WIN32
int recorder_open_dated_segment(Recorder *rec, const AVCodecParameters *par, AVRational in_tb,
                                const char *base_use, const char *safe_title, char *out_path,
                                size_t out_path_cap) {
  if (!rec || !par || !base_use || !safe_title || !*safe_title) return -1;

  SYSTEMTIME st = {0};
  GetLocalTime(&st);

  // Day folder dd.MM.yyyy.
  char day_dir[1100] = {0};
  snprintf(day_dir, sizeof(day_dir), "%srecordings/%s/%02d.%02d.%04d", base_use, safe_title,
           (int)st.wDay, (int)st.wMonth, (int)st.wYear);
  ensure_dir_recursive_win32(day_dir);

  // File name = segment start time. ':' is illegal in Windows paths, so use '-'.
  char path[1200] = {0};
  snprintf(path, sizeof(path), "%s/%02d-%02d-%02d.ts", day_dir, (int)st.wHour, (int)st.wMinute,
           (int)st.wSecond);

  int rc = recorder_open_video(rec, par, in_tb, path);
  if (rc == 0 && out_path && out_path_cap) {
    snprintf(out_path, out_path_cap, "%s", path);
  }
  return rc;
}
#else
int recorder_open_dated_segment(Recorder *rec, const AVCodecParameters *par, AVRational in_tb,
                                const char *base_use, const char *safe_title, char *out_path,
                                size_t out_path_cap) {
  (void)rec;
  (void)par;
  (void)in_tb;
  (void)base_use;
  (void)safe_title;
  (void)out_path;
  (void)out_path_cap;
  return -1;
}
#endif
