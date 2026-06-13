#include "preview_jpeg.h"
#include "source_manager.h"

#include <stdlib.h>
#include <string.h>

#ifndef GBMEDIA_WITH_FFMPEG
#define GBMEDIA_WITH_FFMPEG 0
#endif

#if GBMEDIA_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

#if GBMEDIA_WITH_FFMPEG
static const uint8_t *glyph5x7(char ch) {
  static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
  static const uint8_t n[7] = {17, 25, 21, 19, 17, 17, 17};
  static const uint8_t o[7] = {14, 17, 17, 17, 17, 17, 14};
  static const uint8_t s[7] = {15, 16, 16, 14, 1, 1, 30};
  static const uint8_t i[7] = {14, 4, 4, 4, 4, 4, 14};
  static const uint8_t g[7] = {14, 17, 16, 23, 17, 17, 15};
  static const uint8_t a[7] = {14, 17, 17, 31, 17, 17, 17};
  static const uint8_t l[7] = {16, 16, 16, 16, 16, 16, 31};
  static const uint8_t u[7] = {17, 17, 17, 17, 17, 17, 14};
  static const uint8_t r[7] = {30, 17, 17, 30, 20, 18, 17};
  static const uint8_t c[7] = {14, 17, 16, 16, 16, 17, 14};
  static const uint8_t e[7] = {31, 16, 16, 30, 16, 16, 31};
  switch (ch) {
    case 'N': return n;
    case 'O': return o;
    case 'S': return s;
    case 'I': return i;
    case 'G': return g;
    case 'A': return a;
    case 'L': return l;
    case 'U': return u;
    case 'R': return r;
    case 'C': return c;
    case 'E': return e;
    default: return space;
  }
}

static void draw_text_y(AVFrame *frame, const char *text, int scale) {
  int len = (int) strlen(text);
  int glyph_w = 5 * scale;
  int glyph_h = 7 * scale;
  int gap = scale;
  int x0 = (frame->width - (len * glyph_w + (len - 1) * gap)) / 2;
  int y0 = (frame->height - glyph_h) / 2;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  for (int c = 0; c < len; c++) {
    const uint8_t *glyph = glyph5x7(text[c]);
    int gx = x0 + c * (glyph_w + gap);
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 5; col++) {
        if ((glyph[row] & (1 << (4 - col))) == 0) continue;
        for (int sy = 0; sy < scale; sy++) {
          int y = y0 + row * scale + sy;
          if (y < 0 || y >= frame->height) continue;
          for (int sx = 0; sx < scale; sx++) {
            int x = gx + col * scale + sx;
            if (x >= 0 && x < frame->width) frame->data[0][y * frame->linesize[0] + x] = 230;
          }
        }
      }
    }
  }
}

static int encode_jpeg(const AVFrame *src,
                       int width,
                       int height,
                       uint8_t **jpeg_out,
                       size_t *jpeg_size_out) {
  const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  AVCodecContext *ctx = NULL;
  AVFrame *frame = NULL;
  AVPacket *packet = NULL;
  struct SwsContext *sws = NULL;
  int rc = -1;

  if (!encoder) return -1;
  ctx = avcodec_alloc_context3(encoder);
  frame = av_frame_alloc();
  packet = av_packet_alloc();
  if (!ctx || !frame || !packet) goto done;

  ctx->width = width;
  ctx->height = height;
  ctx->time_base = (AVRational) {1, 5};
  ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
  ctx->color_range = AVCOL_RANGE_JPEG;
  if (avcodec_open2(ctx, encoder, NULL) < 0) goto done;

  frame->format = ctx->pix_fmt;
  frame->width = width;
  frame->height = height;
  frame->color_range = AVCOL_RANGE_JPEG;
  if (av_frame_get_buffer(frame, 32) < 0) goto done;
  if (av_frame_make_writable(frame) < 0) goto done;

  sws = sws_getContext(src->width,
                       src->height,
                       (enum AVPixelFormat) src->format,
                       width,
                       height,
                       ctx->pix_fmt,
                       SWS_BILINEAR,
                       NULL,
                       NULL,
                       NULL);
  if (!sws) goto done;
  sws_scale(sws,
            (const uint8_t * const *) src->data,
            src->linesize,
            0,
            src->height,
            frame->data,
            frame->linesize);

  if (avcodec_send_frame(ctx, frame) < 0) goto done;
  if (avcodec_receive_packet(ctx, packet) < 0) goto done;
  *jpeg_out = (uint8_t *) malloc((size_t) packet->size);
  if (!*jpeg_out) goto done;
  memcpy(*jpeg_out, packet->data, (size_t) packet->size);
  *jpeg_size_out = (size_t) packet->size;
  rc = 0;

done:
  if (packet) av_packet_free(&packet);
  if (frame) av_frame_free(&frame);
  if (ctx) avcodec_free_context(&ctx);
  if (sws) sws_freeContext(sws);
  return rc;
}
#endif

