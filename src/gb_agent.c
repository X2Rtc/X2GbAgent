#include "gb_agent.h"
#include "gb_media_capture.h"
#include "gb_media_codec.h"
#include "gb_platform.h"
#include "source_manager.h"

#if GB_AGENT_USE_X2_GBSDK
#include "c_gb28181_api.h"

typedef void (*o2rtn_check)(const char *fmt, ...);
o2rtn_check funcO2RtnCheck = NULL;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GBMEDIA_WITH_DV500
#define GBMEDIA_WITH_DV500 0
#endif

typedef struct gb_agent_media_runtime gb_agent_media_runtime_t;

typedef struct {
  uint8_t *data;
  size_t size;
  int width;
  int height;
  char format[32];
  unsigned long generation;
  time_t updated_at;
} gb_agent_raw_snapshot_t;

typedef struct {
  gb_agent_t *agent;
  int platform_id;
  unsigned generation;
} gb_agent_cb_ctx_t;

typedef struct {
#if GB_AGENT_USE_X2_GBSDK
  c_gb28181_api_t *api;
  c_gb28181_api_config_t sdk_cfg;
  c_gb28181_api_channel_config_t sdk_channels[GB_AGENT_MAX_CHANNELS_PER_CLIENT];
  c_gb28181_api_callbacks_t callbacks;
  gb_agent_cb_ctx_t cb_ctx;
  char server_domain[32];
  char local_ip[64];
  char channel_ids[GB_AGENT_MAX_CHANNELS_PER_CLIENT][32];
  char channel_names[GB_AGENT_MAX_CHANNELS_PER_CLIENT][32];
  time_t reconnect_at;
  unsigned reconnect_attempts;
#endif
  gb_agent_channel_t channels[GB_AGENT_MAX_CHANNELS_PER_CLIENT];
  int channel_count;
  unsigned generation;
  int cleanup_pending;
#if GB_AGENT_USE_X2_GBSDK
  gb_agent_cb_ctx_t *cleanup_ctx;
#endif
  gb_agent_platform_t cfg;
  gb_agent_status_t status;
} gb_agent_client_t;

#if GB_AGENT_USE_X2_GBSDK
typedef struct {
  gb_agent_t *agent;
  c_gb28181_api_t *api;
  gb_agent_cb_ctx_t *cb_ctx;
  int platform_id;
} gb_agent_stop_job_t;

static const char *sdk_media_proto_name(c_gb28181_api_media_proto_t proto) {
  switch (proto) {
    case C_GB28181_API_MEDIA_PROTO_RTC: return "RTC";
    case C_GB28181_API_MEDIA_PROTO_RTP: return "RTP";
    default: return "UNKNOWN";
  }
}

static const char *sdk_codec_name(c_gb28181_api_codec_type_t codec) {
  switch (codec) {
    case C_GB28181_API_CODEC_PS: return "PS";
    case C_GB28181_API_CODEC_PCMA: return "PCMA";
    case C_GB28181_API_CODEC_PCMU: return "PCMU";
    case C_GB28181_API_CODEC_OPUS: return "OPUS";
    case C_GB28181_API_CODEC_H264: return "H264";
    case C_GB28181_API_CODEC_H265: return "H265";
    case C_GB28181_API_CODEC_NONE: return "NONE";
    default: return "UNKNOWN";
  }
}
#endif

struct gb_agent {
  gb_mutex_t mu;
  gb_mutex_t media_send_mu;
#if GB_AGENT_USE_X2_GBSDK
  gb_cond_t cv;
  int cleanup_jobs;
#endif
  gb_agent_callbacks_t callbacks;
  gb_agent_media_source_t media_sources[GB_AGENT_MAX_CHANNELS];
  int media_api_idx[GB_AGENT_MAX_CHANNELS];
  char media_channel_ids[GB_AGENT_MAX_CHANNELS][32];
  int channel_push_active[GB_AGENT_MAX_CHANNELS];
  int channel_talkback_active[GB_AGENT_MAX_CHANNELS];
  int channel_broadcast_active[GB_AGENT_MAX_CHANNELS];
  unsigned long media_generations[GB_AGENT_MAX_CHANNELS];
  int media_thread_running[GB_AGENT_MAX_CHANNELS];
  gb_thread_t media_threads[GB_AGENT_MAX_CHANNELS];
  gb_agent_client_t clients[GB_AGENT_MAX_CLIENTS];
};

struct gb_agent_media_runtime {
  gb_agent_t *agent;
  int platform_id;
  int platform_idx;
  int client_idx;
  gb_agent_media_source_t source;
  unsigned long generation;
  gbmcd_codec_t *video_encoder;
  gbmcd_codec_t *audio_encoder;
  int video_encoder_width;
  int video_encoder_height;
  char source_key[512];
  int source_acquired;
  int64_t next_preview_pts_us;
  int64_t preview_synthetic_pts_us;
  int stop_requested;
  int logged_first_video_packet;
  int logged_key_video_packet;
  int logged_first_audio_packet;
  int logged_first_raw_frame;
  uint8_t *status_frame;
  size_t status_frame_size;
  int status_frame_width;
  int status_frame_height;
  char status_frame_text[32];
};

static int gb_agent_stop_client_media_sources(gb_agent_t *agent, int client_idx);
static int client_any_media_thread_running_locked(gb_agent_t *agent, int client_idx);

#if GB_AGENT_USE_X2_GBSDK
static int callback_is_current(gb_agent_t *agent, const gb_agent_cb_ctx_t *ctx, int idx) {
  return agent != NULL && ctx != NULL && idx >= 0 &&
         !agent->clients[idx].cleanup_pending &&
         agent->clients[idx].generation == ctx->generation &&
         &agent->clients[idx].cb_ctx == ctx;
}
#endif

static void copy_text(char *dst, size_t size, const char *src) {
  size_t i;
  if (dst == NULL || size == 0) return;
  if (src == NULL) src = "";
  if (dst == src) return;
  for (i = 0; i + 1 < size && src[i] != '\0'; i++) dst[i] = src[i];
  dst[i] = '\0';
}

static int client_index(int platform_id) {
  return platform_id >= 1 && platform_id <= GB_AGENT_MAX_CLIENTS ? platform_id - 1 : -1;
}

static int media_index(int channel_id) {
  return channel_id >= 1 && channel_id <= GB_AGENT_MAX_CHANNELS ? channel_id - 1 : -1;
}

static int media_status_index(gb_agent_t *agent, int media_idx) {
  int idx;
  if (agent == NULL || media_idx < 0 || media_idx >= GB_AGENT_MAX_CHANNELS) return -1;
  idx = agent->media_api_idx[media_idx];
  if (idx >= 0 && idx < GB_AGENT_MAX_CLIENTS) return idx;
  return -1;
}

static void emit_log(gb_agent_t *agent,
                     int platform_id,
                     const char *level,
                     const char *category,
                     const char *message) {
  if (agent != NULL && agent->callbacks.on_log != NULL) {
    agent->callbacks.on_log(agent->callbacks.user_data,
                            platform_id,
                            level != NULL ? level : "INFO",
                            category != NULL ? category : "SIP",
                            message != NULL ? message : "");
  }
}

static void set_reason(gb_agent_status_t *status, const char *reason) {
  if (status == NULL) return;
  copy_text(status->last_reason, sizeof(status->last_reason), reason);
  status->updated_at = time(NULL);
}

static int parse_resolution(const char *resolution, int *width, int *height) {
  int w = 0;
  int h = 0;
  if (resolution != NULL && sscanf(resolution, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
    *width = w;
    *height = h;
    return 1;
  }
  return 0;
}

static const char *media_capture_backend_name(const gb_agent_media_source_t *source) {
#if GBMEDIA_WITH_DV500
  if (source != NULL && strcmp(source->source_mode, "device") == 0 &&
      (source->video_device[0] == '\0' ||
       strncmp(source->video_device, "dv500://", 8) == 0)) {
    return "dv500";
  }
#else
  (void) source;
#endif
  return "ffmpeg";
}

static void refresh_static_status(gb_agent_client_t *client) {
  if (client == NULL) return;
  client->status.id = client->cfg.id;
  client->status.configured = 1;
  client->status.desired_enabled = client->cfg.enabled;
  copy_text(client->status.server_ip, sizeof(client->status.server_ip), client->cfg.server_ip);
  copy_text(client->status.transport, sizeof(client->status.transport), client->cfg.transport);
  client->status.server_port = client->cfg.sip_port;
  client->status.local_sip_port = 15060 + (client->cfg.id - 1) * 10;
}

static void mark_client_media_running(gb_agent_t *agent,
                                      int client_idx,
                                      unsigned long generation,
                                      int running,
                                      const char *reason) {
  if (agent == NULL || client_idx < 0 || client_idx >= GB_AGENT_MAX_CLIENTS) return;
  gb_mutex_lock(&agent->mu);
  if (!running && client_any_media_thread_running_locked(agent, client_idx)) running = 1;
  agent->clients[client_idx].status.media_running = running;
  agent->clients[client_idx].status.media_generation = generation;
  if (reason != NULL) set_reason(&agent->clients[client_idx].status, reason);
  gb_mutex_unlock(&agent->mu);
}

static void mark_media_frame_encoded(gb_agent_t *agent, int idx) {
  int status_idx = media_status_index(agent, idx);
  if (status_idx < 0) return;
  gb_mutex_lock(&agent->mu);
  agent->clients[status_idx].status.media_frames_encoded++;
  agent->clients[status_idx].status.updated_at = time(NULL);
  gb_mutex_unlock(&agent->mu);
}

static void mark_media_frame_sent(gb_agent_t *agent, int idx) {
  int status_idx = media_status_index(agent, idx);
  if (status_idx < 0) return;
  gb_mutex_lock(&agent->mu);
  agent->clients[status_idx].status.media_frames_sent++;
  agent->clients[status_idx].status.updated_at = time(NULL);
  gb_mutex_unlock(&agent->mu);
}

static void mark_media_encode_error(gb_agent_t *agent, int idx) {
  int status_idx = media_status_index(agent, idx);
  if (status_idx < 0) return;
  gb_mutex_lock(&agent->mu);
  agent->clients[status_idx].status.media_encode_errors++;
  agent->clients[status_idx].status.updated_at = time(NULL);
  gb_mutex_unlock(&agent->mu);
}

static void mark_media_send_error(gb_agent_t *agent, int idx, int code, const char *reason) {
  int status_idx = media_status_index(agent, idx);
  if (status_idx < 0) return;
  gb_mutex_lock(&agent->mu);
  agent->clients[status_idx].status.media_encode_errors++;
  agent->clients[status_idx].status.last_error_code = code;
  if (reason != NULL) set_reason(&agent->clients[status_idx].status, reason);
  else agent->clients[status_idx].status.updated_at = time(NULL);
  gb_mutex_unlock(&agent->mu);
}

static const char *encoded_packet_format(const uint8_t *data, size_t size) {
  uint32_t nalu_size;
  if (data == NULL || size < 4) return "unknown";
  if ((data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) ||
      (size >= 5 && data[0] == 0x00 && data[1] == 0x00 &&
       data[2] == 0x00 && data[3] == 0x01)) {
    return "AnnexB";
  }
  nalu_size = ((uint32_t)data[0] << 24) |
              ((uint32_t)data[1] << 16) |
              ((uint32_t)data[2] << 8) |
              (uint32_t)data[3];
  if (nalu_size > 0 && nalu_size <= size - 4) return "AVCC-like";
  return "unknown";
}

static int media_generation_current(gb_agent_t *agent, int idx, unsigned long generation) {
  int current;
  if (agent == NULL || idx < 0 || idx >= GB_AGENT_MAX_CHANNELS) return 0;
  gb_mutex_lock(&agent->mu);
  current = agent->media_thread_running[idx] && agent->media_generations[idx] == generation;
  gb_mutex_unlock(&agent->mu);
  return current;
}

static void media_output_video_size(const gb_agent_media_source_t *source, int *width, int *height) {
  int w = 1280;
  int h = 720;
  if (source != NULL) (void) parse_resolution(source->resolution, &w, &h);
  if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
    w = 1280;
    h = 720;
  }
  if (width != NULL) *width = w;
  if (height != NULL) *height = h;
}

