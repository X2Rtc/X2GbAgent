#include "capture_internal.h"
#include "gb_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef GBMEDIA_WITH_FFMPEG
#define GBMEDIA_WITH_FFMPEG 0
#endif

#if GBMEDIA_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#endif

typedef struct {
  gbmc_capture_t base;
  gbmc_capture_config_t config;
  gbmc_capture_callbacks_t callbacks;
#if GBMEDIA_WITH_FFMPEG
  AVFormatContext *video_ctx;
  AVFormatContext *audio_ctx;
  AVCodecContext *video_dec;
  AVFrame *video_frame;
  int video_stream;
  int audio_stream;
  gb_thread_t thread;
  int64_t next_video_pts_us;
#endif
  int started;
  int stop_requested;
} gbmc_ffmpeg_capture_t;

static int ffmpeg_stop(gbmc_capture_t *capture);

#if GBMEDIA_WITH_FFMPEG
static void ffmpeg_register_once(void) {
  static int registered;
  if (!registered) {
    avdevice_register_all();
    registered = 1;
  }
}

static const char *video_input_format_name(void) {
#if defined(_WIN32)
  return "dshow";
#elif defined(__APPLE__)
  return "avfoundation";
#else
  return "v4l2";
#endif
}

static const char *audio_input_format_name(void) {
#if defined(_WIN32)
  return "dshow";
#elif defined(__APPLE__)
  return "avfoundation";
#else
  return "alsa";
#endif
}

static int is_screen_device_id(const char *device_id) {
  return device_id != NULL && strcmp(device_id, "screen://desktop") == 0;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  if (!src) src = "";
  snprintf(dst, dst_size, "%s", src);
}

static int device_supports_media(const AVDeviceInfo *device, enum AVMediaType media_type) {
  if (!device) return 0;
  if (!device->media_types || device->nb_media_types <= 0) return 1;
  for (int i = 0; i < device->nb_media_types; i++) {
    if (device->media_types[i] == media_type) return 1;
  }
  return 0;
}

static void copy_device_id(char *dst,
                           size_t dst_size,
                           const char *device_name,
                           gbmc_media_type_t media_type) {
#if defined(_WIN32)
  const char *prefix = media_type == GBMC_MEDIA_VIDEO ? "video=" : "audio=";
  if (device_name && (strncmp(device_name, "video=", 6) == 0 ||
                      strncmp(device_name, "audio=", 6) == 0)) {
    copy_text(dst, dst_size, device_name);
  } else {
    snprintf(dst, dst_size, "%s%s", prefix, device_name ? device_name : "");
  }
#else
  copy_text(dst, dst_size, device_name);
#endif
}

static int ffmpeg_interrupt_cb(void *opaque) {
  const gbmc_ffmpeg_capture_t *capture = (const gbmc_ffmpeg_capture_t *) opaque;
  return capture != NULL && capture->stop_requested;
}

static int ffmpeg_result(int rc) {
  if (rc == AVERROR(EBUSY)) return GBMC_ERR_BUSY;
  if (rc == AVERROR(EACCES) || rc == AVERROR(EPERM)) return GBMC_ERR_PERMISSION;
  if (rc == AVERROR(ENOMEM)) return GBMC_ERR_NO_MEMORY;
  if (rc == AVERROR(ENODEV) || rc == AVERROR(ENOENT)) return GBMC_ERR_NOT_FOUND;
  return rc < 0 ? GBMC_ERR_BACKEND : GBMC_OK;
}

static int64_t pts_to_us(AVFormatContext *ctx, int stream_index, int64_t pts) {
  if (!ctx || stream_index < 0 || pts == AV_NOPTS_VALUE) return -1;
  return av_rescale_q(pts,
                      ctx->streams[stream_index]->time_base,
                      (AVRational) {1, 1000000});
}

