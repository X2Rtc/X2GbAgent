#include "gb_media_file_source.h"

#include "gb_platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef GBMEDIA_WITH_FFMPEG
#define GBMEDIA_WITH_FFMPEG 0
#endif

#if GBMEDIA_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#endif

struct gbmf_file_source {
  gbmf_file_source_config_t config;
  gbmf_file_source_callbacks_t callbacks;
#if GBMEDIA_WITH_FFMPEG
  AVFormatContext *fmt;
  AVCodecContext *video_dec;
  AVCodecContext *audio_dec;
  AVPacket *packet;
  AVFrame *frame;
  int video_stream;
  int audio_stream;
  int64_t input_pts_offset_us;
  int64_t next_video_pts_us;
#endif
};

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

const char *gbmf_result_name(int result) {
  switch (result) {
    case GBMF_OK: return "GBMF_OK";
    case GBMF_ERR_INVALID: return "GBMF_ERR_INVALID";
    case GBMF_ERR_NOT_FOUND: return "GBMF_ERR_NOT_FOUND";
    case GBMF_ERR_NO_MEMORY: return "GBMF_ERR_NO_MEMORY";
    case GBMF_ERR_BACKEND: return "GBMF_ERR_BACKEND";
    case GBMF_ERR_UNSUPPORTED: return "GBMF_ERR_UNSUPPORTED";
    case GBMF_ERR_EOF: return "GBMF_ERR_EOF";
    default: return "GBMF_ERR_UNKNOWN";
  }
}

#if GBMEDIA_WITH_FFMPEG
static int target_fps_num(const gbmf_file_source_t *source) {
  return source->config.fps_num > 0 ? source->config.fps_num : 25;
}

static int target_fps_den(const gbmf_file_source_t *source) {
  return source->config.fps_den > 0 ? source->config.fps_den : 1;
}

static int64_t target_duration_us(const gbmf_file_source_t *source) {
  return (int64_t) target_fps_den(source) * 1000000LL / target_fps_num(source);
}

static int64_t pts_to_us(const AVStream *stream, int64_t pts) {
  if (!stream || pts == AV_NOPTS_VALUE) return -1;
  return av_rescale_q(pts, stream->time_base, (AVRational) {1, 1000000});
}

static int open_decoder(AVFormatContext *fmt, int stream_index, AVCodecContext **ctx_out) {
  const AVCodec *decoder;
  AVCodecContext *ctx;
  if (!fmt || stream_index < 0 || !ctx_out) return GBMF_ERR_INVALID;
  decoder = avcodec_find_decoder(fmt->streams[stream_index]->codecpar->codec_id);
  if (!decoder) return GBMF_ERR_UNSUPPORTED;
  ctx = avcodec_alloc_context3(decoder);
  if (!ctx) return GBMF_ERR_NO_MEMORY;
  if (avcodec_parameters_to_context(ctx, fmt->streams[stream_index]->codecpar) < 0 ||
      avcodec_open2(ctx, decoder, NULL) < 0) {
    avcodec_free_context(&ctx);
    return GBMF_ERR_BACKEND;
  }
  *ctx_out = ctx;
  return GBMF_OK;
}

static int emit_video_frame(gbmf_file_source_t *source, AVFrame *frame) {
  gbmf_frame_t out;
  int size;
  uint8_t *buf;
  int cb_rc;
  int64_t frame_pts_us;
  int64_t paced_pts_us;
  int64_t duration_us = target_duration_us(source);
  if (!source->callbacks.on_frame) return GBMF_OK;
  frame_pts_us = pts_to_us(source->fmt->streams[source->video_stream], frame->pts);
  paced_pts_us = frame_pts_us >= 0 ? frame_pts_us + source->input_pts_offset_us : -1;
  if (source->config.fps_num > 0 && paced_pts_us >= 0) {
    if (paced_pts_us + duration_us / 2 < source->next_video_pts_us) return GBMF_OK;
  }
  size = av_image_get_buffer_size((enum AVPixelFormat) frame->format,
                                  frame->width,
                                  frame->height,
                                  1);
  if (size <= 0) return GBMF_ERR_BACKEND;
  buf = (uint8_t *) malloc((size_t) size);
  if (!buf) return GBMF_ERR_NO_MEMORY;
  if (av_image_copy_to_buffer(buf,
                              size,
                              (const uint8_t * const *) frame->data,
                              frame->linesize,
                              (enum AVPixelFormat) frame->format,
                              frame->width,
                              frame->height,
                              1) < 0) {
    free(buf);
    return GBMF_ERR_BACKEND;
  }
  memset(&out, 0, sizeof(out));
  out.type = GBMF_FRAME_VIDEO_RAW;
  out.pts_us = source->config.fps_num > 0 ? source->next_video_pts_us : paced_pts_us;
  out.duration_us = source->config.fps_num > 0 ? duration_us : 0;
  out.data = buf;
  out.size = (size_t) size;
  out.width = frame->width;
  out.height = frame->height;
  out.format = av_get_pix_fmt_name((enum AVPixelFormat) frame->format);
  out.flags = GBMF_FRAME_FLAG_TRANSIENT_DATA;
  if (source->config.fps_num > 0) source->next_video_pts_us += duration_us;
  cb_rc = source->callbacks.on_frame(source->callbacks.user_data, &out);
  if (source->config.realtime && source->config.fps_num > 0) gb_sleep_ms((int) (duration_us / 1000));
  free(buf);
  return cb_rc;
}