static int media_open_video_encoder(gb_agent_media_runtime_t *rt) {
  gbmcd_codec_config_t cfg;
  int width;
  int height;
  int rc;
  if (rt == NULL) return -1;
  media_output_video_size(&rt->source, &width, &height);
  memset(&cfg, 0, sizeof(cfg));
  cfg.backend_name = media_capture_backend_name(&rt->source);
  cfg.codec_name = "H264";
  cfg.media_type = GBMCD_MEDIA_VIDEO;
  cfg.role = GBMCD_CODEC_ENCODER;
  cfg.width = width;
  cfg.height = height;
  cfg.fps_num = rt->source.fps > 0 ? rt->source.fps : 25;
  cfg.fps_den = 1;
  cfg.bitrate_kbps = rt->source.bitrate_kbps > 0 ? rt->source.bitrate_kbps : 2048;
  cfg.gop = rt->source.gop > 0
              ? rt->source.gop
              : cfg.fps_num * (rt->source.iframe_interval > 0 ? rt->source.iframe_interval : 3);
  cfg.low_latency = 1;
  cfg.pixel_format = "yuv420p";
  rc = gbmcd_open(&cfg, NULL, &rt->video_encoder);
  if (rc == GBMCD_OK) {
    rt->video_encoder_width = width;
    rt->video_encoder_height = height;
  }
  return rc;
}

static int media_open_audio_encoder(gb_agent_media_runtime_t *rt) {
  gbmcd_codec_config_t cfg;
  int rc;
  if (rt == NULL) return -1;
  memset(&cfg, 0, sizeof(cfg));
  cfg.backend_name = "ffmpeg";
  cfg.codec_name = "G711A";
  cfg.media_type = GBMCD_MEDIA_AUDIO;
  cfg.role = GBMCD_CODEC_ENCODER;
  cfg.sample_rate = 8000;
  cfg.channels = 1;
  cfg.bitrate_kbps = 64;
  cfg.sample_format = "s16";
  cfg.low_latency = 1;
  rc = gbmcd_open(&cfg, NULL, &rt->audio_encoder);
  return rc;
}

static int media_ensure_video_encoder(gb_agent_media_runtime_t *rt) {
  int width;
  int height;
  if (rt == NULL) return -1;
  media_output_video_size(&rt->source, &width, &height);
  if (rt->video_encoder != NULL &&
      rt->video_encoder_width == width &&
      rt->video_encoder_height == height) {
    return 0;
  }
  if (rt->video_encoder != NULL) {
    gbmcd_close(&rt->video_encoder);
    rt->video_encoder_width = 0;
    rt->video_encoder_height = 0;
  }
  return media_open_video_encoder(rt);
}

static int media_send_encoded_audio(gb_agent_media_runtime_t *rt, const gbmcd_frame_t *packet) {
  if (rt == NULL || packet == NULL || packet->type != GBMCD_FRAME_PACKET || packet->size == 0) {
    return -1;
  }
#if GB_AGENT_USE_X2_GBSDK
  c_gb28181_api_t *api = NULL;
  int active = 0;
  int send_rc = -1;
  char send_channel_id[32] = {0};
  c_gb28181_api_frame_t frame;
  uint32_t timestamp = packet->pts_us >= 0 ? (uint32_t) ((uint64_t) packet->pts_us * 90ULL / 1000ULL) : 0;
  memset(&frame, 0, sizeof(frame));
  frame.media_type = C_GB28181_API_MEDIA_TYPE_AUDIO;
  frame.data = packet->data;
  frame.size = (uint32_t) packet->size;
  frame.timestamp = timestamp;
  frame.key_frame = false;
  gb_mutex_lock(&rt->agent->media_send_mu);
  gb_mutex_lock(&rt->agent->mu);
  if (rt->platform_idx >= 0 && rt->platform_idx < GB_AGENT_MAX_CHANNELS) {
    int api_idx = rt->agent->media_api_idx[rt->platform_idx];
    if (api_idx >= 0 && api_idx < GB_AGENT_MAX_CLIENTS) {
      api = rt->agent->clients[api_idx].api;
      active = rt->agent->channel_push_active[rt->platform_idx];
      copy_text(send_channel_id, sizeof(send_channel_id), rt->agent->media_channel_ids[rt->platform_idx]);
    }
  }
  gb_mutex_unlock(&rt->agent->mu);
  if (api != NULL && active && send_channel_id[0] != '\0') {
    send_rc = c_gb28181_api_send_channel_frame(api, send_channel_id, &frame);
    if (send_rc != 0) {
      char reason[128];
      snprintf(reason, sizeof(reason), "send audio frame failed rc=%d", send_rc);
      mark_media_send_error(rt->agent, rt->platform_idx, send_rc, reason);
    }
    if (!rt->logged_first_audio_packet || send_rc != 0) {
      char msg[256];
      snprintf(msg,
               sizeof(msg),
               "GB28181 send_channel_frame channel_id=%s audio first=%d size=%u ts=%u rc=%d",
               send_channel_id,
               !rt->logged_first_audio_packet ? 1 : 0,
               frame.size,
               frame.timestamp,
               send_rc);
      emit_log(rt->agent,
               -rt->platform_id,
               send_rc == 0 ? "INFO" : "ERROR",
               "RTP",
               msg);
      rt->logged_first_audio_packet = 1;
    }
  }
  gb_mutex_unlock(&rt->agent->media_send_mu);
#endif
  return 0;
}

static int media_send_encoded_video(gb_agent_media_runtime_t *rt, const gbmcd_frame_t *packet) {
  if (rt == NULL || packet == NULL || packet->type != GBMCD_FRAME_PACKET || packet->size == 0) {
    return -1;
  }
#if GB_AGENT_USE_X2_GBSDK
  c_gb28181_api_t *api = NULL;
  int active = 0;
  int send_rc = -1;
  int should_log_frame = 0;
  char send_channel_id[32] = {0};
  c_gb28181_api_frame_t frame;
  uint32_t timestamp = packet->pts_us >= 0 ? (uint32_t) ((uint64_t) packet->pts_us * 90ULL / 1000ULL) : 0;
  memset(&frame, 0, sizeof(frame));
  frame.media_type = C_GB28181_API_MEDIA_TYPE_VIDEO;
  frame.data = packet->data;
  frame.size = (uint32_t) packet->size;
  frame.timestamp = timestamp;
  frame.key_frame = packet->key_frame ? true : false;
  gb_mutex_lock(&rt->agent->media_send_mu);
  gb_mutex_lock(&rt->agent->mu);
  if (rt->platform_idx >= 0 && rt->platform_idx < GB_AGENT_MAX_CHANNELS) {
    int api_idx = rt->agent->media_api_idx[rt->platform_idx];
    if (api_idx >= 0 && api_idx < GB_AGENT_MAX_CLIENTS) {
      api = rt->agent->clients[api_idx].api;
      active = rt->agent->channel_push_active[rt->platform_idx];
      copy_text(send_channel_id, sizeof(send_channel_id), rt->agent->media_channel_ids[rt->platform_idx]);
    }
  }
  gb_mutex_unlock(&rt->agent->mu);
  if (api != NULL && active && send_channel_id[0] != '\0') {
    send_rc = c_gb28181_api_send_channel_frame(api, send_channel_id, &frame);
    if (send_rc == 0) {
      mark_media_frame_sent(rt->agent, rt->platform_idx);
    } else {
      char reason[128];
      snprintf(reason, sizeof(reason), "send frame failed rc=%d", send_rc);
      mark_media_send_error(rt->agent, rt->platform_idx, send_rc, reason);
    }
    should_log_frame = !rt->logged_first_video_packet ||
                       frame.key_frame ||
                       send_rc != 0;
    if (should_log_frame) {
      char msg[256];
      snprintf(msg,
               sizeof(msg),
               "GB28181 send_channel_frame channel_id=%s video %skey=%d size=%u ts=%u format=%s rc=%d",
               send_channel_id,
               !rt->logged_first_video_packet ? "first " : "",
               frame.key_frame ? 1 : 0,
               frame.size,
               frame.timestamp,
               encoded_packet_format(frame.data, frame.size),
               send_rc);
      emit_log(rt->agent,
               -rt->platform_id,
               send_rc == 0 ? "INFO" : "ERROR",
               "RTP",
               msg);
      rt->logged_first_video_packet = 1;
      if (frame.key_frame) rt->logged_key_video_packet = 1;
    }
  }
  gb_mutex_unlock(&rt->agent->media_send_mu);
#endif
  mark_media_frame_encoded(rt->agent, rt->platform_idx);
  return 0;
}

static int media_encode_video(gb_agent_media_runtime_t *rt,
                              const uint8_t *data,
                              size_t size,
                              int64_t pts_us,
                              int64_t duration_us,
                              int width,
                              int height,
                              const char *format,
                              int force_keyframe) {
  gbmcd_frame_t in;
  int rc;
  if (rt == NULL || rt->video_encoder == NULL || data == NULL || size == 0) return -1;
  memset(&in, 0, sizeof(in));
  in.type = GBMCD_FRAME_RAW;
  in.pts_us = pts_us;
  in.duration_us = duration_us;
  in.data = data;
  in.size = size;
  in.width = width;
  in.height = height;
  in.format = format && format[0] ? format : "yuv420p";
  in.key_frame = force_keyframe ? 1 : 0;
  rc = gbmcd_send_frame(rt->video_encoder, &in);
  if (rc != GBMCD_OK) {
    mark_media_encode_error(rt->agent, rt->platform_idx);
    return rc;
  }
  for (;;) {
    gbmcd_frame_t out;
    memset(&out, 0, sizeof(out));
    rc = gbmcd_receive_frame(rt->video_encoder, &out);
    if (rc == GBMCD_ERR_AGAIN) break;
    if (rc != GBMCD_OK) {
      mark_media_encode_error(rt->agent, rt->platform_idx);
      return rc;
    }
    (void) media_send_encoded_video(rt, &out);
    gbmcd_frame_release(&out);
  }
  return GBMCD_OK;
}

