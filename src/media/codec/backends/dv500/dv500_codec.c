#include "codec_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GBMEDIA_WITH_DV500
#define GBMEDIA_WITH_DV500 0
#endif

#if GBMEDIA_WITH_DV500
#include <poll.h>

#include "ot_common.h"
#include "ot_common_vb.h"
#include "ot_common_venc.h"
#include "ot_common_video.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_venc.h"
#endif

#define DV500_MAX_PENDING_PACKETS 8
#define DV500_VPSS_GRP 0
#define DV500_VPSS_CHN 0

typedef struct {
  gbmcd_codec_t base;
  gbmcd_codec_config_t config;
  gbmcd_codec_callbacks_t callbacks;
  gbmcd_rate_control_feedback_t feedback;
  gbmcd_frame_t pending[DV500_MAX_PENDING_PACKETS];
  int pending_count;
  int force_idr;
#if GBMEDIA_WITH_DV500
  ot_venc_chn venc_chn;
  int venc_started;
  int vpss_venc_bound;
  sample_comm_venc_chn_param venc_param;
#endif
} gbmcd_dv500_codec_t;

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

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

static int dv500_supports_codec(const gbmcd_codec_config_t *config) {
  if (!config || config->media_type != GBMCD_MEDIA_VIDEO ||
      config->role != GBMCD_CODEC_ENCODER) {
    return 0;
  }
  return !config->codec_name || !config->codec_name[0] ||
         ascii_ieq(config->codec_name, "H264") ||
         ascii_ieq(config->codec_name, "H.264") ||
         ascii_ieq(config->codec_name, "H265") ||
         ascii_ieq(config->codec_name, "H.265") ||
         ascii_ieq(config->codec_name, "HEVC");
}

#if GBMEDIA_WITH_DV500
static int dv500_result(td_s32 ret) {
  return ret == TD_SUCCESS ? GBMCD_OK : GBMCD_ERR_BACKEND;
}

static int dv500_packet_is_key(const ot_venc_pack *pack) {
  if (!pack) return 0;
  return pack->data_type.h264_type == OT_VENC_H264_NALU_IDR_SLICE ||
         pack->data_type.h264_type == OT_VENC_H264_NALU_SPS ||
         pack->data_type.h264_type == OT_VENC_H264_NALU_PPS ||
         pack->data_type.h265_type == OT_VENC_H265_NALU_IDR_SLICE ||
         pack->data_type.h265_type == OT_VENC_H265_NALU_VPS ||
         pack->data_type.h265_type == OT_VENC_H265_NALU_SPS ||
         pack->data_type.h265_type == OT_VENC_H265_NALU_PPS;
}

static ot_payload_type dv500_payload_type(const char *codec_name) {
  if (ascii_ieq(codec_name, "H265") || ascii_ieq(codec_name, "H.265") ||
      ascii_ieq(codec_name, "HEVC")) {
    return OT_PT_H265;
  }
  return OT_PT_H264;
}

static ot_pic_size dv500_pic_size(int width, int height) {
  if (width <= 640 && height <= 480) return PIC_480P;
  if (width <= 720 && height <= 480) return PIC_D1_NTSC;
  if (width <= 720 && height <= 576) return PIC_D1_PAL;
  if (width <= 1280 && height <= 720) return PIC_720P;
  if (width <= 1920 && height <= 1080) return PIC_1080P;
  if (width <= 2560 && height <= 1440) return PIC_2560X1440;
  if (width <= 2688 && height <= 1520) return PIC_2688X1520;
  return PIC_3840X2160;
}