static int emit_audio_frame(gbmf_file_source_t *source, AVFrame *frame) {
  gbmf_frame_t out;
  int channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;
  int size;
  uint8_t *buf;
  int cb_rc;
  if (!source->callbacks.on_frame) return GBMF_OK;
  size = av_samples_get_buffer_size(NULL,
                                    channels,
                                    frame->nb_samples,
                                    (enum AVSampleFormat) frame->format,
                                    1);
  if (size <= 0) return GBMF_ERR_BACKEND;
  buf = (uint8_t *) malloc((size_t) size);
  if (!buf) return GBMF_ERR_NO_MEMORY;
  if (av_sample_fmt_is_planar((enum AVSampleFormat) frame->format)) {
    int plane_size = size / channels;
    for (int ch = 0; ch < channels; ch++) {
      memcpy(buf + (size_t) ch * (size_t) plane_size, frame->data[ch], (size_t) plane_size);
    }
  } else {
    memcpy(buf, frame->data[0], (size_t) size);
  }
  memset(&out, 0, sizeof(out));
  out.type = GBMF_FRAME_AUDIO_RAW;
  out.pts_us = pts_to_us(source->fmt->streams[source->audio_stream], frame->pts);
  if (out.pts_us >= 0) out.pts_us += source->input_pts_offset_us;
  out.data = buf;
  out.size = (size_t) size;
  out.sample_rate = frame->sample_rate;
  out.channels = channels;
  out.format = av_get_sample_fmt_name((enum AVSampleFormat) frame->format);
  out.flags = GBMF_FRAME_FLAG_TRANSIENT_DATA;
  cb_rc = source->callbacks.on_frame(source->callbacks.user_data, &out);
  free(buf);
  return cb_rc;
}
#endif

int gbmf_probe_file(const char *path, gbmf_file_info_t *info_out) {
  if (!path || !path[0] || !info_out) return GBMF_ERR_INVALID;
  memset(info_out, 0, sizeof(*info_out));
#if !GBMEDIA_WITH_FFMPEG
  return GBMF_ERR_UNSUPPORTED;
#else
  AVFormatContext *fmt = NULL;
  int rc = GBMF_ERR_BACKEND;
  if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return GBMF_ERR_NOT_FOUND;
  if (avformat_find_stream_info(fmt, NULL) < 0) goto done;

  int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  int audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (video >= 0) {
    AVStream *stream = fmt->streams[video];
    info_out->has_video = 1;
    info_out->width = stream->codecpar->width;
    info_out->height = stream->codecpar->height;
    info_out->fps_num = stream->avg_frame_rate.num;
    info_out->fps_den = stream->avg_frame_rate.den;
    copy_text(info_out->video_codec, sizeof(info_out->video_codec),
              avcodec_get_name(stream->codecpar->codec_id));
  }
  if (audio >= 0) {
    AVStream *stream = fmt->streams[audio];
    info_out->has_audio = 1;
    info_out->sample_rate = stream->codecpar->sample_rate;
    info_out->channels = stream->codecpar->ch_layout.nb_channels;
    copy_text(info_out->audio_codec, sizeof(info_out->audio_codec),
              avcodec_get_name(stream->codecpar->codec_id));
  }
  rc = info_out->has_video || info_out->has_audio ? GBMF_OK : GBMF_ERR_UNSUPPORTED;
done:
  avformat_close_input(&fmt);
  return rc;
#endif
}