static int media_encode_silent_audio(gb_agent_media_runtime_t *rt,
                                     int64_t pts_us,
                                     int64_t duration_us) {
  int16_t pcm[160];
  gbmcd_frame_t in;
  int rc;
  if (rt == NULL || rt->audio_encoder == NULL) return -1;
  memset(pcm, 0, sizeof(pcm));
  memset(&in, 0, sizeof(in));
  in.type = GBMCD_FRAME_RAW;
  in.pts_us = pts_us;
  in.duration_us = duration_us;
  in.data = (const uint8_t *) pcm;
  in.size = sizeof(pcm);
  in.sample_rate = 8000;
  in.channels = 1;
  in.format = "s16";
  rc = gbmcd_send_frame(rt->audio_encoder, &in);
  if (rc != GBMCD_OK) return rc == GBMCD_ERR_AGAIN ? 0 : rc;
  for (;;) {
    gbmcd_frame_t out;
    memset(&out, 0, sizeof(out));
    rc = gbmcd_receive_frame(rt->audio_encoder, &out);
    if (rc == GBMCD_ERR_AGAIN) break;
    if (rc != GBMCD_OK) return rc;
    (void) media_send_encoded_audio(rt, &out);
    gbmcd_frame_release(&out);
  }
  return GBMCD_OK;
}

static const uint8_t *status_glyph5x7(char ch) {
  static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
  static const uint8_t n[7] = {17, 25, 21, 19, 17, 17, 17};
  static const uint8_t o[7] = {14, 17, 17, 17, 17, 17, 14};
  static const uint8_t s[7] = {15, 16, 16, 14, 1, 1, 30};
  static const uint8_t i[7] = {14, 4, 4, 4, 4, 4, 14};
  static const uint8_t g[7] = {14, 17, 16, 23, 17, 17, 14};
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

static void draw_status_text_y(uint8_t *y, int width, int height, int stride, const char *text) {
  int len;
  int scale;
  int glyph_w;
  int glyph_h;
  int gap;
  int x0;
  int y0;
  if (y == NULL || width <= 0 || height <= 0 || stride <= 0 || text == NULL) return;
  len = (int) strlen(text);
  scale = width >= 900 ? 14 : 8;
  glyph_w = 5 * scale;
  glyph_h = 7 * scale;
  gap = scale;
  x0 = (width - (len * glyph_w + (len - 1) * gap)) / 2;
  y0 = (height - glyph_h) / 2;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  for (int c = 0; c < len; c++) {
    const uint8_t *glyph = status_glyph5x7(text[c]);
    int gx = x0 + c * (glyph_w + gap);
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 5; col++) {
        if ((glyph[row] & (1 << (4 - col))) == 0) continue;
        for (int sy = 0; sy < scale; sy++) {
          int py = y0 + row * scale + sy;
          if (py < 0 || py >= height) continue;
          for (int sx = 0; sx < scale; sx++) {
            int px = gx + col * scale + sx;
            if (px >= 0 && px < width) y[py * stride + px] = 220;
          }
        }
      }
    }
  }
}

static int media_emit_cached_yuv_frame(gb_agent_media_runtime_t *rt,
                                       const char *text,
                                       int draw_text,
                                       int64_t pts_us,
                                       int64_t duration_us) {
  int width = 1280;
  int height = 720;
  size_t y_size;
  size_t uv_size;
  size_t frame_size;
  const char *cache_text;
  if (rt == NULL) return -1;
  (void) parse_resolution(rt->source.resolution, &width, &height);
  if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
    width = 1280;
    height = 720;
  }
  cache_text = draw_text && text != NULL ? text : "";
  y_size = (size_t) width * (size_t) height;
  uv_size = (size_t) (width / 2) * (size_t) (height / 2);
  frame_size = y_size + uv_size * 2;
  if (rt->status_frame == NULL ||
      rt->status_frame_size != frame_size ||
      rt->status_frame_width != width ||
      rt->status_frame_height != height ||
      strcmp(rt->status_frame_text, cache_text) != 0) {
    uint8_t *frame = (uint8_t *) malloc(frame_size);
    if (frame == NULL) return -1;
    memset(frame, 16, y_size);
    memset(frame + y_size, 128, uv_size * 2);
    if (draw_text) {
      draw_status_text_y(frame, width, height, width, text != NULL ? text : "NO SIGNAL");
    }
    free(rt->status_frame);
    rt->status_frame = frame;
    rt->status_frame_size = frame_size;
    rt->status_frame_width = width;
    rt->status_frame_height = height;
    copy_text(rt->status_frame_text, sizeof(rt->status_frame_text), cache_text);
  }
  return media_encode_video(rt, rt->status_frame, rt->status_frame_size, pts_us, duration_us, width, height, "yuv420p", 0);
}

static int media_emit_black_frame(gb_agent_media_runtime_t *rt, int64_t pts_us, int64_t duration_us) {
  return media_emit_cached_yuv_frame(rt, "", 0, pts_us, duration_us);
}

static int media_emit_no_source_frame(gb_agent_media_runtime_t *rt, int64_t pts_us, int64_t duration_us) {
  return media_emit_cached_yuv_frame(rt, "NO SOURCE", 1, pts_us, duration_us);
}

static int media_emit_no_signal_frame(gb_agent_media_runtime_t *rt, int64_t pts_us, int64_t duration_us) {
  return media_emit_cached_yuv_frame(rt, "NO SIGNAL", 1, pts_us, duration_us);
}

static void media_raw_snapshot_clear(gb_agent_raw_snapshot_t *snap) {
  if (snap == NULL) return;
  free(snap->data);
  memset(snap, 0, sizeof(*snap));
}

static int media_snapshot_latest_raw(gb_agent_media_runtime_t *rt, gb_agent_raw_snapshot_t *snap) {
  if (rt == NULL) return -1;
  if (snap == NULL) return -1;
  memset(snap, 0, sizeof(*snap));
  if (!gb_source_snapshot_raw_meta(rt->source_key,
                                   &snap->data,
                                   &snap->size,
                                   &snap->width,
                                   &snap->height,
                                   snap->format,
                                   sizeof(snap->format),
                                   &snap->generation,
                                   &snap->updated_at)) {
    return -1;
  }
  return 0;
}

static int media_encode_raw_snapshot(gb_agent_media_runtime_t *rt,
                                     const gb_agent_raw_snapshot_t *snap,
                                     int64_t pts_us,
                                     int64_t duration_us,
                                     int force_keyframe) {
  if (rt == NULL || snap == NULL || snap->data == NULL || snap->size == 0) return -1;
  if (media_ensure_video_encoder(rt) != 0) return -1;
  return media_encode_video(rt,
                            snap->data,
                            snap->size,
                            pts_us,
                            duration_us,
                            snap->width,
                            snap->height,
                            snap->format,
                            force_keyframe);
}

static int media_open_source(gb_agent_media_runtime_t *rt) {
  int refs;
  gb_source_runtime_config_t source_cfg;
  if (rt == NULL) return -1;
  if (strcmp(rt->source.source_mode, "none") == 0) {
    gb_source_key(rt->source_key, sizeof(rt->source_key), "none", "no-source");
  } else if (strcmp(rt->source.source_mode, "file") == 0) {
    gb_source_key(rt->source_key, sizeof(rt->source_key), "file", rt->source.media_file);
  } else if (strcmp(rt->source.source_mode, "screen") == 0) {
    gb_source_key(rt->source_key, sizeof(rt->source_key), "screen", rt->source.video_device);
  } else {
    gb_source_key(rt->source_key, sizeof(rt->source_key), "device", rt->source.video_device);
  }
  memset(&source_cfg, 0, sizeof(source_cfg));
  copy_text(source_cfg.source_mode, sizeof(source_cfg.source_mode), rt->source.source_mode);
  copy_text(source_cfg.backend_name, sizeof(source_cfg.backend_name), media_capture_backend_name(&rt->source));
  copy_text(source_cfg.video_device, sizeof(source_cfg.video_device), rt->source.video_device);
  source_cfg.audio_device[0] = '\0';
  copy_text(source_cfg.media_file, sizeof(source_cfg.media_file), rt->source.media_file);
  copy_text(source_cfg.resolution, sizeof(source_cfg.resolution), rt->source.resolution);
  source_cfg.bitrate_kbps = rt->source.bitrate_kbps;
  source_cfg.fps = strcmp(rt->source.source_mode, "file") == 0 ? rt->source.fps : 0;
  source_cfg.loop = rt->source.loop;
  copy_text(source_cfg.file_pacing, sizeof(source_cfg.file_pacing), rt->source.file_pacing);
  refs = gb_source_acquire_runtime(rt->source_key, "agent", &source_cfg);
  if (refs < 0) return -1;
  rt->source_acquired = 1;
  if (media_open_video_encoder(rt) != GBMCD_OK) {
    (void) gb_source_release(rt->source_key, "agent");
    rt->source_acquired = 0;
    return -1;
  }
  return 0;
}

static void media_close_source(gb_agent_media_runtime_t *rt) {
  if (rt == NULL) return;
  if (rt->video_encoder) gbmcd_close(&rt->video_encoder);
  if (rt->audio_encoder) gbmcd_close(&rt->audio_encoder);
  free(rt->status_frame);
  rt->status_frame = NULL;
  rt->status_frame_size = 0;
  rt->status_frame_width = 0;
  rt->status_frame_height = 0;
  rt->status_frame_text[0] = '\0';
  if (rt->source_acquired) {
    (void) gb_source_release(rt->source_key, "agent");
    rt->source_acquired = 0;
  }
}