static int dv500_start_venc(gbmcd_dv500_codec_t *codec) {
  td_s32 ret;
  ot_venc_chn chn;
  if (!codec) return GBMCD_ERR_INVALID;
  memset(&codec->venc_param, 0, sizeof(codec->venc_param));
  codec->venc_param.frame_rate = codec->config.fps_num > 0 ? codec->config.fps_num : 25;
  codec->venc_param.stats_time = 2;
  codec->venc_param.gop = codec->config.gop > 0 ? codec->config.gop : codec->venc_param.frame_rate * 2;
  codec->venc_param.venc_size.width = (td_u32) codec->config.width;
  codec->venc_param.venc_size.height = (td_u32) codec->config.height;
  codec->venc_param.size = dv500_pic_size(codec->config.width, codec->config.height);
  codec->venc_param.profile = 0;
  codec->venc_param.is_rcn_ref_share_buf = TD_FALSE;
  codec->venc_param.type = dv500_payload_type(codec->config.codec_name);
  codec->venc_param.rc_mode = SAMPLE_RC_CBR;
  ret = sample_comm_venc_get_gop_attr(OT_VENC_GOP_MODE_NORMAL_P, &codec->venc_param.gop_attr);
  if (ret != TD_SUCCESS) return dv500_result(ret);
  for (chn = 0; chn < OT_VENC_MAX_CHN_NUM; chn++) {
    ret = sample_comm_venc_start(chn, &codec->venc_param);
    if (ret == TD_SUCCESS) {
      codec->venc_chn = chn;
      codec->venc_started = 1;
      ret = sample_comm_vpss_bind_venc(DV500_VPSS_GRP, DV500_VPSS_CHN, codec->venc_chn);
      if (ret != TD_SUCCESS) {
        sample_comm_venc_stop(codec->venc_chn);
        codec->venc_started = 0;
        return dv500_result(ret);
      }
      codec->vpss_venc_bound = 1;
      (void) ss_mpi_venc_enable_idr(codec->venc_chn, TD_TRUE);
      (void) ss_mpi_venc_request_idr(codec->venc_chn, TD_TRUE);
      return GBMCD_OK;
    }
  }
  return GBMCD_ERR_BUSY;
}

static void dv500_stop_venc(gbmcd_dv500_codec_t *codec) {
  if (!codec) return;
  if (codec->vpss_venc_bound) {
    sample_comm_vpss_un_bind_venc(DV500_VPSS_GRP, DV500_VPSS_CHN, codec->venc_chn);
    codec->vpss_venc_bound = 0;
  }
  if (codec->venc_started) {
    sample_comm_venc_stop(codec->venc_chn);
    codec->venc_started = 0;
  }
}

static int dv500_copy_stream(gbmcd_dv500_codec_t *codec, ot_venc_stream *stream) {
  size_t total = 0;
  uint8_t *buf;
  uint8_t *dst;
  int key = 0;
  gbmcd_frame_t *out;
  if (!codec || !stream || stream->pack_cnt == 0) return GBMCD_ERR_AGAIN;
  if (codec->pending_count >= DV500_MAX_PENDING_PACKETS) return GBMCD_ERR_AGAIN;
  for (uint32_t i = 0; i < stream->pack_cnt; i++) {
    if (stream->pack[i].len > stream->pack[i].offset) {
      total += stream->pack[i].len - stream->pack[i].offset;
    }
    if (dv500_packet_is_key(&stream->pack[i])) key = 1;
  }
  if (total == 0) return GBMCD_ERR_AGAIN;
  buf = (uint8_t *) malloc(total);
  if (!buf) return GBMCD_ERR_NO_MEMORY;
  dst = buf;
  for (uint32_t i = 0; i < stream->pack_cnt; i++) {
    size_t len = stream->pack[i].len > stream->pack[i].offset
                   ? stream->pack[i].len - stream->pack[i].offset
                   : 0;
    if (len == 0) continue;
    memcpy(dst, stream->pack[i].addr + stream->pack[i].offset, len);
    dst += len;
  }
  out = &codec->pending[codec->pending_count++];
  memset(out, 0, sizeof(*out));
  out->type = GBMCD_FRAME_PACKET;
  out->pts_us = stream->pack[0].pts;
  out->data = buf;
  out->size = total;
  out->key_frame = key;
  out->width = codec->config.width;
  out->height = codec->config.height;
  out->format = ascii_ieq(codec->config.codec_name, "H265") ? "h265" : "h264";
  out->flags = GBMCD_FRAME_FLAG_OWNED_DATA;
  return GBMCD_OK;
}

