#include "codec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  gbmcd_codec_t base;
  gbmcd_codec_config_t config;
  gbmcd_codec_callbacks_t callbacks;
  gbmcd_frame_t last_frame;
  gbmcd_rate_control_feedback_t feedback;
} gbmcd_null_codec_t;

static int null_probe(void) {
  return GBMCD_OK;
}

static void fill_codec(gbmcd_codec_info_t *info,
                       gbmcd_media_type_t media_type,
                       gbmcd_codec_role_t role,
                       const char *name,
                       const char *description) {
  memset(info, 0, sizeof(*info));
  strcpy(info->backend, "null");
  snprintf(info->name, sizeof(info->name), "%s", name);
  snprintf(info->description, sizeof(info->description), "%s", description);
  info->media_type = media_type;
  info->role = role;
  info->hardware = 0;
}

static int null_list_codecs(gbmcd_media_type_t media_type,
                            gbmcd_codec_role_t role,
                            gbmcd_codec_info_t *codecs,
                            size_t max_codecs,
                            size_t *count_out) {
  if (!count_out) return GBMCD_ERR_INVALID;
  *count_out = 1;
  if (codecs && max_codecs > 0) {
    fill_codec(&codecs[0], media_type, role, "null",
               "No-op codec backend for tests and unsupported platforms");
  }
  return GBMCD_OK;
}

static int null_open(const gbmcd_codec_config_t *config,
                     const gbmcd_codec_callbacks_t *callbacks,
                     gbmcd_codec_t **codec_out) {
  if (!config || !codec_out) return GBMCD_ERR_INVALID;
  gbmcd_null_codec_t *codec = (gbmcd_null_codec_t *) calloc(1, sizeof(*codec));
  if (!codec) return GBMCD_ERR_NO_MEMORY;
  codec->base.backend = gbmcd_null_backend();
  codec->config = *config;
  codec->feedback.target_bitrate_kbps = config->bitrate_kbps;
  codec->feedback.min_bitrate_kbps = config->bitrate_kbps / 2;
  codec->feedback.max_bitrate_kbps = config->bitrate_kbps * 2;
  if (callbacks) codec->callbacks = *callbacks;
  *codec_out = &codec->base;
  return GBMCD_OK;
}

static void null_close(gbmcd_codec_t *codec) {
  free(codec);
}

static int null_send_frame(gbmcd_codec_t *codec, const gbmcd_frame_t *frame) {
  if (!codec || !frame) return GBMCD_ERR_INVALID;
  gbmcd_null_codec_t *null_codec = (gbmcd_null_codec_t *) codec;
  null_codec->last_frame = *frame;
  if (null_codec->callbacks.on_frame) {
    return null_codec->callbacks.on_frame(null_codec->callbacks.user_data, frame);
  }
  return GBMCD_OK;
}

static int null_receive_frame(gbmcd_codec_t *codec, gbmcd_frame_t *frame_out) {
  if (!codec || !frame_out) return GBMCD_ERR_INVALID;
  *frame_out = ((gbmcd_null_codec_t *) codec)->last_frame;
  return GBMCD_OK;
}

static int null_set_bitrate(gbmcd_codec_t *codec, int bitrate_kbps) {
  if (!codec || bitrate_kbps <= 0) return GBMCD_ERR_INVALID;
  gbmcd_null_codec_t *null_codec = (gbmcd_null_codec_t *) codec;
  null_codec->feedback.target_bitrate_kbps = bitrate_kbps;
  return GBMCD_OK;
}

static int null_request_keyframe(gbmcd_codec_t *codec) {
  return codec ? GBMCD_OK : GBMCD_ERR_INVALID;
}

static int null_poll_feedback(gbmcd_codec_t *codec,
                              gbmcd_rate_control_feedback_t *feedback_out) {
  if (!codec || !feedback_out) return GBMCD_ERR_INVALID;
  gbmcd_null_codec_t *null_codec = (gbmcd_null_codec_t *) codec;
  *feedback_out = null_codec->feedback;
  if (null_codec->callbacks.on_rate_feedback) {
    return null_codec->callbacks.on_rate_feedback(null_codec->callbacks.user_data,
                                                 feedback_out);
  }
  return GBMCD_OK;
}

const gbmcd_codec_backend_t *gbmcd_null_backend(void) {
  static const gbmcd_codec_backend_t backend = {
      "null",
      "Portable no-op codec backend for tests and unsupported platforms",
      -1000,
      null_probe,
      null_list_codecs,
      null_open,
      null_close,
      null_send_frame,
      null_receive_frame,
      null_request_keyframe,
      null_set_bitrate,
      null_poll_feedback,
  };
  return &backend;
}