static void *media_thread_main(void *arg) {
  gb_agent_media_runtime_t *rt = (gb_agent_media_runtime_t *) arg;
  char msg[512];
  if (rt == NULL) return NULL;
  snprintf(msg, sizeof(msg), "media source thread starting channel=%d mode=%s file=%s video=%s",
           rt->platform_id, rt->source.source_mode, rt->source.media_file, rt->source.video_device);
  emit_log(rt->agent, -rt->platform_id, "INFO", "MEDIA", msg);
  int source_ready = media_open_source(rt) == 0;
  if (!source_ready) {
    if (rt->video_encoder == NULL && media_open_video_encoder(rt) != GBMCD_OK) {
      mark_client_media_running(rt->agent, rt->client_idx, rt->generation, 0, "media encoder open failed");
      emit_log(rt->agent, -rt->platform_id, "ERROR", "MEDIA", "media encoder open failed");
      media_close_source(rt);
      free(rt);
      return NULL;
    }
    emit_log(rt->agent, -rt->platform_id, "ERROR", "MEDIA", "media source open failed; sending no signal");
  }
  if (media_open_audio_encoder(rt) != GBMCD_OK) {
    emit_log(rt->agent, -rt->platform_id, "ERROR", "MEDIA", "audio encoder open failed; silent audio disabled");
  }
  mark_client_media_running(rt->agent,
                            rt->client_idx,
                            rt->generation,
                            1,
                            source_ready ? "media source shared" : "media source no signal");
  int64_t audio_pts_us = 0;
  int64_t video_pts_us = 0;
  int output_fps = rt->source.fps > 0 ? rt->source.fps : 25;
  int64_t output_duration_us = 1000000LL / output_fps;
  const int64_t audio_duration_us = 20000;
  const int64_t source_stale_us = 1000000;
  const int audio_sleep_ms = 20;
  unsigned long last_raw_generation = 0;
  int have_raw_generation = 0;
  int64_t last_raw_refresh_pts_us = -1;
  int force_next_real_keyframe = 1;
  while (media_generation_current(rt->agent, rt->platform_idx, rt->generation)) {
    if (rt->audio_encoder != NULL) {
      (void) media_encode_silent_audio(rt, audio_pts_us, audio_duration_us);
    }
    if (video_pts_us <= audio_pts_us) {
      if (strcmp(rt->source.source_mode, "none") == 0) {
        (void) media_emit_no_source_frame(rt, video_pts_us, output_duration_us);
      } else {
        gb_agent_raw_snapshot_t raw;
        int have_snapshot = media_snapshot_latest_raw(rt, &raw) == 0;
        int raw_is_fresh_enough = 0;
        if (have_snapshot) {
          if (!have_raw_generation || raw.generation != last_raw_generation) {
            last_raw_generation = raw.generation;
            have_raw_generation = 1;
            last_raw_refresh_pts_us = audio_pts_us;
            if (!rt->logged_first_raw_frame) {
              char raw_msg[256];
              snprintf(raw_msg,
                       sizeof(raw_msg),
                       "source raw first width=%d height=%d format=%s size=%llu generation=%lu",
                       raw.width,
                       raw.height,
                       raw.format[0] ? raw.format : "",
                       (unsigned long long) raw.size,
                       raw.generation);
              emit_log(rt->agent, -rt->platform_id, "INFO", "MEDIA", raw_msg);
              rt->logged_first_raw_frame = 1;
            }
          }
          raw_is_fresh_enough = have_raw_generation &&
                                last_raw_refresh_pts_us >= 0 &&
                                audio_pts_us - last_raw_refresh_pts_us <= source_stale_us;
        }
        if (have_snapshot && raw_is_fresh_enough) {
          (void) media_encode_raw_snapshot(rt,
                                           &raw,
                                           video_pts_us,
                                           output_duration_us,
                                           force_next_real_keyframe);
          force_next_real_keyframe = 0;
        } else if (audio_pts_us >= source_stale_us) {
          (void) media_emit_no_signal_frame(rt, video_pts_us, output_duration_us);
          force_next_real_keyframe = 1;
        } else {
          (void) media_emit_black_frame(rt, video_pts_us, output_duration_us);
          force_next_real_keyframe = 1;
        }
        media_raw_snapshot_clear(have_snapshot ? &raw : NULL);
      }
      video_pts_us += output_duration_us;
    }
    audio_pts_us += audio_duration_us;
    gb_sleep_ms(audio_sleep_ms);
  }
  media_close_source(rt);
  mark_client_media_running(rt->agent, rt->client_idx, rt->generation, 0, "media source stopped");
  emit_log(rt->agent, -rt->platform_id, "INFO", "MEDIA", "media source thread stopped");
  free(rt);
  return NULL;
}

static int restart_media_thread(gb_agent_t *agent, int platform_id) {
  gb_agent_media_runtime_t *rt;
  gb_thread_t old_thread;
  int join_old = 0;
  int idx = media_index(platform_id);
  if (agent == NULL || idx < 0) return -1;
  rt = (gb_agent_media_runtime_t *) calloc(1, sizeof(*rt));
  if (rt == NULL) return -1;
  gb_mutex_lock(&agent->mu);
  if (agent->media_thread_running[idx]) {
    old_thread = agent->media_threads[idx];
    join_old = 1;
    agent->media_thread_running[idx] = 0;
  }
  agent->media_generations[idx]++;
  rt->agent = agent;
  rt->platform_id = platform_id;
  rt->platform_idx = idx;
  rt->client_idx = agent->media_api_idx[idx];
  rt->source = agent->media_sources[idx];
  rt->generation = agent->media_generations[idx];
  gb_mutex_unlock(&agent->mu);
  if (join_old) gb_thread_join(old_thread);
  gb_mutex_lock(&agent->mu);
  agent->media_thread_running[idx] = 1;
  if (gb_thread_create(&agent->media_threads[idx], media_thread_main, rt) != 0) {
    agent->media_thread_running[idx] = 0;
    gb_mutex_unlock(&agent->mu);
    free(rt);
    return -1;
  }
  gb_mutex_unlock(&agent->mu);
  return 0;
}

static int client_any_media_thread_running_locked(gb_agent_t *agent, int client_idx) {
  int first = client_idx * GB_AGENT_MAX_CHANNELS_PER_CLIENT;
  int last = first + GB_AGENT_MAX_CHANNELS_PER_CLIENT;
  if (agent == NULL || client_idx < 0 || client_idx >= GB_AGENT_MAX_CLIENTS) return 0;
  if (last > GB_AGENT_MAX_CHANNELS) last = GB_AGENT_MAX_CHANNELS;
  for (int i = first; i < last; i++) {
    if (agent->media_thread_running[i]) return 1;
  }
  return 0;
}

static void clear_client_channel_runtime_locked(gb_agent_t *agent, int client_idx) {
  if (agent == NULL || client_idx < 0 || client_idx >= GB_AGENT_MAX_CLIENTS) return;
  for (int i = 0; i < GB_AGENT_MAX_CHANNELS; i++) {
    if (agent->media_api_idx[i] == client_idx) {
      agent->media_api_idx[i] = -1;
      agent->media_channel_ids[i][0] = '\0';
      agent->channel_push_active[i] = 0;
      agent->channel_talkback_active[i] = 0;
      agent->channel_broadcast_active[i] = 0;
    }
  }
}

#if GB_AGENT_USE_X2_GBSDK
static const char *sdk_state_name(c_gb28181_api_state_t state) {
  switch (state) {
    case C_GB28181_API_STATE_IDLE: return "IDLE";
    case C_GB28181_API_STATE_STARTING: return "STARTING";
    case C_GB28181_API_STATE_CONNECTING: return "CONNECTING";
    case C_GB28181_API_STATE_REGISTERING: return "REGISTERING";
    case C_GB28181_API_STATE_REGISTERED: return "REGISTERED";
    case C_GB28181_API_STATE_PUSHING: return "PUSHING";
    case C_GB28181_API_STATE_TALKBACK: return "TALKBACK";
    case C_GB28181_API_STATE_STOPPING: return "STOPPING";
    case C_GB28181_API_STATE_STOPPED: return "STOPPED";
    case C_GB28181_API_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

static c_gb28181_api_sip_transport_t sdk_transport(const char *transport) {
  if (transport != NULL && strcmp(transport, "TCP") == 0) return C_GB28181_API_SIP_TRANSPORT_TCP;
  if (transport != NULL && strcmp(transport, "TrUdp") == 0) return C_GB28181_API_SIP_TRANSPORT_TRUDP;
  return C_GB28181_API_SIP_TRANSPORT_UDP;
}

static c_gb28181_api_media_proto_t sdk_media_proto(const char *media_proto) {
  if (media_proto != NULL && strcmp(media_proto, "RTP") == 0) return C_GB28181_API_MEDIA_PROTO_RTP;
  return C_GB28181_API_MEDIA_PROTO_RTC;
}

static void derive_domain(const char *sip_id, char *out, size_t out_size) {
  size_t n = sip_id != NULL ? strlen(sip_id) : 0;
  if (out == NULL || out_size == 0) return;
  if (n >= 10 && out_size > 10) {
    memcpy(out, sip_id, 10);
    out[10] = '\0';
  } else {
    copy_text(out, out_size, sip_id);
  }
}

static void derive_channel_id(const char *base_id, int ordinal, char *out, size_t out_size) {
  char prefix[15];
  if (out == NULL || out_size == 0) return;
  memset(prefix, 0, sizeof(prefix));
  if (base_id != NULL && strlen(base_id) >= 14) {
    memcpy(prefix, base_id, 14);
  } else {
    snprintf(prefix, sizeof(prefix), "%014d", 0);
  }
  snprintf(out, out_size, "%s%06d", prefix, ordinal);
}

static int channel_id_to_ordinal(const char *channel_id) {
  size_t n;
  int value;
  if (channel_id == NULL) return 0;
  n = strlen(channel_id);
  if (n < 6) return 0;
  value = atoi(channel_id + n - 6);
  return value >= 1 && value <= GB_AGENT_MAX_CHANNELS_PER_CLIENT ? value : 0;
}

static int channel_callback_media_id(gb_agent_t *agent,
                                     const gb_agent_cb_ctx_t *ctx,
                                     const char *channel_id,
                                     int *client_idx_out);

static int find_channel_media_id_locked(gb_agent_t *agent, int client_idx, const char *channel_id) {
  if (agent == NULL || client_idx < 0 || client_idx >= GB_AGENT_MAX_CLIENTS ||
      channel_id == NULL || channel_id[0] == '\0') {
    return 0;
  }
  for (int i = 0; i < agent->clients[client_idx].channel_count; i++) {
    const gb_agent_channel_t *ch = &agent->clients[client_idx].channels[i];
    if (ch->channel_id[0] != '\0' && strcmp(ch->channel_id, channel_id) == 0) return ch->id;
  }
  return 0;
}

static int client_any_channel_push_active_locked(gb_agent_t *agent, int client_idx) {
  if (agent == NULL || client_idx < 0 || client_idx >= GB_AGENT_MAX_CLIENTS) return 0;
  for (int i = 0; i < GB_AGENT_MAX_CHANNELS; i++) {
    if (agent->media_api_idx[i] == client_idx && agent->channel_push_active[i]) return 1;
  }
  return 0;
}

static void on_state(void *user_data,
                     c_gb28181_api_t *api,
                     c_gb28181_api_state_t old_state,
                     c_gb28181_api_state_t new_state,
                     const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int idx = ctx != NULL ? client_index(ctx->platform_id) : -1;
  (void) api;
  (void) old_state;
  if (agent == NULL || idx < 0) return;
  gb_mutex_lock(&agent->mu);
  if (!callback_is_current(agent, ctx, idx)) {
    gb_mutex_unlock(&agent->mu);
    return;
  }
  copy_text(agent->clients[idx].status.sdk_state,
            sizeof(agent->clients[idx].status.sdk_state),
            sdk_state_name(new_state));
  if (new_state == C_GB28181_API_STATE_REGISTERED ||
      new_state == C_GB28181_API_STATE_PUSHING ||
      new_state == C_GB28181_API_STATE_TALKBACK) {
    agent->clients[idx].status.registered = 1;
  } else if (new_state == C_GB28181_API_STATE_STOPPED) {
    agent->clients[idx].status.registered = 0;
  }
  set_reason(&agent->clients[idx].status, reason);
  gb_mutex_unlock(&agent->mu);
}

static void on_registered(void *user_data,
                          c_gb28181_api_t *api,
                          bool ok,
                          int code,
                          const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int idx = ctx != NULL ? client_index(ctx->platform_id) : -1;
  char msg[256];
  (void) api;
  if (agent == NULL || idx < 0) return;
  gb_mutex_lock(&agent->mu);
  if (!callback_is_current(agent, ctx, idx)) {
    gb_mutex_unlock(&agent->mu);
    return;
  }
  agent->clients[idx].status.registered = ok ? 1 : 0;
  agent->clients[idx].status.register_code = code;
  if (ok) {
    agent->clients[idx].status.last_error_code = 0;
    agent->clients[idx].reconnect_at = 0;
    agent->clients[idx].reconnect_attempts = 0;
  }
  set_reason(&agent->clients[idx].status, reason);
  gb_mutex_unlock(&agent->mu);
  snprintf(msg, sizeof(msg), "platform %d register ok=%d code=%d reason=%s",
           ctx->platform_id, ok ? 1 : 0, code, reason != NULL ? reason : "");
  emit_log(agent, ctx->platform_id, ok ? "INFO" : "ERROR", "SIP", msg);
}

static void on_keepalive(void *user_data, c_gb28181_api_t *api, bool ok) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int idx = ctx != NULL ? client_index(ctx->platform_id) : -1;
  (void) api;
  if (agent == NULL || idx < 0) return;
  gb_mutex_lock(&agent->mu);
  if (!callback_is_current(agent, ctx, idx)) {
    gb_mutex_unlock(&agent->mu);
    return;
  }
  agent->clients[idx].status.keepalive_ok = ok ? 1 : 0;
  if (ok) agent->clients[idx].status.keepalive_ok_count++;
  agent->clients[idx].status.updated_at = time(NULL);
  gb_mutex_unlock(&agent->mu);
}

static void on_error(void *user_data, c_gb28181_api_t *api, int code, const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int idx = ctx != NULL ? client_index(ctx->platform_id) : -1;
  char msg[256];
  (void) api;
  if (agent == NULL || idx < 0) return;
  gb_mutex_lock(&agent->mu);
  if (!callback_is_current(agent, ctx, idx)) {
    gb_mutex_unlock(&agent->mu);
    return;
  }
  agent->clients[idx].status.last_error_code = code;
  agent->clients[idx].status.registered = 0;
  agent->clients[idx].status.keepalive_ok = 0;
  agent->clients[idx].status.push_active = 0;
  clear_client_channel_runtime_locked(agent, idx);
  if (agent->clients[idx].status.desired_enabled) {
    agent->clients[idx].reconnect_attempts++;
    agent->clients[idx].reconnect_at = time(NULL) + 3;
  }
  copy_text(agent->clients[idx].status.sdk_state,
            sizeof(agent->clients[idx].status.sdk_state),
            "ERROR");
  set_reason(&agent->clients[idx].status, reason);
  gb_mutex_unlock(&agent->mu);
  (void) gb_agent_stop_client_media_sources(agent, idx);
  snprintf(msg, sizeof(msg), "platform %d error code=%d reason=%s",
           ctx->platform_id, code, reason != NULL ? reason : "");
  emit_log(agent, ctx->platform_id, "ERROR", "SIP", msg);
}

static void on_message(void *user_data, c_gb28181_api_t *api, const char *data, size_t len) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  snprintf(msg,
           sizeof(msg),
           "GB28181 message received len=%llu",
           (unsigned long long) len);
  emit_log(agent, ctx->platform_id, "INFO", "SIP", msg);
  (void) data;
}