static int dv500_drain_stream(gbmcd_dv500_codec_t *codec, int timeout_ms) {
  ot_venc_chn_status stat;
  ot_venc_stream stream;
  struct pollfd pfd;
  int fd;
  td_s32 ret;
  if (!codec || !codec->venc_started) return GBMCD_ERR_INVALID;
  fd = ss_mpi_venc_get_fd(codec->venc_chn);
  if (fd < 0) return GBMCD_ERR_BACKEND;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, timeout_ms) <= 0) return GBMCD_ERR_AGAIN;
  memset(&stat, 0, sizeof(stat));
  ret = ss_mpi_venc_query_status(codec->venc_chn, &stat);
  if (ret != TD_SUCCESS || stat.cur_packs == 0) return GBMCD_ERR_AGAIN;
  memset(&stream, 0, sizeof(stream));
  stream.pack = (ot_venc_pack *) calloc(stat.cur_packs, sizeof(ot_venc_pack));
  if (!stream.pack) return GBMCD_ERR_NO_MEMORY;
  stream.pack_cnt = stat.cur_packs;
  ret = ss_mpi_venc_get_stream(codec->venc_chn, &stream, TD_FALSE);
  if (ret == TD_SUCCESS) {
    int rc = dv500_copy_stream(codec, &stream);
    (void) ss_mpi_venc_release_stream(codec->venc_chn, &stream);
    free(stream.pack);
    return rc;
  }
  free(stream.pack);
  return dv500_result(ret);
}
#endif

static int dv500_probe(void) {
#if GBMEDIA_WITH_DV500
  return GBMCD_OK;
#else
  return GBMCD_ERR_UNSUPPORTED;
#endif
}

static int dv500_list_codecs(gbmcd_media_type_t media_type,
                             gbmcd_codec_role_t role,
                             gbmcd_codec_info_t *codecs,
                             size_t max_codecs,
                             size_t *count_out) {
  if (!count_out) return GBMCD_ERR_INVALID;
  *count_out = 0;
  if (media_type != GBMCD_MEDIA_VIDEO || role != GBMCD_CODEC_ENCODER) {
    return GBMCD_ERR_UNSUPPORTED;
  }
#if !GBMEDIA_WITH_DV500
  (void) codecs;
  (void) max_codecs;
  return GBMCD_ERR_UNSUPPORTED;
#else
  *count_out = 2;
  if (codecs && max_codecs > 0) {
    memset(&codecs[0], 0, sizeof(codecs[0]));
    copy_text(codecs[0].backend, sizeof(codecs[0].backend), "dv500");
    copy_text(codecs[0].name, sizeof(codecs[0].name), "H264");
    copy_text(codecs[0].description, sizeof(codecs[0].description), "DV500 hardware H.264 encoder");
    codecs[0].media_type = media_type;
    codecs[0].role = role;
    codecs[0].hardware = 1;
  }
  if (codecs && max_codecs > 1) {
    memset(&codecs[1], 0, sizeof(codecs[1]));
    copy_text(codecs[1].backend, sizeof(codecs[1].backend), "dv500");
    copy_text(codecs[1].name, sizeof(codecs[1].name), "H265");
    copy_text(codecs[1].description, sizeof(codecs[1].description), "DV500 hardware H.265 encoder");
    codecs[1].media_type = media_type;
    codecs[1].role = role;
    codecs[1].hardware = 1;
  }
  return GBMCD_OK;
#endif
}

static int dv500_open(const gbmcd_codec_config_t *config,
                      const gbmcd_codec_callbacks_t *callbacks,
                      gbmcd_codec_t **codec_out) {
  gbmcd_dv500_codec_t *codec;
  if (!config || !codec_out) return GBMCD_ERR_INVALID;
  *codec_out = NULL;
  if (!dv500_supports_codec(config)) return GBMCD_ERR_UNSUPPORTED;
#if !GBMEDIA_WITH_DV500
  (void) callbacks;
  return GBMCD_ERR_UNSUPPORTED;
#else
  codec = (gbmcd_dv500_codec_t *) calloc(1, sizeof(*codec));
  if (!codec) return GBMCD_ERR_NO_MEMORY;
  codec->base.backend = gbmcd_dv500_backend();
  codec->config = *config;
  if (callbacks) codec->callbacks = *callbacks;
  codec->feedback.target_bitrate_kbps = config->bitrate_kbps;
  codec->feedback.min_bitrate_kbps = config->bitrate_kbps / 2;
  codec->feedback.max_bitrate_kbps = config->bitrate_kbps * 2;
  if (dv500_start_venc(codec) != GBMCD_OK) {
    free(codec);
    return GBMCD_ERR_BACKEND;
  }
  *codec_out = &codec->base;
  return GBMCD_OK;
#endif
}

