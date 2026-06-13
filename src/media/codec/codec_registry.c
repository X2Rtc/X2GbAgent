#include "codec_internal.h"

#include <stdlib.h>
#include <string.h>

#define GBMCD_MAX_BACKENDS 16

static const gbmcd_codec_backend_t *s_backends[GBMCD_MAX_BACKENDS];
static size_t s_backend_count;
static int s_builtins_registered;

static int backend_name_matches(const gbmcd_codec_backend_t *backend,
                                const char *name) {
  return backend && backend->name && name && strcmp(backend->name, name) == 0;
}

int gbmcd_register_backend(const gbmcd_codec_backend_t *backend) {
  if (!backend || !backend->name || !backend->open || !backend->close) {
    return GBMCD_ERR_INVALID;
  }
  if (gbmcd_find_backend(backend->name)) return GBMCD_OK;
  if (s_backend_count >= GBMCD_MAX_BACKENDS) return GBMCD_ERR_NO_MEMORY;

  size_t pos = s_backend_count;
  while (pos > 0 && s_backends[pos - 1]->priority < backend->priority) {
    s_backends[pos] = s_backends[pos - 1];
    pos--;
  }
  s_backends[pos] = backend;
  s_backend_count++;
  return GBMCD_OK;
}

int gbmcd_unregister_backend(const char *backend_name) {
  if (!backend_name) return GBMCD_ERR_INVALID;
  for (size_t i = 0; i < s_backend_count; i++) {
    if (backend_name_matches(s_backends[i], backend_name)) {
      for (size_t j = i + 1; j < s_backend_count; j++) {
        s_backends[j - 1] = s_backends[j];
      }
      s_backends[--s_backend_count] = NULL;
      return GBMCD_OK;
    }
  }
  return GBMCD_ERR_NOT_FOUND;
}

size_t gbmcd_backend_count(void) {
  return s_backend_count;
}

const gbmcd_codec_backend_t *gbmcd_backend_at(size_t index) {
  return index < s_backend_count ? s_backends[index] : NULL;
}

const gbmcd_codec_backend_t *gbmcd_find_backend(const char *backend_name) {
  if (!backend_name || !backend_name[0]) return NULL;
  for (size_t i = 0; i < s_backend_count; i++) {
    if (backend_name_matches(s_backends[i], backend_name)) return s_backends[i];
  }
  return NULL;
}

int gbmcd_register_builtin_backends(void) {
  if (s_builtins_registered) return GBMCD_OK;
  s_builtins_registered = 1;
  (void) gbmcd_register_backend(gbmcd_null_backend());
#if defined(GBMEDIA_ENABLE_DV500_BACKEND) && GBMEDIA_ENABLE_DV500_BACKEND
  (void) gbmcd_register_backend(gbmcd_dv500_backend());
#endif
#if defined(GBMEDIA_ENABLE_FFMPEG_BACKEND) && GBMEDIA_ENABLE_FFMPEG_BACKEND
  (void) gbmcd_register_backend(gbmcd_ffmpeg_backend());
#endif
  return GBMCD_OK;
}

int gbmcd_list_codecs(const char *backend_name,
                      gbmcd_media_type_t media_type,
                      gbmcd_codec_role_t role,
                      gbmcd_codec_info_t *codecs,
                      size_t max_codecs,
                      size_t *count_out) {
  if (!count_out) return GBMCD_ERR_INVALID;
  *count_out = 0;
  gbmcd_register_builtin_backends();

  if (backend_name && backend_name[0]) {
    const gbmcd_codec_backend_t *backend = gbmcd_find_backend(backend_name);
    if (!backend) return GBMCD_ERR_NOT_FOUND;
    if (!backend->list_codecs) return GBMCD_ERR_UNSUPPORTED;
    return backend->list_codecs(media_type, role, codecs, max_codecs, count_out);
  }

  size_t total = 0;
  for (size_t i = 0; i < s_backend_count; i++) {
    const gbmcd_codec_backend_t *backend = s_backends[i];
    if (!backend->list_codecs) continue;
    size_t written = 0;
    size_t room = total < max_codecs ? max_codecs - total : 0;
    int rc = backend->list_codecs(media_type, role,
                                  room ? codecs + total : NULL,
                                  room,
                                  &written);
    if (rc != GBMCD_OK && rc != GBMCD_ERR_UNSUPPORTED) return rc;
    total += written;
  }
  *count_out = total;
  return GBMCD_OK;
}