static void on_sip_message(void *user_data,
                           c_gb28181_api_t *api,
                           const char *channel_id,
                           const char *cmd_type,
                           const char *body) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  char msg[512];
  size_t body_len = body != NULL ? strlen(body) : 0;
  int client_idx = -1;
  int media_id;
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  if (channel_id != NULL && channel_id[0] != '\0' && media_id <= 0) {
    snprintf(msg,
             sizeof(msg),
             "SIP message ignored: unconfigured channel_id=%s cmd=%s",
             channel_id,
             cmd_type != NULL ? cmd_type : "");
    emit_log(agent, ctx->platform_id, "WARN", "SIP", msg);
    return;
  }
  snprintf(msg,
           sizeof(msg),
           "SIP message channel_id=%s cmd=%s body_len=%llu",
           channel_id != NULL ? channel_id : "",
           cmd_type != NULL ? cmd_type : "",
           (unsigned long long) body_len);
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "SIP", msg);
}

static void on_channel_video_bitrate_update(void *user_data,
                                            c_gb28181_api_t *api,
                                            const char *channel_id,
                                            uint32_t bitrate) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  (void) client_idx;
  if (agent == NULL || ctx == NULL) return;
  if (media_id <= 0) {
    snprintf(msg, sizeof(msg), "bitrate update ignored: unconfigured channel_id=%s bitrate=%u",
             channel_id != NULL ? channel_id : "", bitrate);
    emit_log(agent, ctx->platform_id, "WARN", "RTP", msg);
    return;
  }
  snprintf(msg, sizeof(msg), "bitrate update channel_id=%s bitrate=%u",
           channel_id != NULL ? channel_id : "", bitrate);
  emit_log(agent, -media_id, "INFO", "RTP", msg);
}

static void on_device_control(void *user_data,
                              c_gb28181_api_t *api,
                              const char *channel_id,
                              const char *device_id,
                              c_gb28181_api_device_control_type_t control_type,
                              const char *raw_value) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  (void) client_idx;
  if (agent == NULL || ctx == NULL) return;
  if (channel_id != NULL && channel_id[0] != '\0' && media_id <= 0) {
    snprintf(msg, sizeof(msg), "device control ignored: unconfigured channel_id=%s type=%d",
             channel_id, (int) control_type);
    emit_log(agent, ctx->platform_id, "WARN", "SIP", msg);
    return;
  }
  snprintf(msg, sizeof(msg), "device control channel_id=%s device_id=%s type=%d value=%s",
           channel_id != NULL ? channel_id : "",
           device_id != NULL ? device_id : "",
           (int) control_type,
           raw_value != NULL ? raw_value : "");
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "SIP", msg);
}

static int on_record_info(void *user_data,
                          c_gb28181_api_t *api,
                          const char *channel_id,
                          const c_gb28181_api_record_info_request_t *request,
                          uint32_t item_index,
                          c_gb28181_api_record_item_t *item_out) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  (void) request;
  (void) item_index;
  (void) item_out;
  if (agent == NULL || ctx == NULL) return 0;
  if (channel_id != NULL && channel_id[0] != '\0' && media_id <= 0) {
    snprintf(msg, sizeof(msg), "record query ignored: unconfigured channel_id=%s", channel_id);
    emit_log(agent, ctx->platform_id, "WARN", "RECORD", msg);
    return 0;
  }
  snprintf(msg, sizeof(msg), "record query channel_id=%s item_index=%u",
           channel_id != NULL ? channel_id : "", item_index);
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "RECORD", msg);
  return 0;
}

static void on_channel_push_started(void *user_data,
                                    c_gb28181_api_t *api,
                                    const char *channel_id,
                                    const c_gb28181_api_push_config_t *config) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int media_id = 0;
  char msg[256];
  (void) api;
  (void) config;
  if (agent == NULL || ctx == NULL) return;
  int idx = client_index(ctx->platform_id);
  if (idx < 0) return;
  gb_mutex_lock(&agent->mu);
  if (!callback_is_current(agent, ctx, idx)) {
    gb_mutex_unlock(&agent->mu);
    return;
  }
  media_id = find_channel_media_id_locked(agent, idx, channel_id);
  if (media_id <= 0 || media_id > GB_AGENT_MAX_CHANNELS) {
    gb_mutex_unlock(&agent->mu);
    snprintf(msg, sizeof(msg), "GB28181 channel push ignored: unconfigured channel_id=%s",
             channel_id != NULL ? channel_id : "");
    emit_log(agent, ctx->platform_id, "WARN", "RTP", msg);
    return;
  }
  agent->media_api_idx[media_id - 1] = idx;
  agent->channel_push_active[media_id - 1] = 1;
  copy_text(agent->media_channel_ids[media_id - 1], sizeof(agent->media_channel_ids[media_id - 1]), channel_id);
  agent->clients[idx].status.push_active = 1;
  copy_text(agent->clients[idx].status.sdk_state,
            sizeof(agent->clients[idx].status.sdk_state),
            "PUSHING");
  agent->clients[idx].status.updated_at = time(NULL);
  gb_mutex_unlock(&agent->mu);
  snprintf(msg, sizeof(msg), "GB28181 channel push started channel_id=%s media_channel=%d",
           channel_id != NULL ? channel_id : "",
           media_id);
  emit_log(agent, -media_id, "INFO", "RTP", msg);
  (void) restart_media_thread(agent, media_id);
}

static void on_channel_push_stopped(void *user_data,
                                    c_gb28181_api_t *api,
                                    const char *channel_id,
                                    const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int media_id = 0;
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  int idx = client_index(ctx->platform_id);
  if (idx >= 0) {
    gb_mutex_lock(&agent->mu);
    if (callback_is_current(agent, ctx, idx)) {
      media_id = find_channel_media_id_locked(agent, idx, channel_id);
      if (media_id > 0 && media_id <= GB_AGENT_MAX_CHANNELS) {
        agent->channel_push_active[media_id - 1] = 0;
        agent->media_api_idx[media_id - 1] = -1;
        agent->media_channel_ids[media_id - 1][0] = '\0';
      }
      agent->clients[idx].status.push_active = client_any_channel_push_active_locked(agent, idx);
      set_reason(&agent->clients[idx].status, reason);
    }
    gb_mutex_unlock(&agent->mu);
  }
  if (media_id > 0) (void) gb_agent_stop_platform_media_source(agent, media_id);
  snprintf(msg, sizeof(msg), "GB28181 channel push stopped channel_id=%s reason=%s",
           channel_id != NULL ? channel_id : "",
           reason != NULL ? reason : "");
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "RTP", msg);
}

static int channel_callback_media_id(gb_agent_t *agent,
                                     const gb_agent_cb_ctx_t *ctx,
                                     const char *channel_id,
                                     int *client_idx_out) {
  int idx = ctx != NULL ? client_index(ctx->platform_id) : -1;
  int media_id = 0;
  if (client_idx_out) *client_idx_out = idx;
  if (agent == NULL || ctx == NULL || idx < 0) return 0;
  gb_mutex_lock(&agent->mu);
  if (callback_is_current(agent, ctx, idx)) media_id = find_channel_media_id_locked(agent, idx, channel_id);
  gb_mutex_unlock(&agent->mu);
  return media_id;
}

static void on_channel_talkback_invite(void *user_data,
                                       c_gb28181_api_t *api,
                                       const char *channel_id,
                                       uint32_t invite_id,
                                       const char *from_id,
                                       const char *sdp_body) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  (void) sdp_body;
  if (agent == NULL || ctx == NULL) return;
  if (media_id <= 0) {
    snprintf(msg, sizeof(msg), "GB28181 talkback invite ignored: unconfigured channel_id=%s invite=%u",
             channel_id != NULL ? channel_id : "", invite_id);
    emit_log(agent, ctx->platform_id, "WARN", "TALKBACK", msg);
    return;
  }
  gb_mutex_lock(&agent->mu);
  if (client_idx >= 0 && callback_is_current(agent, ctx, client_idx)) {
    agent->channel_talkback_active[media_id - 1] = 1;
  }
  gb_mutex_unlock(&agent->mu);
  snprintf(msg, sizeof(msg), "GB28181 talkback invite channel_id=%s invite=%u from=%s",
           channel_id != NULL ? channel_id : "", invite_id, from_id != NULL ? from_id : "");
  emit_log(agent, -media_id, "INFO", "TALKBACK", msg);
}

static void on_channel_talkback_canceled(void *user_data,
                                         c_gb28181_api_t *api,
                                         const char *channel_id,
                                         uint32_t invite_id,
                                         const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  if (media_id > 0) {
    gb_mutex_lock(&agent->mu);
    if (client_idx >= 0 && callback_is_current(agent, ctx, client_idx)) {
      agent->channel_talkback_active[media_id - 1] = 0;
    }
    gb_mutex_unlock(&agent->mu);
  }
  snprintf(msg, sizeof(msg), "GB28181 talkback canceled channel_id=%s invite=%u reason=%s",
           channel_id != NULL ? channel_id : "", invite_id, reason != NULL ? reason : "");
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "TALKBACK", msg);
}