int gb_preview_jpeg_make(const char *source,
                         int is_device,
                         int width,
                         int height,
                         uint8_t **jpeg_out,
                         size_t *jpeg_size_out) {
  if (!source || !source[0] || !jpeg_out || !jpeg_size_out || width <= 0 || height <= 0) {
    return -1;
  }
  *jpeg_out = NULL;
  *jpeg_size_out = 0;

#if !GBMEDIA_WITH_FFMPEG
  (void) is_device;
  return -1;
#else
  if (is_device) return -1;
  AVFormatContext *fmt = NULL;
  AVCodecContext *dec = NULL;
  const AVCodec *decoder = NULL;
  AVPacket *packet = NULL;
  AVFrame *frame = NULL;
  int video_stream;
  int rc = -1;

  if (avformat_open_input(&fmt, source, NULL, NULL) < 0) goto done;
  if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
  video_stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (video_stream < 0 || !decoder) goto done;

  dec = avcodec_alloc_context3(decoder);
  if (!dec) goto done;
  if (avcodec_parameters_to_context(dec, fmt->streams[video_stream]->codecpar) < 0) goto done;
  if (avcodec_open2(dec, decoder, NULL) < 0) goto done;

  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (!packet || !frame) goto done;

  for (int i = 0; i < 120 && av_read_frame(fmt, packet) >= 0; i++) {
    if (packet->stream_index == video_stream &&
        avcodec_send_packet(dec, packet) == 0 &&
        avcodec_receive_frame(dec, frame) == 0) {
      rc = encode_jpeg(frame, width, height, jpeg_out, jpeg_size_out);
      av_packet_unref(packet);
      break;
    }
    av_packet_unref(packet);
  }

done:
  if (frame) av_frame_free(&frame);
  if (packet) av_packet_free(&packet);
  if (dec) avcodec_free_context(&dec);
  if (fmt) avformat_close_input(&fmt);
  if (rc != 0) gb_preview_jpeg_free(jpeg_out);
  return rc;
#endif
}

int gb_preview_jpeg_make_at_ms(const char *source,
                               int64_t position_ms,
                               int width,
                               int height,
                               uint8_t **jpeg_out,
                               size_t *jpeg_size_out) {
  if (!source || !source[0] || !jpeg_out || !jpeg_size_out || width <= 0 || height <= 0) {
    return -1;
  }
  *jpeg_out = NULL;
  *jpeg_size_out = 0;

#if !GBMEDIA_WITH_FFMPEG
  (void) position_ms;
  return -1;
#else
  AVFormatContext *fmt = NULL;
  AVCodecContext *dec = NULL;
  const AVCodec *decoder = NULL;
  AVPacket *packet = NULL;
  AVFrame *frame = NULL;
  int video_stream;
  int64_t target_ts = AV_NOPTS_VALUE;
  int rc = -1;

  if (avformat_open_input(&fmt, source, NULL, NULL) < 0) goto done;
  if (avformat_find_stream_info(fmt, NULL) < 0) goto done;
  video_stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (video_stream < 0 || !decoder) goto done;

  if (position_ms > 0) {
    int64_t duration_ms = fmt->duration > 0 ? fmt->duration / 1000 : 0;
    int64_t seek_ms = duration_ms > 0 ? position_ms % duration_ms : position_ms;
    target_ts = av_rescale_q(seek_ms,
                             (AVRational) {1, 1000},
                             fmt->streams[video_stream]->time_base);
    (void) av_seek_frame(fmt, video_stream, target_ts, AVSEEK_FLAG_BACKWARD);
  }

  dec = avcodec_alloc_context3(decoder);
  if (!dec) goto done;
  if (avcodec_parameters_to_context(dec, fmt->streams[video_stream]->codecpar) < 0) goto done;
  if (avcodec_open2(dec, decoder, NULL) < 0) goto done;

  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (!packet || !frame) goto done;

  for (int i = 0; i < 240 && av_read_frame(fmt, packet) >= 0; i++) {
    if (packet->stream_index == video_stream && avcodec_send_packet(dec, packet) == 0) {
      while (avcodec_receive_frame(dec, frame) == 0) {
        int64_t frame_ts = frame->best_effort_timestamp;
        if (target_ts == AV_NOPTS_VALUE ||
            frame_ts == AV_NOPTS_VALUE ||
            frame_ts >= target_ts) {
          rc = encode_jpeg(frame, width, height, jpeg_out, jpeg_size_out);
          av_packet_unref(packet);
          goto done;
        }
        av_frame_unref(frame);
      }
    }
    av_packet_unref(packet);
  }

done:
  if (frame) av_frame_free(&frame);
  if (packet) av_packet_free(&packet);
  if (dec) avcodec_free_context(&dec);
  if (fmt) avformat_close_input(&fmt);
  if (rc != 0) gb_preview_jpeg_free(jpeg_out);
  return rc;
#endif
}