static int open_video_decoder(gbmc_ffmpeg_capture_t *capture) {
  const AVCodec *decoder;
  AVCodecContext *dec;
  if (!capture || !capture->video_ctx || capture->video_stream < 0) return GBMC_ERR_INVALID;
  decoder = avcodec_find_decoder(capture->video_ctx->streams[capture->video_stream]->codecpar->codec_id);
  if (!decoder) return GBMC_ERR_UNSUPPORTED;
  dec = avcodec_alloc_context3(decoder);
  if (!dec) return GBMC_ERR_NO_MEMORY;
  if (avcodec_parameters_to_context(dec, capture->video_ctx->streams[capture->video_stream]->codecpar) < 0 ||
      avcodec_open2(dec, decoder, NULL) < 0) {
    avcodec_free_context(&dec);
    return GBMC_ERR_BACKEND;
  }
  capture->video_dec = dec;
  capture->video_frame = av_frame_alloc();
  if (!capture->video_frame) {
    avcodec_free_context(&capture->video_dec);
    return GBMC_ERR_NO_MEMORY;
  }
  return GBMC_OK;
}

static int emit_decoded_video(gbmc_ffmpeg_capture_t *capture, const AVPacket *packet) {
  int rc;
  if (!capture || !capture->video_dec || !capture->video_frame || !packet || !capture->callbacks.on_sample) {
    return GBMC_OK;
  }
  rc = avcodec_send_packet(capture->video_dec, packet);
  if (rc < 0 && rc != AVERROR(EAGAIN)) return GBMC_ERR_BACKEND;
  for (;;) {
    int size;
    uint8_t *buf;
    gbmc_sample_t sample;
    int out_width;
    int out_height;
    const char *out_format;
    rc = avcodec_receive_frame(capture->video_dec, capture->video_frame);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return GBMC_OK;
    if (rc < 0) return GBMC_ERR_BACKEND;
    out_width = capture->config.width > 0 ? capture->config.width : capture->video_frame->width;
    out_height = capture->config.height > 0 ? capture->config.height : capture->video_frame->height;
    if (out_width <= 0 || out_height <= 0) {
      av_frame_unref(capture->video_frame);
      return GBMC_ERR_BACKEND;
    }
    if (out_width & 1) out_width--;
    if (out_height & 1) out_height--;
    out_format = capture->config.width > 0 || capture->config.height > 0
                   ? "yuv420p"
                   : av_get_pix_fmt_name((enum AVPixelFormat) capture->video_frame->format);
    size = av_image_get_buffer_size(out_format && strcmp(out_format, "yuv420p") == 0
                                      ? AV_PIX_FMT_YUV420P
                                      : (enum AVPixelFormat) capture->video_frame->format,
                                    out_width,
                                    out_height,
                                    1);
    if (size <= 0) {
      av_frame_unref(capture->video_frame);
      return GBMC_ERR_BACKEND;
    }
    buf = (uint8_t *) malloc((size_t) size);
    if (!buf) {
      av_frame_unref(capture->video_frame);
      return GBMC_ERR_NO_MEMORY;
    }
    if (out_width != capture->video_frame->width ||
        out_height != capture->video_frame->height ||
        strcmp(out_format ? out_format : "", "yuv420p") == 0) {
      uint8_t *dst_data[4] = {0};
      int dst_linesize[4] = {0};
      struct SwsContext *sws;
      if (av_image_fill_arrays(dst_data, dst_linesize, buf, AV_PIX_FMT_YUV420P, out_width, out_height, 1) < 0) {
        free(buf);
        av_frame_unref(capture->video_frame);
        return GBMC_ERR_BACKEND;
      }
      sws = sws_getContext(capture->video_frame->width,
                           capture->video_frame->height,
                           (enum AVPixelFormat) capture->video_frame->format,
                           out_width,
                           out_height,
                           AV_PIX_FMT_YUV420P,
                           SWS_BILINEAR,
                           NULL,
                           NULL,
                           NULL);
      if (!sws) {
        free(buf);
        av_frame_unref(capture->video_frame);
        return GBMC_ERR_BACKEND;
      }
      sws_scale(sws,
                (const uint8_t * const *) capture->video_frame->data,
                capture->video_frame->linesize,
                0,
                capture->video_frame->height,
                dst_data,
                dst_linesize);
      sws_freeContext(sws);
      out_format = "yuv420p";
    } else {
      if (av_image_copy_to_buffer(buf,
                                  size,
                                  (const uint8_t * const *) capture->video_frame->data,
                                  capture->video_frame->linesize,
                                  (enum AVPixelFormat) capture->video_frame->format,
                                  capture->video_frame->width,
                                  capture->video_frame->height,
                                  1) < 0) {
        free(buf);
        av_frame_unref(capture->video_frame);
        return GBMC_ERR_BACKEND;
      }
    }
    memset(&sample, 0, sizeof(sample));
    sample.type = GBMC_SAMPLE_VIDEO_RAW;
    sample.pts_us = pts_to_us(capture->video_ctx, capture->video_stream, capture->video_frame->pts);
    sample.duration_us = packet->duration > 0 ? pts_to_us(capture->video_ctx, capture->video_stream, packet->duration) : 0;
    sample.data = buf;
    sample.size = (size_t) size;
    sample.width = out_width;
    sample.height = out_height;
    sample.format = out_format;
    sample.flags = GBMC_SAMPLE_FLAG_TRANSIENT_DATA;
    rc = capture->callbacks.on_sample(capture->callbacks.user_data, &sample);
    free(buf);
    av_frame_unref(capture->video_frame);
    if (rc != 0) return rc;
  }
}