static void on_channel_talkback_started(void *user_data,
                                        c_gb28181_api_t *api,
                                        const char *channel_id,
                                        const char *remote_ip,
                                        int remote_port) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  if (media_id > 0) {
    gb_mutex_lock(&agent->mu);
    if (client_idx >= 0 && callback_is_current(agent, ctx, client_idx)) {
      agent->channel_talkback_active[media_id - 1] = 1;
    }
    gb_mutex_unlock(&agent->mu);
  }
  snprintf(msg, sizeof(msg), "GB28181 talkback started channel_id=%s remote=%s:%d",
           channel_id != NULL ? channel_id : "", remote_ip != NULL ? remote_ip : "", remote_port);
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "TALKBACK", msg);
}

static void on_channel_talkback_stopped(void *user_data,
                                        c_gb28181_api_t *api,
                                        const char *channel_id,
                                        const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  if (media_id > 0) {
    gb_mutex_lock(&agent->mu);
    if (client_idx >= 0 && callback_is_current(agent, ctx, client_idx)) {
      agent->channel_talkback_active[media_id - 1] = 0;
    }
    gb_mutex_unlock(&agent->mu);
  }
  snprintf(msg, sizeof(msg), "GB28181 talkback stopped channel_id=%s reason=%s",
           channel_id != NULL ? channel_id : "", reason != NULL ? reason : "");
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "TALKBACK", msg);
}

static void on_channel_broadcast_started(void *user_data,
                                         c_gb28181_api_t *api,
                                         const char *channel_id,
                                         const char *remote_ip,
                                         int remote_port) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  if (media_id <= 0) {
    snprintf(msg, sizeof(msg), "GB28181 broadcast ignored: unconfigured channel_id=%s",
             channel_id != NULL ? channel_id : "");
    emit_log(agent, ctx->platform_id, "WARN", "BROADCAST", msg);
    return;
  }
  gb_mutex_lock(&agent->mu);
  if (client_idx >= 0 && callback_is_current(agent, ctx, client_idx)) {
    agent->channel_broadcast_active[media_id - 1] = 1;
  }
  gb_mutex_unlock(&agent->mu);
  snprintf(msg, sizeof(msg), "GB28181 broadcast started channel_id=%s remote=%s:%d",
           channel_id != NULL ? channel_id : "", remote_ip != NULL ? remote_ip : "", remote_port);
  emit_log(agent, -media_id, "INFO", "BROADCAST", msg);
}

static void on_channel_broadcast_stopped(void *user_data,
                                         c_gb28181_api_t *api,
                                         const char *channel_id,
                                         const char *reason) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  if (agent == NULL || ctx == NULL) return;
  if (media_id > 0) {
    gb_mutex_lock(&agent->mu);
    if (client_idx >= 0 && callback_is_current(agent, ctx, client_idx)) {
      agent->channel_broadcast_active[media_id - 1] = 0;
    }
    gb_mutex_unlock(&agent->mu);
  }
  snprintf(msg, sizeof(msg), "GB28181 broadcast stopped channel_id=%s reason=%s",
           channel_id != NULL ? channel_id : "", reason != NULL ? reason : "");
  emit_log(agent, media_id > 0 ? -media_id : ctx->platform_id, "INFO", "BROADCAST", msg);
}

static void on_channel_frame(void *user_data,
                             c_gb28181_api_t *api,
                             const char *channel_id,
                             const c_gb28181_api_frame_t *frame) {
  gb_agent_cb_ctx_t *ctx = (gb_agent_cb_ctx_t *) user_data;
  gb_agent_t *agent = ctx != NULL ? ctx->agent : NULL;
  int client_idx = -1;
  int media_id = channel_callback_media_id(agent, ctx, channel_id, &client_idx);
  char msg[256];
  (void) api;
  (void) client_idx;
  if (agent == NULL || ctx == NULL || frame == NULL) return;
  if (media_id <= 0) {
    snprintf(msg, sizeof(msg), "GB28181 channel frame ignored: unconfigured channel_id=%s size=%u",
             channel_id != NULL ? channel_id : "", frame->size);
    emit_log(agent, ctx->platform_id, "WARN", "MEDIA", msg);
  }
}

static void fill_sdk_config(gb_agent_client_t *client) {
  gb_agent_platform_t *p = &client->cfg;
  c_gb28181_api_config_t *cfg = &client->sdk_cfg;
  c_gb28181_api_media_proto_t media_proto;
  memset(cfg, 0, sizeof(*cfg));
  memset(client->sdk_channels, 0, sizeof(client->sdk_channels));
  memset(client->channel_ids, 0, sizeof(client->channel_ids));
  memset(client->channel_names, 0, sizeof(client->channel_names));
  derive_domain(p->sip_id, client->server_domain, sizeof(client->server_domain));
  copy_text(client->local_ip, sizeof(client->local_ip), "0.0.0.0");
  media_proto = sdk_media_proto(p->media_proto);
  cfg->server_ip = p->server_ip;
  cfg->server_port = p->sip_port;
  cfg->connection_timeout_ms = 5000;
  cfg->runtime_options.server_id = p->sip_id;
  cfg->runtime_options.server_domain = client->server_domain;
  cfg->runtime_options.ipc_id = p->username[0] ? p->username : p->device_id;
  cfg->runtime_options.ipc_password = p->password;
  cfg->runtime_options.ipc_ip = client->local_ip;
  cfg->runtime_options.ipc_sip_port = client->status.local_sip_port;
  cfg->runtime_options.device_id = p->device_id;
  cfg->runtime_options.transport = sdk_transport(p->transport);
  cfg->runtime_options.media_proto = media_proto;
  cfg->runtime_options.expires = p->register_interval > 0 ? p->register_interval : 3600;
  cfg->runtime_options.keepalive_interval_ms = (p->heartbeat_interval > 0 ? p->heartbeat_interval : 60) * 1000;
  cfg->runtime_options.sip_trace_enabled = true;
  cfg->device_info.server_id = p->sip_id;
  cfg->device_info.server_ip = p->server_ip;
  cfg->device_info.server_port = p->sip_port;
  cfg->device_info.sip_transport = sdk_transport(p->transport);
  cfg->device_info.ipc_id = p->device_id;
  cfg->device_info.ipc_pwd = p->password;
  cfg->device_info.ipc_ip = client->local_ip;
  cfg->device_info.ipc_sip_port = client->status.local_sip_port;
  cfg->device_info.ipc_media_port = client->status.local_sip_port + 2;
  cfg->device_info.device_name = p->name[0] ? p->name : "GB28181 Agent";
  cfg->device_info.device_manufacturer = "X2Rtn";
  cfg->device_info.device_model = "GB28181-Agent";
  cfg->device_info.device_firmware = "1.0";
  cfg->device_info.device_encode = "ON";
  cfg->device_info.device_record = "OFF";
  cfg->device_info.device_chan_id = p->device_id;
  cfg->device_info.pushing_proto_type = cfg->runtime_options.media_proto == C_GB28181_API_MEDIA_PROTO_RTC
                                           ? C_GB28181_API_PUSHING_PROTO_X2RTC
                                           : C_GB28181_API_PUSHING_PROTO_STANDARD_GB;
  int channel_count = client->channel_count;
  int valid_count = 0;
  if (channel_count > GB_AGENT_MAX_CHANNELS_PER_CLIENT) channel_count = GB_AGENT_MAX_CHANNELS_PER_CLIENT;
  for (int i = 0; i < channel_count; i++) {
    const gb_agent_channel_t *src = &client->channels[i];
    if (src->channel_id[0] == '\0') continue;
    copy_text(client->channel_ids[valid_count], sizeof(client->channel_ids[valid_count]), src->channel_id);
    snprintf(client->channel_names[valid_count],
             sizeof(client->channel_names[valid_count]),
             "%s",
             src->name[0] ? src->name : "");
    if (client->channel_names[valid_count][0] == '\0') {
      snprintf(client->channel_names[valid_count], sizeof(client->channel_names[valid_count]), "CH%d", src->ordinal > 0 ? src->ordinal : valid_count + 1);
    }
    client->sdk_channels[valid_count].channel_id = client->channel_ids[valid_count];
    client->sdk_channels[valid_count].name = client->channel_names[valid_count];
    client->sdk_channels[valid_count].media_proto = sdk_media_proto(src->media_proto);
    client->sdk_channels[valid_count].video_config.width = src->width > 0 ? (uint32_t) src->width : 1280;
    client->sdk_channels[valid_count].video_config.height = src->height > 0 ? (uint32_t) src->height : 720;
    client->sdk_channels[valid_count].video_config.fps = src->fps > 0 ? (uint32_t) src->fps : 25;
    client->sdk_channels[valid_count].video_config.bitrate_kbps = src->bitrate_kbps > 0 ? (uint32_t) src->bitrate_kbps : 2048;
    valid_count++;
  }
  cfg->device_info.ipc_channel_count = valid_count;
  cfg->channels = client->sdk_channels;
  cfg->channel_count = valid_count;
  memset(&client->callbacks, 0, sizeof(client->callbacks));
}

static void *stop_job_thread(void *arg) {
  gb_agent_stop_job_t *job = (gb_agent_stop_job_t *) arg;
  int idx;
  if (job == NULL) return NULL;
  if (job->api != NULL) {
    gb_mutex_lock(&job->agent->media_send_mu);
    (void) c_gb28181_api_stop(job->api);
    c_gb28181_api_destroy(&job->api);
    gb_mutex_unlock(&job->agent->media_send_mu);
  }
  gb_mutex_lock(&job->agent->mu);
  idx = client_index(job->platform_id);
  if (idx >= 0 && job->agent->clients[idx].cleanup_ctx == job->cb_ctx) {
    job->agent->clients[idx].cleanup_pending = 0;
    job->agent->clients[idx].cleanup_ctx = NULL;
    if (job->agent->clients[idx].status.desired_enabled) {
      copy_text(job->agent->clients[idx].status.sdk_state,
                sizeof(job->agent->clients[idx].status.sdk_state),
                "PENDING_START");
      set_reason(&job->agent->clients[idx].status, "previous client cleaned up");
    }
  }
  job->agent->cleanup_jobs--;
  gb_cond_broadcast(&job->agent->cv);
  gb_mutex_unlock(&job->agent->mu);
  free(job);
  return NULL;
}

static void cleanup_api_async(gb_agent_t *agent,
                              int platform_id,
                              c_gb28181_api_t *api,
                              gb_agent_cb_ctx_t *cb_ctx) {
  gb_agent_stop_job_t *job;
  gb_thread_t tid;
  if (api == NULL) {
    return;
  }
  job = (gb_agent_stop_job_t *) calloc(1, sizeof(*job));
  if (job == NULL) {
    gb_mutex_lock(&agent->media_send_mu);
    (void) c_gb28181_api_stop(api);
    c_gb28181_api_destroy(&api);
    gb_mutex_unlock(&agent->media_send_mu);
    return;
  }
  job->agent = agent;
  job->api = api;
  job->cb_ctx = cb_ctx;
  job->platform_id = platform_id;
  gb_mutex_lock(&agent->mu);
  agent->cleanup_jobs++;
  gb_mutex_unlock(&agent->mu);
  if (gb_thread_create(&tid, stop_job_thread, job) == 0) {
    gb_thread_detach(tid);
  } else {
    int idx = client_index(platform_id);
    gb_mutex_lock(&agent->mu);
    agent->cleanup_jobs--;
    if (idx >= 0 && agent->clients[idx].cleanup_ctx == cb_ctx) {
      agent->clients[idx].cleanup_pending = 0;
      agent->clients[idx].cleanup_ctx = NULL;
    }
    gb_cond_broadcast(&agent->cv);
    gb_mutex_unlock(&agent->mu);
    gb_mutex_lock(&agent->media_send_mu);
    (void) c_gb28181_api_stop(api);
    c_gb28181_api_destroy(&api);
    gb_mutex_unlock(&agent->media_send_mu);
    free(job);
  }
}
#endif

