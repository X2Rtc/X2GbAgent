#include "codec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef GBMEDIA_WITH_FFMPEG
#define GBMEDIA_WITH_FFMPEG 0
#endif

#if GBMEDIA_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#endif

typedef struct {
  gbmcd_codec_t base;
  gbmcd_codec_config_t config;
  gbmcd_codec_callbacks_t callbacks;
  gbmcd_rate_control_feedback_t feedback;
#if GBMEDIA_WITH_FFMPEG
  const AVCodec *avcodec;
  AVCodecContext *ctx;
  AVFrame *frame;
  AVPacket *packet;
  char output_format[GBMCD_MAX_NAME];
  int force_next_keyframe;
#endif
} gbmcd_ffmpeg_codec_t;

static void ffmpeg_close(gbmcd_codec_t *codec);

static int ffmpeg_probe(void) {
#if GBMEDIA_WITH_FFMPEG
  return GBMCD_OK;
#else
  return GBMCD_ERR_UNSUPPORTED;
#endif
}

#if GBMEDIA_WITH_FFMPEG
static int ffmpeg_result(int rc) {
  if (rc == AVERROR(EAGAIN)) return GBMCD_ERR_AGAIN;
  if (rc == AVERROR_EOF) return GBMCD_ERR_EOF;
  if (rc == AVERROR(ENOMEM)) return GBMCD_ERR_NO_MEMORY;
  if (rc == AVERROR(EBUSY)) return GBMCD_ERR_BUSY;
  if (rc == AVERROR(EACCES) || rc == AVERROR(EPERM)) return GBMCD_ERR_PERMISSION;
  return rc < 0 ? GBMCD_ERR_BACKEND : GBMCD_OK;
}