static int read_one(gbmc_ffmpeg_capture_t *capture,
                    AVFormatContext *ctx,
                    int stream_index,
                    gbmc_sample_type_t sample_type) {
  AVPacket packet;
  int rc;
  if (!capture || !ctx || stream_index < 0) return GBMC_OK;
  av_init_packet(&packet);
  rc = av_read_frame(ctx, &packet);
  if (rc == AVERROR(EAGAIN)) return GBMC_OK;
  if (rc < 0) return GBMC_ERR_BACKEND;
  if (packet.stream_index == stream_index && capture->callbacks.on_sample) {
    if (sample_type == GBMC_SAMPLE_VIDEO_PACKET && capture->video_dec) {
      rc = emit_decoded_video(capture, &packet);
      if (rc != 0) capture->stop_requested = 1;
      av_packet_unref(&packet);
      return GBMC_OK;
    }
    AVStream *stream = ctx->streams[stream_index];
    gbmc_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.type = sample_type;
    sample.pts_us = pts_to_us(ctx, stream_index, packet.pts);
    sample.duration_us = packet.duration > 0
                           ? pts_to_us(ctx, stream_index, packet.duration)
                           : 0;
    sample.data = packet.data;
    sample.size = (size_t) packet.size;
    sample.flags = GBMC_SAMPLE_FLAG_TRANSIENT_DATA;
    if (stream->codecpar) {
      sample.width = stream->codecpar->width;
      sample.height = stream->codecpar->height;
      sample.sample_rate = stream->codecpar->sample_rate;
      sample.channels = stream->codecpar->ch_layout.nb_channels;
      sample.format = avcodec_get_name(stream->codecpar->codec_id);
    }
    rc = capture->callbacks.on_sample(capture->callbacks.user_data, &sample);
    if (rc != 0) capture->stop_requested = 1;
  }
  av_packet_unref(&packet);
  return GBMC_OK;
}

static void *capture_thread(void *arg) {
  gbmc_ffmpeg_capture_t *capture = (gbmc_ffmpeg_capture_t *) arg;
  int fps_num = capture->config.fps_num > 0 ? capture->config.fps_num : 25;
  int fps_den = capture->config.fps_den > 0 ? capture->config.fps_den : 1;
  int frame_ms = (int) ((int64_t) fps_den * 1000LL / fps_num);
  if (frame_ms <= 0) frame_ms = 40;
  while (capture && !capture->stop_requested) {
    int did_work = 0;
    if (capture->video_ctx) {
      int rc = read_one(capture,
                        capture->video_ctx,
                        capture->video_stream,
                        GBMC_SAMPLE_VIDEO_PACKET);
      did_work = 1;
      if (rc != GBMC_OK) {
        avformat_close_input(&capture->video_ctx);
        capture->video_stream = -1;
      }
    }
    if (capture->audio_ctx) {
      (void) read_one(capture,
                      capture->audio_ctx,
                      capture->audio_stream,
                      GBMC_SAMPLE_AUDIO_PACKET);
      did_work = 1;
    }
    if (!did_work) {
      gb_sleep_ms(5);
    }
  }
  return NULL;
}
#endif

static int ffmpeg_probe(void) {
#if GBMEDIA_WITH_FFMPEG
  ffmpeg_register_once();
  return GBMC_OK;
#else
  return GBMC_ERR_UNSUPPORTED;
#endif
}