int gb_agent_create(const gb_agent_callbacks_t *callbacks, gb_agent_t **agent_out) {
  gb_agent_t *agent;
  int i;
  if (agent_out == NULL) return -1;
  *agent_out = NULL;
  agent = (gb_agent_t *) calloc(1, sizeof(*agent));
  if (agent == NULL) return -1;
  gb_mutex_init(&agent->mu);
  gb_mutex_init(&agent->media_send_mu);
#if GB_AGENT_USE_X2_GBSDK
  gb_cond_init(&agent->cv);
#endif
  if (callbacks != NULL) agent->callbacks = *callbacks;
  (void) gbmc_register_builtin_backends();
  (void) gbmcd_register_builtin_backends();
  for (i = 0; i < GB_AGENT_MAX_CHANNELS; i++) {
    agent->media_api_idx[i] = -1;
    copy_text(agent->media_sources[i].source_mode, sizeof(agent->media_sources[i].source_mode), "none");
    copy_text(agent->media_sources[i].file_pacing, sizeof(agent->media_sources[i].file_pacing), "realtime");
    agent->media_sources[i].fps = 25;
  }
  for (i = 0; i < GB_AGENT_MAX_CLIENTS; i++) {
    agent->clients[i].cfg.id = i + 1;
    agent->clients[i].status.id = i + 1;
    copy_text(agent->clients[i].status.sdk_state,
              sizeof(agent->clients[i].status.sdk_state),
              "STOPPED");
  }
  *agent_out = agent;
  return 0;
}

static void normalize_media_source(gb_agent_media_source_t *source) {
  if (source == NULL) return;
  if (source->source_mode[0] == '\0') copy_text(source->source_mode, sizeof(source->source_mode), "device");
  if (strcmp(source->source_mode, "none") == 0) {
    source->video_device[0] = '\0';
    source->audio_device[0] = '\0';
    source->media_file[0] = '\0';
  } else if (strcmp(source->source_mode, "file") == 0) {
    source->video_device[0] = '\0';
    source->audio_device[0] = '\0';
  } else if (strcmp(source->source_mode, "screen") == 0) {
    source->audio_device[0] = '\0';
    source->media_file[0] = '\0';
  } else {
    source->media_file[0] = '\0';
  }
  if (source->file_pacing[0] == '\0') copy_text(source->file_pacing, sizeof(source->file_pacing), "realtime");
  if (source->fps <= 0) source->fps = 25;
  if (source->iframe_interval <= 0) source->iframe_interval = 3;
  if (source->gop <= 0) source->gop = source->fps * source->iframe_interval;
}

int gb_agent_set_platform_media_source(gb_agent_t *agent, int platform_id, const gb_agent_media_source_t *source) {
  gb_agent_media_source_t normalized;
  int idx = media_index(platform_id);
  if (agent == NULL || source == NULL || idx < 0) return -1;
  normalized = *source;
  normalize_media_source(&normalized);
  gb_mutex_lock(&agent->mu);
  agent->media_sources[idx] = normalized;
  gb_mutex_unlock(&agent->mu);
  return 0;
}

int gb_agent_stop_platform_media_source(gb_agent_t *agent, int platform_id) {
  gb_thread_t old_thread;
  int join_old = 0;
  int idx = media_index(platform_id);
  int client_idx = (platform_id - 1) / GB_AGENT_MAX_CHANNELS_PER_CLIENT;
  if (agent == NULL || idx < 0) return -1;
  gb_mutex_lock(&agent->mu);
  if (agent->media_thread_running[idx]) {
    old_thread = agent->media_threads[idx];
    agent->media_thread_running[idx] = 0;
    join_old = 1;
  }
  agent->media_generations[idx]++;
  gb_mutex_unlock(&agent->mu);
  if (join_old) gb_thread_join(old_thread);
  gb_mutex_lock(&agent->mu);
  if (client_idx >= 0 && client_idx < GB_AGENT_MAX_CLIENTS) {
    int still_running = client_any_media_thread_running_locked(agent, client_idx);
    agent->clients[client_idx].status.media_running = still_running;
    agent->clients[client_idx].status.media_generation = agent->media_generations[idx];
    agent->clients[client_idx].status.updated_at = time(NULL);
    if (!still_running) set_reason(&agent->clients[client_idx].status, "media source idle");
  }
  gb_mutex_unlock(&agent->mu);
  return 0;
}

int gb_agent_get_platform_media_source(gb_agent_t *agent, int platform_id, gb_agent_media_source_t *source_out) {
  int idx = media_index(platform_id);
  if (agent == NULL || source_out == NULL || idx < 0) return -1;
  gb_mutex_lock(&agent->mu);
  *source_out = agent->media_sources[idx];
  gb_mutex_unlock(&agent->mu);
  return 0;
}

int gb_agent_set_media_source(gb_agent_t *agent, const gb_agent_media_source_t *source) {
  return gb_agent_set_platform_media_source(agent, 1, source);
}

int gb_agent_stop_media_source(gb_agent_t *agent) {
  int rc = 0;
  for (int i = 1; i <= GB_AGENT_MAX_CHANNELS; i++) {
    if (gb_agent_stop_platform_media_source(agent, i) != 0) rc = -1;
  }
  return rc;
}

static int gb_agent_stop_client_media_sources(gb_agent_t *agent, int client_idx) {
  int rc = 0;
  int first = client_idx * GB_AGENT_MAX_CHANNELS_PER_CLIENT + 1;
  int last = first + GB_AGENT_MAX_CHANNELS_PER_CLIENT;
  if (agent == NULL || client_idx < 0 || client_idx >= GB_AGENT_MAX_CLIENTS) return -1;
  if (last > GB_AGENT_MAX_CHANNELS + 1) last = GB_AGENT_MAX_CHANNELS + 1;
  for (int media_id = first; media_id < last; media_id++) {
    if (gb_agent_stop_platform_media_source(agent, media_id) != 0) rc = -1;
  }
  return rc;
}

int gb_agent_get_media_source(gb_agent_t *agent, gb_agent_media_source_t *source_out) {
  return gb_agent_get_platform_media_source(agent, 1, source_out);
}

void gb_agent_destroy(gb_agent_t **agent) {
  if (agent == NULL || *agent == NULL) return;
  gb_agent_stop_all(*agent);
  (void) gb_agent_stop_media_source(*agent);
#if GB_AGENT_USE_X2_GBSDK
  gb_mutex_lock(&(*agent)->mu);
  while ((*agent)->cleanup_jobs > 0) gb_cond_wait(&(*agent)->cv, &(*agent)->mu);
  gb_mutex_unlock(&(*agent)->mu);
  gb_cond_destroy(&(*agent)->cv);
#endif
  gb_mutex_destroy(&(*agent)->media_send_mu);
  gb_mutex_destroy(&(*agent)->mu);
  free(*agent);
  *agent = NULL;
}

int gb_agent_stop_platform(gb_agent_t *agent, int platform_id) {
  int idx = client_index(platform_id);
  int should_log = 0;
#if GB_AGENT_USE_X2_GBSDK
  c_gb28181_api_t *api = NULL;
  gb_agent_cb_ctx_t *cb_ctx = NULL;
#endif
  if (agent == NULL || idx < 0) return -1;
#if GB_AGENT_USE_X2_GBSDK
#endif
  gb_mutex_lock(&agent->mu);
  should_log = agent->clients[idx].cfg.enabled ||
               agent->clients[idx].status.desired_enabled ||
               agent->clients[idx].status.configured ||
               agent->clients[idx].status.registered ||
               agent->clients[idx].status.push_active ||
               client_any_media_thread_running_locked(agent, idx);
#if GB_AGENT_USE_X2_GBSDK
  should_log = should_log || agent->clients[idx].api != NULL ||
               agent->clients[idx].cleanup_pending;
#endif
  agent->clients[idx].cfg.enabled = 0;
  agent->clients[idx].status.desired_enabled = 0;
  agent->clients[idx].status.registered = 0;
  agent->clients[idx].status.keepalive_ok = 0;
  agent->clients[idx].status.push_active = 0;
  clear_client_channel_runtime_locked(agent, idx);
#if GB_AGENT_USE_X2_GBSDK
  agent->clients[idx].reconnect_at = 0;
  agent->clients[idx].reconnect_attempts = 0;
#endif
  copy_text(agent->clients[idx].status.sdk_state,
            sizeof(agent->clients[idx].status.sdk_state),
            "STOPPED");
  set_reason(&agent->clients[idx].status, "stopped");
#if GB_AGENT_USE_X2_GBSDK
  api = agent->clients[idx].api;
  agent->clients[idx].api = NULL;
  cb_ctx = api != NULL ? &agent->clients[idx].cb_ctx : NULL;
  if (api != NULL) {
    agent->clients[idx].cleanup_pending = 1;
    agent->clients[idx].cleanup_ctx = cb_ctx;
  }
#endif
  gb_mutex_unlock(&agent->mu);

  (void) gb_agent_stop_client_media_sources(agent, idx);
#if GB_AGENT_USE_X2_GBSDK
  cleanup_api_async(agent, platform_id, api, cb_ctx);
#endif
  if (should_log) emit_log(agent, platform_id, "INFO", "SIP", "platform client stopped");
  return 0;
}

void gb_agent_stop_all(gb_agent_t *agent) {
  if (agent == NULL) return;
  for (int i = 1; i <= GB_AGENT_MAX_PLATFORMS; i++) {
    (void) gb_agent_stop_platform(agent, i);
  }
}

int gb_agent_poll_reconnect(gb_agent_t *agent) {
#if GB_AGENT_USE_X2_GBSDK
  gb_agent_platform_t due[GB_AGENT_MAX_PLATFORMS];
  unsigned attempts[GB_AGENT_MAX_PLATFORMS];
  int count = 0;
  time_t now;
  if (agent == NULL) return -1;
  now = time(NULL);
  gb_mutex_lock(&agent->mu);
  for (int i = 0; i < GB_AGENT_MAX_PLATFORMS; i++) {
    gb_agent_client_t *client = &agent->clients[i];
    if (!client->status.desired_enabled ||
        client->cleanup_pending ||
        client->reconnect_at == 0 ||
        client->reconnect_at > now) {
      continue;
    }
    due[count] = client->cfg;
    attempts[count] = client->reconnect_attempts;
    client->reconnect_at = 0;
    count++;
  }
  gb_mutex_unlock(&agent->mu);
  for (int i = 0; i < count; i++) {
    char msg[256];
    snprintf(msg,
             sizeof(msg),
             "platform %d reconnecting attempt=%u",
             due[i].id,
             attempts[i]);
    emit_log(agent, due[i].id, "INFO", "SIP", msg);
    (void) gb_agent_apply_platform(agent, &due[i]);
  }
  return count;
#else
  (void) agent;
  return 0;
#endif
}