static enum AVMediaType to_av_media(gbmcd_media_type_t media_type) {
  return media_type == GBMCD_MEDIA_AUDIO ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

static enum AVPixelFormat pixel_format_from_name(const char *name) {
  enum AVPixelFormat fmt;
  if (!name || !name[0]) return AV_PIX_FMT_YUV420P;
  fmt = av_get_pix_fmt(name);
  return fmt == AV_PIX_FMT_NONE ? AV_PIX_FMT_YUV420P : fmt;
}

static enum AVSampleFormat sample_format_from_name(const AVCodec *codec,
                                                   const char *name) {
  enum AVSampleFormat fmt = AV_SAMPLE_FMT_NONE;
  if (name && name[0]) fmt = av_get_sample_fmt(name);
  if (fmt != AV_SAMPLE_FMT_NONE) return fmt;
  if (codec && codec->sample_fmts && codec->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
    return codec->sample_fmts[0];
  }
  return AV_SAMPLE_FMT_FLTP;
}

static const AVCodec *find_named_codec(const char *name, gbmcd_codec_role_t role) {
  if (!name || !name[0]) return NULL;
  return role == GBMCD_CODEC_ENCODER
           ? avcodec_find_encoder_by_name(name)
           : avcodec_find_decoder_by_name(name);
}

static const AVCodec *find_codec_by_names(const char *const *names,
                                          gbmcd_codec_role_t role) {
  const AVCodec *codec = NULL;
  for (size_t i = 0; names && names[i]; i++) {
    codec = find_named_codec(names[i], role);
    if (codec) return codec;
  }
  return NULL;
}

static int ascii_ieq(const char *a, const char *b) {
  if (!a || !b) return 0;
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = (char) (ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char) (cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return *a == '\0' && *b == '\0';
}

static const AVCodec *find_project_codec(const char *name,
                                         gbmcd_media_type_t media_type,
                                         gbmcd_codec_role_t role,
                                         int prefer_hardware) {
  static const char *const h264_names[] = {
      "libx264", "h264", "h264_mf", "h264_qsv", "h264_nvenc", "h264_amf", NULL};
  static const char *const h264_hw_names[] = {
      "h264_qsv", "h264_nvenc", "h264_amf", "h264_mf", "libx264", "h264", NULL};
  static const char *const h265_names[] = {
      "libx265", "hevc", "hevc_mf", "hevc_qsv", "hevc_nvenc", "hevc_amf", NULL};
  static const char *const h265_hw_names[] = {
      "hevc_qsv", "hevc_nvenc", "hevc_amf", "hevc_mf", "libx265", "hevc", NULL};
  static const char *const aac_names[] = {"aac", NULL};
  static const char *const opus_names[] = {"libopus", "opus", NULL};
  static const char *const g711a_names[] = {"pcm_alaw", NULL};
  static const char *const g711u_names[] = {"pcm_mulaw", NULL};

  if (!name || !name[0]) return NULL;
  if (media_type == GBMCD_MEDIA_VIDEO) {
    if (ascii_ieq(name, "H264") || ascii_ieq(name, "H.264")) {
      return find_codec_by_names(prefer_hardware ? h264_hw_names : h264_names, role);
    }
    if (ascii_ieq(name, "H265") || ascii_ieq(name, "H.265") ||
        ascii_ieq(name, "HEVC")) {
      return find_codec_by_names(prefer_hardware ? h265_hw_names : h265_names, role);
    }
  } else {
    if (ascii_ieq(name, "G711A") || ascii_ieq(name, "PCMA")) {
      return find_codec_by_names(g711a_names, role);
    }
    if (ascii_ieq(name, "G711U") || ascii_ieq(name, "PCMU")) {
      return find_codec_by_names(g711u_names, role);
    }
    if (ascii_ieq(name, "AAC")) return find_codec_by_names(aac_names, role);
    if (ascii_ieq(name, "OPUS")) return find_codec_by_names(opus_names, role);
  }
  return find_named_codec(name, role);
}

static int64_t pts_us_to_codec(const AVCodecContext *ctx, int64_t pts_us) {
  if (pts_us < 0) return AV_NOPTS_VALUE;
  return av_rescale_q(pts_us, (AVRational) {1, 1000000}, ctx->time_base);
}

static int64_t codec_pts_to_us(const AVCodecContext *ctx, int64_t pts) {
  if (pts == AV_NOPTS_VALUE) return -1;
  return av_rescale_q(pts, ctx->time_base, (AVRational) {1, 1000000});
}

static int ensure_video_frame_data(AVFrame *frame, const gbmcd_frame_t *input) {
  enum AVPixelFormat dst_fmt;
  enum AVPixelFormat src_fmt;
  int src_width;
  int src_height;
  int required;
  uint8_t *src_data[4] = {0};
  int src_linesize[4] = {0};
  struct SwsContext *sws;
  if (!input->data || input->size == 0) return GBMCD_ERR_INVALID;
  dst_fmt = (enum AVPixelFormat) frame->format;
  src_fmt = pixel_format_from_name(input->format);
  src_width = input->width > 0 ? input->width : frame->width;
  src_height = input->height > 0 ? input->height : frame->height;
  if (src_width <= 0 || src_height <= 0 || frame->width <= 0 || frame->height <= 0) {
    return GBMCD_ERR_INVALID;
  }
  required = av_image_get_buffer_size(src_fmt, src_width, src_height, 1);
  if (required < 0) return GBMCD_ERR_BACKEND;
  if (!input->data || input->size < (size_t) required) return GBMCD_ERR_INVALID;
  if (src_width == frame->width && src_height == frame->height && src_fmt == dst_fmt) {
    return av_image_fill_arrays(frame->data,
                                frame->linesize,
                                input->data,
                                dst_fmt,
                                frame->width,
                                frame->height,
                                1) < 0
             ? GBMCD_ERR_BACKEND
             : GBMCD_OK;
  }

  if (av_image_fill_arrays(src_data,
                           src_linesize,
                           input->data,
                           src_fmt,
                           src_width,
                           src_height,
                           1) < 0) {
    return GBMCD_ERR_BACKEND;
  }
  if (av_frame_get_buffer(frame, 32) < 0) return GBMCD_ERR_NO_MEMORY;
  if (av_frame_make_writable(frame) < 0) return GBMCD_ERR_BACKEND;
  sws = sws_getContext(src_width,
                       src_height,
                       src_fmt,
                       frame->width,
                       frame->height,
                       dst_fmt,
                       SWS_BILINEAR,
                       NULL,
                       NULL,
                       NULL);
  if (!sws) return GBMCD_ERR_BACKEND;
  sws_scale(sws,
            (const uint8_t * const *) src_data,
            src_linesize,
            0,
            src_height,
            frame->data,
            frame->linesize);
  sws_freeContext(sws);
  return GBMCD_OK;
}

static int ensure_audio_frame_data(AVFrame *frame, const gbmcd_frame_t *input) {
  int bytes_per_sample;
  int planar;
  int channels;
  int samples;
  int plane_size;
  const uint8_t *src;
  if (!input->data || input->size == 0) return GBMCD_ERR_INVALID;
  bytes_per_sample = av_get_bytes_per_sample((enum AVSampleFormat) frame->format);
  if (bytes_per_sample <= 0) return GBMCD_ERR_BACKEND;
  planar = av_sample_fmt_is_planar((enum AVSampleFormat) frame->format);
  channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;
  samples = (int) (input->size / (size_t) (bytes_per_sample * channels));
  if (samples <= 0) return GBMCD_ERR_INVALID;
  frame->nb_samples = samples;
  if (av_frame_get_buffer(frame, 0) < 0) return GBMCD_ERR_NO_MEMORY;
  if (av_frame_make_writable(frame) < 0) return GBMCD_ERR_BACKEND;

  src = input->data;
  if (!planar) {
    size_t need = (size_t) samples * (size_t) channels * (size_t) bytes_per_sample;
    if (input->size < need) return GBMCD_ERR_INVALID;
    memcpy(frame->data[0], src, need);
    return GBMCD_OK;
  }

  plane_size = samples * bytes_per_sample;
  if (input->size < (size_t) plane_size * (size_t) channels) return GBMCD_ERR_INVALID;
  for (int ch = 0; ch < channels; ch++) {
    memcpy(frame->data[ch], src + (size_t) ch * (size_t) plane_size, (size_t) plane_size);
  }
  return GBMCD_OK;
}

static int frame_to_owned_video(const AVCodecContext *ctx,
                                const AVFrame *frame,
                                gbmcd_frame_t *out) {
  int size = av_image_get_buffer_size((enum AVPixelFormat) frame->format,
                                      frame->width,
                                      frame->height,
                                      1);
  uint8_t *buf;
  if (size < 0) return GBMCD_ERR_BACKEND;
  buf = (uint8_t *) malloc((size_t) size);
  if (!buf) return GBMCD_ERR_NO_MEMORY;
  if (av_image_copy_to_buffer(buf,
                              size,
                              (const uint8_t * const *) frame->data,
                              frame->linesize,
                              (enum AVPixelFormat) frame->format,
                              frame->width,
                              frame->height,
                              1) < 0) {
    free(buf);
    return GBMCD_ERR_BACKEND;
  }
  out->type = GBMCD_FRAME_RAW;
  out->pts_us = codec_pts_to_us(ctx, frame->pts);
  out->data = buf;
  out->size = (size_t) size;
  out->key_frame = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
  out->width = frame->width;
  out->height = frame->height;
  out->format = av_get_pix_fmt_name((enum AVPixelFormat) frame->format);
  out->flags = GBMCD_FRAME_FLAG_OWNED_DATA;
  return GBMCD_OK;
}

static int frame_to_owned_audio(const AVCodecContext *ctx,
                                const AVFrame *frame,
                                gbmcd_frame_t *out) {
  int channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;
  int size = av_samples_get_buffer_size(NULL,
                                        channels,
                                        frame->nb_samples,
                                        (enum AVSampleFormat) frame->format,
                                        1);
  uint8_t *buf;
  if (size < 0) return GBMCD_ERR_BACKEND;
  buf = (uint8_t *) malloc((size_t) size);
  if (!buf) return GBMCD_ERR_NO_MEMORY;
  memcpy(buf, frame->data[0], (size_t) size);
  out->type = GBMCD_FRAME_RAW;
  out->pts_us = codec_pts_to_us(ctx, frame->pts);
  out->data = buf;
  out->size = (size_t) size;
  out->sample_rate = frame->sample_rate;
  out->channels = channels;
  out->format = av_get_sample_fmt_name((enum AVSampleFormat) frame->format);
  out->flags = GBMCD_FRAME_FLAG_OWNED_DATA;
  return GBMCD_OK;
}

static int packet_to_owned_frame(const AVCodecContext *ctx,
                                 const AVPacket *packet,
                                 gbmcd_frame_t *out) {
  uint8_t *buf = NULL;
  if (packet->size > 0) {
    buf = (uint8_t *) malloc((size_t) packet->size);
    if (!buf) return GBMCD_ERR_NO_MEMORY;
    memcpy(buf, packet->data, (size_t) packet->size);
  }
  out->type = GBMCD_FRAME_PACKET;
  out->pts_us = codec_pts_to_us(ctx, packet->pts);
  out->duration_us = packet->duration > 0 ? codec_pts_to_us(ctx, packet->duration) : 0;
  out->data = buf;
  out->size = (size_t) packet->size;
  out->key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
  out->width = ctx->width;
  out->height = ctx->height;
  out->sample_rate = ctx->sample_rate;
  out->channels = ctx->ch_layout.nb_channels;
  out->format = ctx->codec ? ctx->codec->name : NULL;
  out->flags = GBMCD_FRAME_FLAG_OWNED_DATA;
  return GBMCD_OK;
}
#endif

static int ffmpeg_list_codecs(gbmcd_media_type_t media_type,
                              gbmcd_codec_role_t role,
                              gbmcd_codec_info_t *codecs,
                              size_t max_codecs,
                              size_t *count_out) {
  if (!count_out) return GBMCD_ERR_INVALID;
  *count_out = 0;
#if !GBMEDIA_WITH_FFMPEG
  (void) media_type;
  (void) role;
  (void) codecs;
  (void) max_codecs;
  return GBMCD_ERR_UNSUPPORTED;
#else
  void *opaque = NULL;
  const AVCodec *codec = NULL;
  while ((codec = av_codec_iterate(&opaque)) != NULL) {
    if (codec->type != to_av_media(media_type)) continue;
    if (role == GBMCD_CODEC_ENCODER && !av_codec_is_encoder(codec)) continue;
    if (role == GBMCD_CODEC_DECODER && !av_codec_is_decoder(codec)) continue;

    size_t index = *count_out;
    (*count_out)++;
    if (!codecs || index >= max_codecs) continue;

    memset(&codecs[index], 0, sizeof(codecs[index]));
    copy_text(codecs[index].backend, sizeof(codecs[index].backend), "ffmpeg");
    copy_text(codecs[index].name, sizeof(codecs[index].name), codec->name);
    copy_text(codecs[index].description, sizeof(codecs[index].description),
              codec->long_name ? codec->long_name : codec->name);
    codecs[index].media_type = media_type;
    codecs[index].role = role;
    codecs[index].hardware = 0;
  }
  return GBMCD_OK;
#endif
}

static int ffmpeg_open(const gbmcd_codec_config_t *config,
                       const gbmcd_codec_callbacks_t *callbacks,
                       gbmcd_codec_t **codec_out) {
  if (!config || !codec_out) return GBMCD_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  (void) callbacks;
  return GBMCD_ERR_UNSUPPORTED;
#else
  int rc;
  gbmcd_ffmpeg_codec_t *codec =
      (gbmcd_ffmpeg_codec_t *) calloc(1, sizeof(*codec));
  if (!codec) return GBMCD_ERR_NO_MEMORY;
  codec->base.backend = gbmcd_ffmpeg_backend();
  codec->config = *config;
  if (callbacks) codec->callbacks = *callbacks;

  if (config->codec_name && config->codec_name[0]) {
    codec->avcodec = find_project_codec(config->codec_name,
                                        config->media_type,
                                        config->role,
                                        config->prefer_hardware);
  } else if (config->media_type == GBMCD_MEDIA_VIDEO) {
    codec->avcodec = config->role == GBMCD_CODEC_ENCODER
                       ? avcodec_find_encoder(AV_CODEC_ID_H264)
                       : avcodec_find_decoder(AV_CODEC_ID_H264);
  } else {
    codec->avcodec = config->role == GBMCD_CODEC_ENCODER
                       ? avcodec_find_encoder(AV_CODEC_ID_AAC)
                       : avcodec_find_decoder(AV_CODEC_ID_AAC);
  }
  if (!codec->avcodec) {
    free(codec);
    return GBMCD_ERR_NOT_FOUND;
  }

  codec->ctx = avcodec_alloc_context3(codec->avcodec);
  codec->frame = av_frame_alloc();
  codec->packet = av_packet_alloc();
  if (!codec->ctx || !codec->frame || !codec->packet) {
    ffmpeg_close(&codec->base);
    return GBMCD_ERR_NO_MEMORY;
  }

  codec->ctx->bit_rate = (int64_t) config->bitrate_kbps * 1000;
  if (config->fps_num > 0 && config->fps_den > 0) {
    codec->ctx->time_base.num = config->fps_den;
    codec->ctx->time_base.den = config->fps_num;
    codec->ctx->framerate.num = config->fps_num;
    codec->ctx->framerate.den = config->fps_den;
  } else {
    codec->ctx->time_base = (AVRational) {1, 1000000};
  }

  if (config->media_type == GBMCD_MEDIA_VIDEO) {
    codec->ctx->width = config->width;
    codec->ctx->height = config->height;
    codec->ctx->pix_fmt = pixel_format_from_name(config->pixel_format);
    codec->ctx->gop_size = config->gop > 0 ? config->gop : 50;
    codec->ctx->keyint_min = codec->ctx->gop_size;
    codec->ctx->max_b_frames = 0;
    if (config->low_latency) {
      codec->ctx->thread_count = 1;
      codec->ctx->thread_type = 0;
      codec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    copy_text(codec->output_format, sizeof(codec->output_format),
              av_get_pix_fmt_name(codec->ctx->pix_fmt));
  } else {
    codec->ctx->sample_rate = config->sample_rate > 0 ? config->sample_rate : 8000;
    av_channel_layout_default(&codec->ctx->ch_layout,
                              config->channels > 0 ? config->channels : 1);
    codec->ctx->sample_fmt = sample_format_from_name(codec->avcodec,
                                                     config->sample_format);
    copy_text(codec->output_format, sizeof(codec->output_format),
              av_get_sample_fmt_name(codec->ctx->sample_fmt));
  }

  if (config->role == GBMCD_CODEC_ENCODER &&
      (codec->avcodec->id == AV_CODEC_ID_H264 || codec->avcodec->id == AV_CODEC_ID_HEVC) &&
      codec->ctx->priv_data) {
    av_opt_set(codec->ctx->priv_data, "preset", "veryfast", 0);
    if (config->low_latency) {
      av_opt_set(codec->ctx->priv_data, "tune", "zerolatency", 0);
      av_opt_set(codec->ctx->priv_data, "x265-params", "bframes=0:rc-lookahead=0:repeat-headers=1", 0);
    }
    if (codec->avcodec->id == AV_CODEC_ID_H264) {
      char x264_params[128];
      int keyint = codec->ctx->gop_size > 0 ? codec->ctx->gop_size : 50;
      snprintf(x264_params,
               sizeof(x264_params),
               "keyint=%d:min-keyint=%d:scenecut=0:repeat-headers=1",
               keyint,
               keyint);
      av_opt_set(codec->ctx->priv_data, "x264-params", x264_params, 0);
    }
    if (config->profile && config->profile[0]) {
      av_opt_set(codec->ctx->priv_data, "profile", config->profile, 0);
    }
    if (config->level && config->level[0]) {
      av_opt_set(codec->ctx->priv_data, "level", config->level, 0);
    }
  }

  rc = avcodec_open2(codec->ctx, codec->avcodec, NULL);
  if (rc < 0) {
    ffmpeg_close(&codec->base);
    return ffmpeg_result(rc);
  }

  codec->feedback.target_bitrate_kbps = config->bitrate_kbps;
  codec->feedback.min_bitrate_kbps = config->bitrate_kbps / 2;
  codec->feedback.max_bitrate_kbps = config->bitrate_kbps * 2;

  *codec_out = &codec->base;
  return GBMCD_OK;
#endif
}

static void ffmpeg_close(gbmcd_codec_t *codec) {
  if (!codec) return;
#if GBMEDIA_WITH_FFMPEG
  gbmcd_ffmpeg_codec_t *ff = (gbmcd_ffmpeg_codec_t *) codec;
  if (ff->ctx) avcodec_free_context(&ff->ctx);
  if (ff->frame) av_frame_free(&ff->frame);
  if (ff->packet) av_packet_free(&ff->packet);
#endif
  free(codec);
}

static int ffmpeg_send_frame(gbmcd_codec_t *codec, const gbmcd_frame_t *frame) {
  if (!codec || !frame) return GBMCD_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMCD_ERR_UNSUPPORTED;
#else
  int rc;
  gbmcd_ffmpeg_codec_t *ff = (gbmcd_ffmpeg_codec_t *) codec;
  if (ff->config.role == GBMCD_CODEC_DECODER) {
    if (frame->type != GBMCD_FRAME_PACKET) return GBMCD_ERR_INVALID;
    av_packet_unref(ff->packet);
    if (frame->data && frame->size > 0) {
      rc = av_new_packet(ff->packet, (int) frame->size);
      if (rc < 0) return ffmpeg_result(rc);
      memcpy(ff->packet->data, frame->data, frame->size);
    }
    ff->packet->pts = pts_us_to_codec(ff->ctx, frame->pts_us);
    ff->packet->duration = frame->duration_us > 0
                             ? pts_us_to_codec(ff->ctx, frame->duration_us)
                             : 0;
    rc = avcodec_send_packet(ff->ctx, frame->size > 0 ? ff->packet : NULL);
    av_packet_unref(ff->packet);
    return ffmpeg_result(rc);
  }

  if (frame->type != GBMCD_FRAME_RAW) return GBMCD_ERR_INVALID;
  av_frame_unref(ff->frame);
  ff->frame->pts = pts_us_to_codec(ff->ctx, frame->pts_us);
  if (ff->config.media_type == GBMCD_MEDIA_VIDEO) {
    ff->frame->format = ff->ctx->pix_fmt;
    ff->frame->width = ff->ctx->width;
    ff->frame->height = ff->ctx->height;
    if (frame->key_frame || ff->force_next_keyframe) {
      ff->frame->pict_type = AV_PICTURE_TYPE_I;
      ff->force_next_keyframe = 0;
    }
    rc = ensure_video_frame_data(ff->frame, frame);
  } else {
    ff->frame->format = ff->ctx->sample_fmt;
    ff->frame->sample_rate = frame->sample_rate > 0 ? frame->sample_rate : ff->ctx->sample_rate;
    av_channel_layout_copy(&ff->frame->ch_layout, &ff->ctx->ch_layout);
    rc = ensure_audio_frame_data(ff->frame, frame);
  }
  if (rc != GBMCD_OK) return rc;
  rc = avcodec_send_frame(ff->ctx, ff->frame);
  av_frame_unref(ff->frame);
  return ffmpeg_result(rc);
#endif
}

static int ffmpeg_request_keyframe(gbmcd_codec_t *codec) {
  if (!codec) return GBMCD_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMCD_ERR_UNSUPPORTED;
#else
  gbmcd_ffmpeg_codec_t *ff = (gbmcd_ffmpeg_codec_t *) codec;
  if (ff->config.role != GBMCD_CODEC_ENCODER ||
      ff->config.media_type != GBMCD_MEDIA_VIDEO) {
    return GBMCD_ERR_UNSUPPORTED;
  }
  ff->force_next_keyframe = 1;
  return GBMCD_OK;
#endif
}

static int ffmpeg_receive_frame(gbmcd_codec_t *codec, gbmcd_frame_t *frame_out) {
  if (!codec || !frame_out) return GBMCD_ERR_INVALID;
  memset(frame_out, 0, sizeof(*frame_out));
#if !GBMEDIA_WITH_FFMPEG
  return GBMCD_ERR_UNSUPPORTED;
#else
  int rc;
  gbmcd_ffmpeg_codec_t *ff = (gbmcd_ffmpeg_codec_t *) codec;
  if (ff->config.role == GBMCD_CODEC_ENCODER) {
    av_packet_unref(ff->packet);
    rc = avcodec_receive_packet(ff->ctx, ff->packet);
    if (rc < 0) return ffmpeg_result(rc);
    rc = packet_to_owned_frame(ff->ctx, ff->packet, frame_out);
    av_packet_unref(ff->packet);
  } else {
    av_frame_unref(ff->frame);
    rc = avcodec_receive_frame(ff->ctx, ff->frame);
    if (rc < 0) return ffmpeg_result(rc);
    if (ff->config.media_type == GBMCD_MEDIA_VIDEO) {
      rc = frame_to_owned_video(ff->ctx, ff->frame, frame_out);
    } else {
      rc = frame_to_owned_audio(ff->ctx, ff->frame, frame_out);
    }
    av_frame_unref(ff->frame);
  }
  if (rc == GBMCD_OK && ff->callbacks.on_frame) {
    int cb_rc = ff->callbacks.on_frame(ff->callbacks.user_data, frame_out);
    if (cb_rc != 0) return cb_rc;
  }
  return rc;
#endif
}

static int ffmpeg_set_bitrate(gbmcd_codec_t *codec, int bitrate_kbps) {
  if (!codec || bitrate_kbps <= 0) return GBMCD_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMCD_ERR_UNSUPPORTED;
#else
  gbmcd_ffmpeg_codec_t *ff = (gbmcd_ffmpeg_codec_t *) codec;
  ff->feedback.target_bitrate_kbps = bitrate_kbps;
  if (ff->ctx) ff->ctx->bit_rate = (int64_t) bitrate_kbps * 1000;
  return GBMCD_OK;
#endif
}

static int ffmpeg_poll_feedback(gbmcd_codec_t *codec,
                                gbmcd_rate_control_feedback_t *feedback_out) {
  if (!codec || !feedback_out) return GBMCD_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMCD_ERR_UNSUPPORTED;
#else
  gbmcd_ffmpeg_codec_t *ff = (gbmcd_ffmpeg_codec_t *) codec;
  *feedback_out = ff->feedback;
  if (ff->ctx && ff->ctx->bit_rate > 0) {
    feedback_out->target_bitrate_kbps = (int) (ff->ctx->bit_rate / 1000);
  }
  if (ff->callbacks.on_rate_feedback) {
    return ff->callbacks.on_rate_feedback(ff->callbacks.user_data, feedback_out);
  }
  return GBMCD_OK;
#endif
}

const gbmcd_codec_backend_t *gbmcd_ffmpeg_backend(void) {
  static const gbmcd_codec_backend_t backend = {
      "ffmpeg",
      "FFmpeg libavcodec backend",
      100,
      ffmpeg_probe,
      ffmpeg_list_codecs,
      ffmpeg_open,
      ffmpeg_close,
      ffmpeg_send_frame,
      ffmpeg_receive_frame,
      ffmpeg_request_keyframe,
      ffmpeg_set_bitrate,
      ffmpeg_poll_feedback,
  };
  return &backend;
}