static int ffmpeg_list_devices(gbmc_media_type_t media_type,
                               gbmc_device_info_t *devices,
                               size_t max_devices,
                               size_t *count_out) {
  if (!count_out) return GBMC_ERR_INVALID;
  *count_out = 0;
#if !GBMEDIA_WITH_FFMPEG
  (void) media_type;
  (void) devices;
  (void) max_devices;
  return GBMC_ERR_UNSUPPORTED;
#else
  ffmpeg_register_once();
  const char *fmt_name = media_type == GBMC_MEDIA_VIDEO
                           ? video_input_format_name()
                           : audio_input_format_name();
  const AVInputFormat *fmt = av_find_input_format(fmt_name);
  if (!fmt) return GBMC_ERR_UNSUPPORTED;

  AVDeviceInfoList *list = NULL;
  int rc = avdevice_list_input_sources(fmt, NULL, NULL, &list);
  if (rc < 0 || !list) return GBMC_ERR_BACKEND;

  enum AVMediaType av_media = media_type == GBMC_MEDIA_VIDEO
                                ? AVMEDIA_TYPE_VIDEO
                                : AVMEDIA_TYPE_AUDIO;
  size_t total = 0;
  for (int i = 0; i < list->nb_devices; i++) {
    AVDeviceInfo *device = list->devices[i];
    if (!device_supports_media(device, av_media)) continue;
    if (devices && total < max_devices) {
      memset(&devices[total], 0, sizeof(devices[total]));
      copy_text(devices[total].backend, sizeof(devices[total].backend), "ffmpeg");
      copy_device_id(devices[total].id,
                     sizeof(devices[total].id),
                     device->device_name,
                     media_type);
      copy_text(devices[total].name,
                sizeof(devices[total].name),
                device->device_description);
      devices[total].media_type = media_type;
    }
    total++;
  }
  *count_out = total;
  avdevice_free_list_devices(&list);
  return GBMC_OK;
#endif
}

#if GBMEDIA_WITH_FFMPEG
static int ffmpeg_open_one(gbmc_ffmpeg_capture_t *capture,
                           const char *device_id,
                           gbmc_media_type_t media_type,
                           AVFormatContext **ctx_out,
                           int *stream_out) {
  if (!device_id || !device_id[0]) return GBMC_OK;
  const char *fmt_name = media_type == GBMC_MEDIA_VIDEO
                           ? video_input_format_name()
                           : audio_input_format_name();
  const char *input_id = device_id;
  int is_screen = media_type == GBMC_MEDIA_VIDEO && is_screen_device_id(device_id);
#if defined(_WIN32)
  if (is_screen) {
    fmt_name = "gdigrab";
    input_id = "desktop";
  }
#else
  if (is_screen) return GBMC_ERR_UNSUPPORTED;
#endif
  const AVInputFormat *fmt = av_find_input_format(fmt_name);
  if (!fmt) return GBMC_ERR_UNSUPPORTED;

  AVFormatContext *ctx = avformat_alloc_context();
  AVDictionary *opts = NULL;
  if (!ctx) return GBMC_ERR_NO_MEMORY;
  ctx->interrupt_callback.callback = ffmpeg_interrupt_cb;
  ctx->interrupt_callback.opaque = capture;
  av_dict_set(&opts, "fflags", "nobuffer", 0);
  av_dict_set(&opts, "flags", "low_delay", 0);
  av_dict_set(&opts, "rtbufsize", "1048576", 0);
  av_dict_set(&opts, "probesize", "32768", 0);
  av_dict_set(&opts, "analyzeduration", "0", 0);
  if (media_type == GBMC_MEDIA_VIDEO && capture->config.fps_num > 0) {
    char fps[16];
    snprintf(fps, sizeof(fps), "%d", capture->config.fps_num / (capture->config.fps_den > 0 ? capture->config.fps_den : 1));
    av_dict_set(&opts, "framerate", fps, 0);
  }
  if (is_screen) av_dict_set(&opts, "draw_mouse", "1", 0);
  int rc = avformat_open_input(&ctx, input_id, fmt, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    avformat_free_context(ctx);
    return ffmpeg_result(rc);
  }
  (void) avformat_find_stream_info(ctx, NULL);
  rc = av_find_best_stream(ctx,
                           media_type == GBMC_MEDIA_VIDEO ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO,
                           -1,
                           -1,
                           NULL,
                           0);
  if (rc < 0) {
    avformat_close_input(&ctx);
    return ffmpeg_result(rc);
  }
  *ctx_out = ctx;
  if (stream_out) *stream_out = rc;
  return GBMC_OK;
}
#endif