int gb_agent_apply_platform(gb_agent_t *agent, const gb_agent_platform_t *platform) {
  int idx;
  int ret = 0;
  char msg[256];
  char sdk_channel_msg[512] = {0};
  int have_sdk_channel_msg = 0;
#if GB_AGENT_USE_X2_GBSDK
  c_gb28181_api_t *api = NULL;
#endif
  if (agent == NULL || platform == NULL) return -1;
  idx = client_index(platform->id);
  if (idx < 0) return -1;

#if GB_AGENT_USE_X2_GBSDK
  gb_mutex_lock(&agent->mu);
  if (agent->clients[idx].cleanup_pending) {
    agent->clients[idx].cfg = *platform;
    agent->clients[idx].status.desired_enabled = platform->enabled;
    agent->clients[idx].status.registered = 0;
    agent->clients[idx].status.keepalive_ok = 0;
    agent->clients[idx].status.push_active = 0;
    if (platform->enabled) {
      refresh_static_status(&agent->clients[idx]);
      copy_text(agent->clients[idx].status.sdk_state,
                sizeof(agent->clients[idx].status.sdk_state),
                "STOPPING");
      set_reason(&agent->clients[idx].status, "waiting for previous client cleanup");
    } else {
      copy_text(agent->clients[idx].status.sdk_state,
                sizeof(agent->clients[idx].status.sdk_state),
                "STOPPED");
      set_reason(&agent->clients[idx].status, "stopped");
    }
    gb_mutex_unlock(&agent->mu);
    return platform->enabled ? 1 : 0;
  }
  gb_mutex_unlock(&agent->mu);
#endif

  if (!platform->enabled) return gb_agent_stop_platform(agent, platform->id);

  (void) gb_agent_stop_platform(agent, platform->id);

  gb_mutex_lock(&agent->mu);
  agent->clients[idx].cfg = *platform;
  agent->clients[idx].generation++;
  refresh_static_status(&agent->clients[idx]);
  agent->clients[idx].status.registered = 0;
  agent->clients[idx].status.keepalive_ok = 0;
  agent->clients[idx].status.push_active = 0;
  agent->clients[idx].status.register_code = 0;
  agent->clients[idx].status.last_error_code = 0;
  agent->clients[idx].status.started_at = time(NULL);
  copy_text(agent->clients[idx].status.sdk_state,
            sizeof(agent->clients[idx].status.sdk_state),
            "STARTING");
  set_reason(&agent->clients[idx].status, "starting");
#if GB_AGENT_USE_X2_GBSDK
  agent->clients[idx].cb_ctx.agent = agent;
  agent->clients[idx].cb_ctx.platform_id = platform->id;
  agent->clients[idx].cb_ctx.generation = agent->clients[idx].generation;
  fill_sdk_config(&agent->clients[idx]);
  if (agent->clients[idx].sdk_cfg.channel_count <= 0 || agent->clients[idx].sdk_cfg.channels == NULL) {
    ret = -1001;
    agent->clients[idx].status.last_error_code = ret;
    copy_text(agent->clients[idx].status.sdk_state,
              sizeof(agent->clients[idx].status.sdk_state),
              "ERROR");
    set_reason(&agent->clients[idx].status, "no configured GB28181 channels");
  }
  agent->clients[idx].callbacks.user_data = &agent->clients[idx].cb_ctx;
  agent->clients[idx].callbacks.on_state = on_state;
  agent->clients[idx].callbacks.on_registered = on_registered;
  agent->clients[idx].callbacks.on_keepalive = on_keepalive;
  agent->clients[idx].callbacks.on_message = on_message;
  agent->clients[idx].callbacks.on_error = on_error;
  agent->clients[idx].callbacks.on_channel_push_started = on_channel_push_started;
  agent->clients[idx].callbacks.on_channel_push_stopped = on_channel_push_stopped;
  agent->clients[idx].callbacks.on_channel_talkback_invite = on_channel_talkback_invite;
  agent->clients[idx].callbacks.on_channel_talkback_canceled = on_channel_talkback_canceled;
  agent->clients[idx].callbacks.on_channel_talkback_started = on_channel_talkback_started;
  agent->clients[idx].callbacks.on_channel_talkback_stopped = on_channel_talkback_stopped;
  agent->clients[idx].callbacks.on_channel_broadcast_started = on_channel_broadcast_started;
  agent->clients[idx].callbacks.on_channel_broadcast_stopped = on_channel_broadcast_stopped;
  agent->clients[idx].callbacks.on_channel_video_bitrate_update = on_channel_video_bitrate_update;
  agent->clients[idx].callbacks.on_channel_frame = on_channel_frame;
  agent->clients[idx].callbacks.on_sip_message = on_sip_message;
  agent->clients[idx].callbacks.on_device_control = on_device_control;
  agent->clients[idx].callbacks.on_record_info = on_record_info;
  if (ret == 0) {
    char channel_msg[512];
    size_t used = (size_t) snprintf(channel_msg,
                                    sizeof(channel_msg),
                                    "platform %d sdk catalog channels=%d ids=",
                                    platform->id,
                                    agent->clients[idx].sdk_cfg.channel_count);
    for (int i = 0; i < agent->clients[idx].sdk_cfg.channel_count && used < sizeof(channel_msg); i++) {
      int n = snprintf(channel_msg + used,
                       sizeof(channel_msg) - used,
                       "%s%s",
                       i == 0 ? "" : ",",
                       agent->clients[idx].channel_ids[i]);
      if (n < 0) break;
      used += (size_t) n;
    }
    copy_text(sdk_channel_msg, sizeof(sdk_channel_msg), channel_msg);
    have_sdk_channel_msg = 1;
  }
#endif
  gb_mutex_unlock(&agent->mu);

  if (have_sdk_channel_msg) emit_log(agent, platform->id, "INFO", "SIP", sdk_channel_msg);

#if GB_AGENT_USE_X2_GBSDK
  if (ret == 0) {
    ret = c_gb28181_api_create(&agent->clients[idx].sdk_cfg, &agent->clients[idx].callbacks, &api);
    if (ret == 0 && api != NULL) ret = c_gb28181_api_start(api);
  }
  gb_mutex_lock(&agent->mu);
  if (ret == 0 && api != NULL) {
    agent->clients[idx].api = api;
    copy_text(agent->clients[idx].status.sdk_state,
              sizeof(agent->clients[idx].status.sdk_state),
              "REGISTERING");
    set_reason(&agent->clients[idx].status, "client started");
  } else {
    agent->clients[idx].status.last_error_code = ret;
    agent->clients[idx].reconnect_attempts++;
    agent->clients[idx].reconnect_at = time(NULL) + 3;
    copy_text(agent->clients[idx].status.sdk_state,
              sizeof(agent->clients[idx].status.sdk_state),
              "ERROR");
    set_reason(&agent->clients[idx].status, "create/start failed");
  }
  gb_mutex_unlock(&agent->mu);
  if (ret != 0 && api != NULL) {
    gb_mutex_lock(&agent->media_send_mu);
    c_gb28181_api_destroy(&api);
    gb_mutex_unlock(&agent->media_send_mu);
  }
#else
  gb_mutex_lock(&agent->mu);
  agent->clients[idx].status.registered = 0;
  agent->clients[idx].status.keepalive_ok = 0;
  agent->clients[idx].status.push_active = 0;
  copy_text(agent->clients[idx].status.sdk_state,
            sizeof(agent->clients[idx].status.sdk_state),
            "SDK_DISABLED");
  set_reason(&agent->clients[idx].status, "GB28181 SDK disabled in this build");
  gb_mutex_unlock(&agent->mu);
#endif

  snprintf(msg, sizeof(msg), "platform %d client %s ret=%d server=%s:%d transport=%s",
           platform->id,
           ret == 0 ? "started" : "failed",
           ret,
           platform->server_ip,
           platform->sip_port,
           platform->transport);
  emit_log(agent, platform->id, ret == 0 ? "INFO" : "ERROR", "SIP", msg);
  return ret == 0 ? 0 : -1;
}

int gb_agent_set_platform_channels(gb_agent_t *agent,
                                   int platform_id,
                                   const gb_agent_channel_t *channels,
                                   int channel_count) {
  int idx = client_index(platform_id);
  int count = channel_count;
  if (agent == NULL || idx < 0 || channels == NULL || channel_count < 0) return -1;
  if (count > GB_AGENT_MAX_CHANNELS_PER_CLIENT) count = GB_AGENT_MAX_CHANNELS_PER_CLIENT;
  gb_mutex_lock(&agent->mu);
  if (agent->clients[idx].status.registered ||
      agent->clients[idx].status.desired_enabled
#if GB_AGENT_USE_X2_GBSDK
      || agent->clients[idx].api != NULL
#endif
      ) {
    gb_mutex_unlock(&agent->mu);
    return 1;
  }
  memset(agent->clients[idx].channels, 0, sizeof(agent->clients[idx].channels));
  for (int i = 0; i < count; i++) {
    agent->clients[idx].channels[i] = channels[i];
  }
  agent->clients[idx].channel_count = count;
  gb_mutex_unlock(&agent->mu);
  return 0;
}

int gb_agent_get_status(gb_agent_t *agent, int platform_id, gb_agent_status_t *status_out) {
  int idx = client_index(platform_id);
  if (agent == NULL || status_out == NULL || idx < 0) return -1;
  gb_mutex_lock(&agent->mu);
  *status_out = agent->clients[idx].status;
  gb_mutex_unlock(&agent->mu);
  return 0;
}

int gb_agent_get_all_status(gb_agent_t *agent,
                            gb_agent_status_t *statuses,
                            int max_statuses) {
  int count;
  if (agent == NULL || statuses == NULL || max_statuses <= 0) return -1;
  count = max_statuses < GB_AGENT_MAX_PLATFORMS ? max_statuses : GB_AGENT_MAX_PLATFORMS;
  gb_mutex_lock(&agent->mu);
  for (int i = 0; i < count; i++) statuses[i] = agent->clients[i].status;
  gb_mutex_unlock(&agent->mu);
  return count;
}

int gb_agent_get_channel_status(gb_agent_t *agent,
                                int channel_id,
                                gb_agent_channel_status_t *status_out) {
  int idx = media_index(channel_id);
  if (agent == NULL || status_out == NULL || idx < 0) return -1;
  memset(status_out, 0, sizeof(*status_out));
  gb_mutex_lock(&agent->mu);
  status_out->id = channel_id;
  status_out->push_active = agent->channel_push_active[idx];
  status_out->talkback_active = agent->channel_talkback_active[idx];
  status_out->broadcast_active = agent->channel_broadcast_active[idx];
  status_out->media_running = agent->media_thread_running[idx];
  status_out->media_generation = agent->media_generations[idx];
  copy_text(status_out->channel_id,
            sizeof(status_out->channel_id),
            agent->media_channel_ids[idx]);
  {
    int client_idx = media_status_index(agent, idx);
    if (client_idx >= 0 && client_idx < GB_AGENT_MAX_CLIENTS) {
      status_out->media_frames_encoded = agent->clients[client_idx].status.media_frames_encoded;
      status_out->media_frames_sent = agent->clients[client_idx].status.media_frames_sent;
      status_out->media_encode_errors = agent->clients[client_idx].status.media_encode_errors;
    }
  }
  gb_mutex_unlock(&agent->mu);
  return 0;
}