int gb_preview_jpeg_no_signal(int width,
                              int height,
                              uint8_t **jpeg_out,
                              size_t *jpeg_size_out) {
  if (!jpeg_out || !jpeg_size_out || width <= 0 || height <= 0) return -1;
  *jpeg_out = NULL;
  *jpeg_size_out = 0;
#if !GBMEDIA_WITH_FFMPEG
  return -1;
#else
  AVFrame *frame = av_frame_alloc();
  int rc;
  if (!frame) return -1;
  frame->format = AV_PIX_FMT_YUVJ420P;
  frame->width = width;
  frame->height = height;
  frame->color_range = AVCOL_RANGE_JPEG;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    return -1;
  }
  memset(frame->data[0], 28, (size_t) frame->linesize[0] * (size_t) height);
  memset(frame->data[1], 128, (size_t) frame->linesize[1] * (size_t) ((height + 1) / 2));
  memset(frame->data[2], 128, (size_t) frame->linesize[2] * (size_t) ((height + 1) / 2));
  draw_text_y(frame, "NO SIGNAL", width >= 900 ? 14 : 8);
  rc = encode_jpeg(frame, width, height, jpeg_out, jpeg_size_out);
  av_frame_free(&frame);
  if (rc != 0) gb_preview_jpeg_free(jpeg_out);
  return rc;
#endif
}

int gb_preview_jpeg_no_source(int width,
                              int height,
                              uint8_t **jpeg_out,
                              size_t *jpeg_size_out) {
  if (!jpeg_out || !jpeg_size_out || width <= 0 || height <= 0) return -1;
  *jpeg_out = NULL;
  *jpeg_size_out = 0;
#if !GBMEDIA_WITH_FFMPEG
  return -1;
#else
  AVFrame *frame = av_frame_alloc();
  int rc;
  if (!frame) return -1;
  frame->format = AV_PIX_FMT_YUVJ420P;
  frame->width = width;
  frame->height = height;
  frame->color_range = AVCOL_RANGE_JPEG;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    return -1;
  }
  memset(frame->data[0], 28, (size_t) frame->linesize[0] * (size_t) height);
  memset(frame->data[1], 128, (size_t) frame->linesize[1] * (size_t) ((height + 1) / 2));
  memset(frame->data[2], 128, (size_t) frame->linesize[2] * (size_t) ((height + 1) / 2));
  draw_text_y(frame, "NO SOURCE", width >= 900 ? 14 : 8);
  rc = encode_jpeg(frame, width, height, jpeg_out, jpeg_size_out);
  av_frame_free(&frame);
  if (rc != 0) gb_preview_jpeg_free(jpeg_out);
  return rc;
#endif
}

int gb_preview_jpeg_from_raw(const uint8_t *data,
                             size_t size,
                             int src_width,
                             int src_height,
                             const char *format,
                             int out_width,
                             int out_height,
                             uint8_t **jpeg_out,
                             size_t *jpeg_size_out) {
  if (!data || size == 0 || src_width <= 0 || src_height <= 0 ||
      out_width <= 0 || out_height <= 0 || !jpeg_out || !jpeg_size_out) {
    return -1;
  }
  *jpeg_out = NULL;
  *jpeg_size_out = 0;
#if !GBMEDIA_WITH_FFMPEG
  (void) format;
  return -1;
#else
  AVFrame *frame = av_frame_alloc();
  enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;
  int rc;
  if (!frame) return -1;
  if (format && format[0]) pix_fmt = av_get_pix_fmt(format);
  if (pix_fmt == AV_PIX_FMT_NONE) pix_fmt = AV_PIX_FMT_YUV420P;
  frame->format = pix_fmt;
  frame->width = src_width;
  frame->height = src_height;
  if (av_image_fill_arrays(frame->data,
                           frame->linesize,
                           (uint8_t *) data,
                           pix_fmt,
                           src_width,
                           src_height,
                           1) < 0) {
    av_frame_free(&frame);
    return -1;
  }
  if (av_image_get_buffer_size(pix_fmt, src_width, src_height, 1) > (int) size) {
    av_frame_free(&frame);
    return -1;
  }
  rc = encode_jpeg(frame, out_width, out_height, jpeg_out, jpeg_size_out);
  av_frame_free(&frame);
  if (rc != 0) gb_preview_jpeg_free(jpeg_out);
  return rc;
#endif
}

void gb_preview_jpeg_free(uint8_t **jpeg) {
  if (!jpeg || !*jpeg) return;
  free(*jpeg);
  *jpeg = NULL;
}