static int ffmpeg_open(const gbmc_capture_config_t *config,
                       const gbmc_capture_callbacks_t *callbacks,
                       gbmc_capture_t **capture_out) {
  if (!config || !capture_out) return GBMC_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  (void) callbacks;
  return GBMC_ERR_UNSUPPORTED;
#else
  ffmpeg_register_once();
  gbmc_ffmpeg_capture_t *capture =
      (gbmc_ffmpeg_capture_t *) calloc(1, sizeof(*capture));
  if (!capture) return GBMC_ERR_NO_MEMORY;
  capture->base.backend = gbmc_ffmpeg_backend();
  capture->config = *config;
  if (callbacks) capture->callbacks = *callbacks;

  capture->video_stream = -1;
  capture->audio_stream = -1;
  int rc = ffmpeg_open_one(capture,
                           config->video_device_id,
                           GBMC_MEDIA_VIDEO,
                           &capture->video_ctx,
                           &capture->video_stream);
  if (rc != GBMC_OK && config->video_device_id && config->video_device_id[0]) {
    free(capture);
    return rc;
  }
  if (rc == GBMC_OK) {
    if (capture->video_ctx && capture->video_stream >= 0 && open_video_decoder(capture) != GBMC_OK) {
      rc = GBMC_ERR_BACKEND;
    }
  }
  if (rc == GBMC_OK) {
    int audio_rc = ffmpeg_open_one(capture,
                                   config->audio_device_id,
                                   GBMC_MEDIA_AUDIO,
                                   &capture->audio_ctx,
                                   &capture->audio_stream);
    if (audio_rc != GBMC_OK && !capture->video_ctx) rc = audio_rc;
  }
  if (rc != GBMC_OK) {
    if (capture->video_ctx) avformat_close_input(&capture->video_ctx);
    free(capture);
    return rc;
  }

  *capture_out = &capture->base;
  return GBMC_OK;
#endif
}

static void ffmpeg_close(gbmc_capture_t *capture) {
  if (!capture) return;
#if GBMEDIA_WITH_FFMPEG
  gbmc_ffmpeg_capture_t *ff = (gbmc_ffmpeg_capture_t *) capture;
  (void) ffmpeg_stop(capture);
  if (ff->video_ctx) avformat_close_input(&ff->video_ctx);
  if (ff->audio_ctx) avformat_close_input(&ff->audio_ctx);
  if (ff->video_frame) av_frame_free(&ff->video_frame);
  if (ff->video_dec) avcodec_free_context(&ff->video_dec);
#endif
  free(capture);
}

static int ffmpeg_start(gbmc_capture_t *capture) {
  if (!capture) return GBMC_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMC_ERR_UNSUPPORTED;
#else
  gbmc_ffmpeg_capture_t *ff = (gbmc_ffmpeg_capture_t *) capture;
  if (ff->started) return GBMC_OK;
  if (!ff->video_ctx && !ff->audio_ctx) return GBMC_ERR_INVALID;
  ff->stop_requested = 0;
  if (gb_thread_create(&ff->thread, capture_thread, ff) != 0) return GBMC_ERR_BACKEND;
  ff->started = 1;
  return GBMC_OK;
#endif
}

static int ffmpeg_stop(gbmc_capture_t *capture) {
  if (!capture) return GBMC_ERR_INVALID;
#if !GBMEDIA_WITH_FFMPEG
  return GBMC_OK;
#else
  gbmc_ffmpeg_capture_t *ff = (gbmc_ffmpeg_capture_t *) capture;
  if (!ff->started) return GBMC_OK;
  ff->stop_requested = 1;
  if (!gb_thread_is_current(ff->thread)) gb_thread_join(ff->thread);
  memset(&ff->thread, 0, sizeof(ff->thread));
  ff->started = 0;
  return GBMC_OK;
#endif
}

const gbmc_capture_backend_t *gbmc_ffmpeg_backend(void) {
  static const gbmc_capture_backend_t backend = {
      "ffmpeg",
      "FFmpeg libavdevice capture backend",
      100,
      ffmpeg_probe,
      ffmpeg_list_devices,
      ffmpeg_open,
      ffmpeg_close,
      ffmpeg_start,
      ffmpeg_stop,
  };
  return &backend;
}