int gbmf_open(const gbmf_file_source_config_t *config,
              const gbmf_file_source_callbacks_t *callbacks,
              gbmf_file_source_t **source_out) {
  if (!config || !config->path || !config->path[0] || !source_out) return GBMF_ERR_INVALID;
  *source_out = NULL;
#if !GBMEDIA_WITH_FFMPEG
  (void) callbacks;
  return GBMF_ERR_UNSUPPORTED;
#else
  gbmf_file_source_t *source = (gbmf_file_source_t *) calloc(1, sizeof(*source));
  if (!source) return GBMF_ERR_NO_MEMORY;
  source->config = *config;
  if (callbacks) source->callbacks = *callbacks;
  source->video_stream = -1;
  source->audio_stream = -1;
  source->packet = av_packet_alloc();
  source->frame = av_frame_alloc();
  if (!source->packet || !source->frame) {
    gbmf_close(&source);
    return GBMF_ERR_NO_MEMORY;
  }
  if (avformat_open_input(&source->fmt, config->path, NULL, NULL) < 0) {
    gbmf_close(&source);
    return GBMF_ERR_NOT_FOUND;
  }
  if (avformat_find_stream_info(source->fmt, NULL) < 0) {
    gbmf_close(&source);
    return GBMF_ERR_BACKEND;
  }
  source->video_stream = av_find_best_stream(source->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  source->audio_stream = av_find_best_stream(source->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (source->video_stream < 0 && source->audio_stream < 0) {
    gbmf_close(&source);
    return GBMF_ERR_UNSUPPORTED;
  }
  if (source->video_stream >= 0 &&
      open_decoder(source->fmt, source->video_stream, &source->video_dec) != GBMF_OK) {
    gbmf_close(&source);
    return GBMF_ERR_BACKEND;
  }
  if (source->audio_stream >= 0 &&
      open_decoder(source->fmt, source->audio_stream, &source->audio_dec) != GBMF_OK) {
    source->audio_stream = -1;
  }
  *source_out = source;
  return GBMF_OK;
#endif
}

void gbmf_close(gbmf_file_source_t **source) {
  if (!source || !*source) return;
#if GBMEDIA_WITH_FFMPEG
  if ((*source)->packet) av_packet_free(&(*source)->packet);
  if ((*source)->frame) av_frame_free(&(*source)->frame);
  if ((*source)->video_dec) avcodec_free_context(&(*source)->video_dec);
  if ((*source)->audio_dec) avcodec_free_context(&(*source)->audio_dec);
  if ((*source)->fmt) avformat_close_input(&(*source)->fmt);
#endif
  free(*source);
  *source = NULL;
}

int gbmf_read(gbmf_file_source_t *source) {
  if (!source) return GBMF_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMF_ERR_UNSUPPORTED;
#else
  for (;;) {
    AVCodecContext *dec = NULL;
    int stream_index;
    int rc = av_read_frame(source->fmt, source->packet);
    if (rc < 0) {
      if (source->config.loop) {
        if (av_seek_frame(source->fmt, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
          return GBMF_ERR_EOF;
        }
        if (source->video_dec) avcodec_flush_buffers(source->video_dec);
        if (source->audio_dec) avcodec_flush_buffers(source->audio_dec);
        source->input_pts_offset_us = source->next_video_pts_us;
        continue;
      }
      return GBMF_ERR_EOF;
    }
    stream_index = source->packet->stream_index;
    if (stream_index == source->video_stream) dec = source->video_dec;
    else if (stream_index == source->audio_stream) dec = source->audio_dec;
    if (!dec) {
      av_packet_unref(source->packet);
      continue;
    }
    if (avcodec_send_packet(dec, source->packet) < 0) {
      av_packet_unref(source->packet);
      if (stream_index == source->video_stream) {
        return GBMF_ERR_BACKEND;
      }
      return GBMF_ERR_BACKEND;
    }
    av_packet_unref(source->packet);
    rc = avcodec_receive_frame(dec, source->frame);
    if (rc == AVERROR(EAGAIN)) continue;
    if (rc < 0) {
      if (stream_index == source->video_stream) {
        return GBMF_ERR_BACKEND;
      }
      return GBMF_ERR_BACKEND;
    }
    rc = stream_index == source->video_stream
           ? emit_video_frame(source, source->frame)
           : emit_audio_frame(source, source->frame);
    av_frame_unref(source->frame);
    return rc == 0 ? GBMF_OK : rc;
  }
#endif
}