int gbmcd_open(const gbmcd_codec_config_t *config,
               const gbmcd_codec_callbacks_t *callbacks,
               gbmcd_codec_t **codec_out) {
  if (!config || !codec_out) return GBMCD_ERR_INVALID;
  *codec_out = NULL;
  gbmcd_register_builtin_backends();

  if (config->backend_name && config->backend_name[0]) {
    const gbmcd_codec_backend_t *backend = gbmcd_find_backend(config->backend_name);
    if (!backend) return GBMCD_ERR_NOT_FOUND;
    return backend->open(config, callbacks, codec_out);
  }

  for (size_t i = 0; i < s_backend_count; i++) {
    const gbmcd_codec_backend_t *backend = s_backends[i];
    if (backend->probe && backend->probe() != GBMCD_OK) continue;
    int rc = backend->open(config, callbacks, codec_out);
    if (rc == GBMCD_OK) return GBMCD_OK;
  }
  return GBMCD_ERR_NOT_FOUND;
}

void gbmcd_close(gbmcd_codec_t **codec) {
  if (!codec || !*codec) return;
  gbmcd_codec_t *local = *codec;
  *codec = NULL;
  if (local->backend && local->backend->close) local->backend->close(local);
}

int gbmcd_send_frame(gbmcd_codec_t *codec, const gbmcd_frame_t *frame) {
  if (!codec || !codec->backend || !frame) return GBMCD_ERR_INVALID;
  return codec->backend->send_frame
           ? codec->backend->send_frame(codec, frame)
           : GBMCD_ERR_UNSUPPORTED;
}

int gbmcd_receive_frame(gbmcd_codec_t *codec, gbmcd_frame_t *frame_out) {
  if (!codec || !codec->backend || !frame_out) return GBMCD_ERR_INVALID;
  return codec->backend->receive_frame
           ? codec->backend->receive_frame(codec, frame_out)
           : GBMCD_ERR_UNSUPPORTED;
}

void gbmcd_frame_release(gbmcd_frame_t *frame) {
  if (!frame) return;
  if ((frame->flags & GBMCD_FRAME_FLAG_OWNED_DATA) != 0) {
    free((void *) frame->data);
  }
  memset(frame, 0, sizeof(*frame));
}

int gbmcd_request_keyframe(gbmcd_codec_t *codec) {
  if (!codec || !codec->backend) return GBMCD_ERR_INVALID;
  return codec->backend->request_keyframe
           ? codec->backend->request_keyframe(codec)
           : GBMCD_ERR_UNSUPPORTED;
}

static int clamp_int(int value, int min_value, int max_value) {
  if (min_value > 0 && value < min_value) value = min_value;
  if (max_value > 0 && value > max_value) value = max_value;
  return value;
}

int gbmcd_adapt_bitrate(gbmcd_codec_t *codec,
                        const gbmcd_rate_control_feedback_t *feedback,
                        int *applied_bitrate_kbps) {
  int current;
  int next;
  int rc;
  if (!codec || !feedback) return GBMCD_ERR_INVALID;
  current = feedback->target_bitrate_kbps;
  if (current <= 0) return GBMCD_ERR_INVALID;
  next = current;
  if (feedback->dropped_frames > 0 ||
      feedback->congestion_score >= 70 ||
      feedback->encoder_queue_ms >= 500) {
    next = current * 85 / 100;
  } else if (feedback->congestion_score <= 20 &&
             feedback->encoder_queue_ms <= 100 &&
             feedback->dropped_frames == 0) {
    next = current * 105 / 100;
  }
  next = clamp_int(next, feedback->min_bitrate_kbps, feedback->max_bitrate_kbps);
  rc = gbmcd_set_bitrate(codec, next);
  if (rc == GBMCD_OK && applied_bitrate_kbps) *applied_bitrate_kbps = next;
  return rc;
}

int gbmcd_set_bitrate(gbmcd_codec_t *codec, int bitrate_kbps) {
  if (!codec || !codec->backend || bitrate_kbps <= 0) return GBMCD_ERR_INVALID;
  return codec->backend->set_bitrate
           ? codec->backend->set_bitrate(codec, bitrate_kbps)
           : GBMCD_ERR_UNSUPPORTED;
}

int gbmcd_poll_feedback(gbmcd_codec_t *codec,
                        gbmcd_rate_control_feedback_t *feedback_out) {
  if (!codec || !codec->backend || !feedback_out) return GBMCD_ERR_INVALID;
  return codec->backend->poll_feedback
           ? codec->backend->poll_feedback(codec, feedback_out)
           : GBMCD_ERR_UNSUPPORTED;
}

const char *gbmcd_result_name(int result) {
  switch (result) {
    case GBMCD_OK: return "OK";
    case GBMCD_ERR_INVALID: return "INVALID";
    case GBMCD_ERR_NOT_FOUND: return "NOT_FOUND";
    case GBMCD_ERR_NO_MEMORY: return "NO_MEMORY";
    case GBMCD_ERR_BACKEND: return "BACKEND";
    case GBMCD_ERR_UNSUPPORTED: return "UNSUPPORTED";
    case GBMCD_ERR_AGAIN: return "AGAIN";
    case GBMCD_ERR_EOF: return "EOF";
    case GBMCD_ERR_BUSY: return "BUSY";
    case GBMCD_ERR_PERMISSION: return "PERMISSION";
    default: return "UNKNOWN";
  }
}