static void dv500_close(gbmcd_codec_t *codec) {
  if (!codec) return;
  gbmcd_dv500_codec_t *dv = (gbmcd_dv500_codec_t *) codec;
  for (int i = 0; i < dv->pending_count; i++) gbmcd_frame_release(&dv->pending[i]);
#if GBMEDIA_WITH_DV500
  dv500_stop_venc(dv);
#endif
  free(codec);
}

static int dv500_send_frame(gbmcd_codec_t *codec, const gbmcd_frame_t *frame) {
  if (!codec || !frame) return GBMCD_ERR_INVALID;
  if (frame->type != GBMCD_FRAME_RAW) return GBMCD_ERR_INVALID;
#if !GBMEDIA_WITH_DV500
  return GBMCD_ERR_UNSUPPORTED;
#else
  gbmcd_dv500_codec_t *dv = (gbmcd_dv500_codec_t *) codec;
  /*
   * The preferred DV500 path binds VI/VPSS directly into VENC. The raw frame
   * argument keeps the generic framework contract; each call drains one encoded
   * packet produced by the hardware channel.
   */
  (void) frame;
  if (dv->force_idr) {
    (void) ss_mpi_venc_request_idr(dv->venc_chn, TD_TRUE);
    dv->force_idr = 0;
  }
  return dv500_drain_stream(dv, 0);
#endif
}

static int dv500_receive_frame(gbmcd_codec_t *codec, gbmcd_frame_t *frame_out) {
  if (!codec || !frame_out) return GBMCD_ERR_INVALID;
  memset(frame_out, 0, sizeof(*frame_out));
#if !GBMEDIA_WITH_DV500
  return GBMCD_ERR_UNSUPPORTED;
#else
  gbmcd_dv500_codec_t *dv = (gbmcd_dv500_codec_t *) codec;
  if (dv->pending_count == 0) {
    int rc = dv500_drain_stream(dv, 1);
    if (rc != GBMCD_OK) return rc;
  }
  *frame_out = dv->pending[0];
  for (int i = 1; i < dv->pending_count; i++) dv->pending[i - 1] = dv->pending[i];
  memset(&dv->pending[--dv->pending_count], 0, sizeof(dv->pending[0]));
  if (dv->callbacks.on_frame) return dv->callbacks.on_frame(dv->callbacks.user_data, frame_out);
  return GBMCD_OK;
#endif
}

static int dv500_request_keyframe(gbmcd_codec_t *codec) {
  if (!codec) return GBMCD_ERR_INVALID;
  ((gbmcd_dv500_codec_t *) codec)->force_idr = 1;
  return GBMCD_OK;
}

static int dv500_set_bitrate(gbmcd_codec_t *codec, int bitrate_kbps) {
  if (!codec || bitrate_kbps <= 0) return GBMCD_ERR_INVALID;
  ((gbmcd_dv500_codec_t *) codec)->feedback.target_bitrate_kbps = bitrate_kbps;
  return GBMCD_OK;
}

static int dv500_poll_feedback(gbmcd_codec_t *codec,
                               gbmcd_rate_control_feedback_t *feedback_out) {
  if (!codec || !feedback_out) return GBMCD_ERR_INVALID;
  *feedback_out = ((gbmcd_dv500_codec_t *) codec)->feedback;
  return GBMCD_OK;
}

const gbmcd_codec_backend_t *gbmcd_dv500_backend(void) {
  static const gbmcd_codec_backend_t backend = {
      "dv500",
      "DV500 hardware video codec backend",
      300,
      dv500_probe,
      dv500_list_codecs,
      dv500_open,
      dv500_close,
      dv500_send_frame,
      dv500_receive_frame,
      dv500_request_keyframe,
      dv500_set_bitrate,
      dv500_poll_feedback,
  };
  return &backend;
}
