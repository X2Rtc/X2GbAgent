#include "mongoose.h"
#include "app_channels.h"
#include "app_config.h"
#include "app_log.h"
#include "app_media.h"
#include "app_preview.h"
#include "app_session.h"
#include "gb_agent.h"
#include "gb_media_capture.h"
#include "gb_media_codec.h"
#include "gb_media_screen_source.h"
#include "gb_platform.h"
#include "preview_jpeg.h"
#include "source_manager.h"

#include <errno.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JSON_HDR "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n"
#define MAX_LOG_LINES 200
#ifndef GB_AGENT_VERSION
#define GB_AGENT_VERSION "0.1.0-dev"
#endif
#ifndef GB_AGENT_EMBEDDED
#define GB_AGENT_EMBEDDED 0
#endif

typedef struct {
  gb_mutex_t mu;
  sqlite3 *db;
  time_t started_at;
  bool running;
  int channel_count;
  char console_username[64];
  char console_password[128];
  unsigned long platform_generation;
  unsigned long applied_platform_generation;
  unsigned long media_generation;
  unsigned long applied_media_generation;
  int sip_registered[MAX_PLATFORMS];
  int streaming;
  unsigned long sip_ticks;
  unsigned long heartbeat_ticks;
  platform_cfg_t platforms[MAX_PLATFORMS];
  gb_channel_cfg_t gb_channels[MAX_CHANNELS];
  device_source_cfg_t channel_sources[MAX_CHANNELS];
  char channel_source_profile[MAX_CHANNELS][16];
  video_cfg_t channel_videos[MAX_CHANNELS];
  audio_cfg_t channel_audios[MAX_CHANNELS];
  video_cfg_t video;
  audio_cfg_t audio;
  device_source_cfg_t device;
  gb_agent_t *gb_agent;
  gb_agent_status_t gb_status[MAX_PLATFORMS];
  session_t sessions[MAX_SESSIONS];
  gb_log_file_t *file_log;
} app_t;

static app_t g_app;
static preview_cache_t g_preview;
static const char *s_listen = "http://0.0.0.0:8000";
static const char *s_web_root = "web_root";
static const char *s_db_path = "gb28181-agent.db";
static gb_log_file_config_t s_log_config;

static size_t appendf(char *buf, size_t cap, size_t len, const char *fmt, ...) {
  if (len >= cap) return len;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0) return len;
  size_t add = (size_t) n;
  return len + (add < cap - len ? add : cap - len - 1);
}

static size_t append_json_string(char *buf, size_t cap, size_t len, const char *s) {
  if (len < cap) buf[len++] = '"';
  for (; s && *s && len + 6 < cap; s++) {
    unsigned char ch = (unsigned char) *s;
    if (ch == '"' || ch == '\\') {
      buf[len++] = '\\';
      buf[len++] = (char) ch;
    } else if (ch == '\n') {
      buf[len++] = '\\';
      buf[len++] = 'n';
    } else if (ch == '\r') {
      buf[len++] = '\\';
      buf[len++] = 'r';
    } else if (ch == '\t') {
      buf[len++] = '\\';
      buf[len++] = 't';
    } else if (ch < 0x20) {
      len = appendf(buf, cap, len, "\\u%04x", ch);
    } else {
      buf[len++] = (char) ch;
    }
  }
  if (len < cap) buf[len++] = '"';
  if (len < cap) buf[len] = '\0';
  return len;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

static void executable_dir(char *out, size_t out_size, const char *argv0) {
  const char *slash1;
  const char *slash2;
  const char *slash;
  size_t n;
  if (!out || out_size == 0) return;
  copy_text(out, out_size, ".");
  if (!argv0 || !argv0[0]) return;
  slash1 = strrchr(argv0, '/');
  slash2 = strrchr(argv0, '\\');
  slash = slash1 > slash2 ? slash1 : slash2;
  if (!slash) return;
  n = (size_t) (slash - argv0);
  if (n == 0 || n >= out_size) return;
  memcpy(out, argv0, n);
  out[n] = '\0';
}

static unsigned long long parse_ull_arg(const char *value, unsigned long long fallback) {
  char *end = NULL;
  unsigned long long parsed;
  if (!value || !value[0]) return fallback;
  parsed = strtoull(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0) return fallback;
  return parsed;
}

static int parse_int_arg(const char *value, int fallback) {
  char *end = NULL;
  long parsed;
  if (!value || !value[0]) return fallback;
  parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed <= 0 || parsed > 1000) return fallback;
  return (int) parsed;
}

static void init_log_config(int argc, char **argv) {
  char install_dir[512];
  const char *env_dir;
  const char *env_max;
  const char *env_count;
  executable_dir(install_dir, sizeof(install_dir), argc > 0 ? argv[0] : NULL);
  gb_log_file_default_config(&s_log_config, install_dir);
  env_dir = getenv("GB_AGENT_LOG_DIR");
  env_max = getenv("GB_AGENT_LOG_MAX_BYTES");
  env_count = getenv("GB_AGENT_LOG_ROTATE_COUNT");
  if (env_dir && env_dir[0]) copy_text(s_log_config.directory, sizeof(s_log_config.directory), env_dir);
  if (env_max && env_max[0]) s_log_config.max_file_bytes = parse_ull_arg(env_max, s_log_config.max_file_bytes);
  if (env_count && env_count[0]) s_log_config.rotate_count = parse_int_arg(env_count, s_log_config.rotate_count);
  if (argc > 4 && argv[4] && argv[4][0]) copy_text(s_log_config.directory, sizeof(s_log_config.directory), argv[4]);
  if (argc > 5) s_log_config.max_file_bytes = parse_ull_arg(argv[5], s_log_config.max_file_bytes);
  if (argc > 6) s_log_config.rotate_count = parse_int_arg(argv[6], s_log_config.rotate_count);
}

static size_t append_platform_json(char *buf, size_t cap, size_t len, const platform_cfg_t *p) {
  len = appendf(buf, cap, len, "{\"id\":%d,\"enabled\":%d,\"name\":", p->id, p->enabled);
  len = append_json_string(buf, cap, len, p->name);
  len = appendf(buf, cap, len, ",\"server_ip\":");
  len = append_json_string(buf, cap, len, p->server_ip);
  len = appendf(buf, cap, len, ",\"sip_port\":%d,\"sip_id\":", p->sip_port);
  len = append_json_string(buf, cap, len, p->sip_id);
  len = appendf(buf, cap, len, ",\"device_id\":");
  len = append_json_string(buf, cap, len, p->device_id);
  len = appendf(buf, cap, len, ",\"username\":");
  len = append_json_string(buf, cap, len, p->username);
  len = appendf(buf, cap, len, ",\"transport\":");
  len = append_json_string(buf, cap, len, p->transport);
  len = appendf(buf, cap, len, ",\"media_proto\":");
  len = append_json_string(buf, cap, len, p->media_proto);
  len = appendf(buf, cap, len, ",\"register_interval\":%d,\"heartbeat_interval\":%d}",
                p->register_interval, p->heartbeat_interval);
  return len;
}

static size_t append_capture_devices_json(char *buf,
                                          size_t cap,
                                          size_t len,
                                          gbmc_media_type_t media_type) {
  gbmc_device_info_t devices[16];
  size_t count = 0;
  int rc = gbmc_list_devices(NULL, media_type, devices, 16, &count);
  len = appendf(buf, cap, len, "[");
  if (rc == GBMC_OK) {
    size_t n = count < 16 ? count : 16;
    size_t emitted = 0;
    for (size_t i = 0; i < n; i++) {
      if (strcmp(devices[i].backend, "null") == 0) continue;
      len = appendf(buf, cap, len, "%s{\"backend\":", emitted++ ? "," : "");
      len = append_json_string(buf, cap, len, devices[i].backend);
      len = appendf(buf, cap, len, ",\"id\":");
      len = append_json_string(buf, cap, len, devices[i].id);
      len = appendf(buf, cap, len, ",\"name\":");
      len = append_json_string(buf, cap, len, devices[i].name);
      len = appendf(buf, cap, len, "}");
    }
  }
  return appendf(buf, cap, len, "]");
}

static size_t append_source_types_json(char *buf,
                                       size_t cap,
                                       size_t len,
                                       int screen_supported) {
  len = appendf(buf, cap, len, "[\"device\",\"file\"");
  if (screen_supported) len = appendf(buf, cap, len, ",\"screen\"");
  return appendf(buf, cap, len, "]");
}

static size_t append_screen_sources_json(char *buf, size_t cap, size_t len) {
  gbms_screen_source_info_t screens[8];
  size_t count = 0;
#if GB_AGENT_EMBEDDED
  (void) screens;
#else
  if (gbms_list_screens(screens, sizeof(screens) / sizeof(screens[0]), &count) != 0) {
    count = 0;
  }
#endif
  len = appendf(buf, cap, len, "[");
  for (size_t i = 0; i < count && i < sizeof(screens) / sizeof(screens[0]); i++) {
    len = appendf(buf, cap, len, "%s{\"id\":", i ? "," : "");
    len = append_json_string(buf, cap, len, screens[i].id);
    len = appendf(buf, cap, len, ",\"name\":");
    len = append_json_string(buf, cap, len, screens[i].name);
    len = appendf(buf, cap, len, ",\"width\":%d,\"height\":%d,\"primary\":%d}",
                  screens[i].width, screens[i].height, screens[i].primary);
  }
  return appendf(buf, cap, len, "]");
}

static int find_screen_source(const char *id, gbms_screen_source_info_t *out) {
  gbms_screen_source_info_t screens[8];
  size_t count = 0;
#if GB_AGENT_EMBEDDED
  (void) id;
  (void) out;
  (void) screens;
  return 0;
#else
  if (id == NULL || id[0] == '\0') return 0;
  if (gbms_list_screens(screens, sizeof(screens) / sizeof(screens[0]), &count) != 0) return 0;
  for (size_t i = 0; i < count && i < sizeof(screens) / sizeof(screens[0]); i++) {
    if (strcmp(screens[i].id, id) == 0) {
      if (out != NULL) *out = screens[i];
      return 1;
    }
  }
  return 0;
#endif
}

static int first_screen_source(gbms_screen_source_info_t *out) {
  gbms_screen_source_info_t screens[8];
  size_t count = 0;
#if GB_AGENT_EMBEDDED
  (void) out;
  (void) screens;
  return 0;
#else
  if (gbms_list_screens(screens, sizeof(screens) / sizeof(screens[0]), &count) != 0 || count == 0) {
    return 0;
  }
  if (out != NULL) *out = screens[0];
  return 1;
#endif
}

static const char *project_codec_label(const char *name, gbmcd_media_type_t media_type) {
  if (!name) return NULL;
  if (media_type == GBMCD_MEDIA_VIDEO) {
    if (strcmp(name, "h264") == 0 || strcmp(name, "libx264") == 0 ||
        strcmp(name, "h264_nvenc") == 0 || strcmp(name, "h264_qsv") == 0 ||
        strcmp(name, "h264_amf") == 0 || strcmp(name, "h264_mf") == 0) {
      return "H264";
    }
    if (strcmp(name, "hevc") == 0 || strcmp(name, "libx265") == 0 ||
        strcmp(name, "hevc_nvenc") == 0 || strcmp(name, "hevc_qsv") == 0 ||
        strcmp(name, "hevc_amf") == 0 || strcmp(name, "hevc_mf") == 0) {
      return "H265";
    }
    return NULL;
  }
  if (strcmp(name, "pcm_alaw") == 0) return "G711A";
  if (strcmp(name, "pcm_mulaw") == 0) return "G711U";
  if (strcmp(name, "aac") == 0) return "AAC";
  if (strcmp(name, "opus") == 0 || strcmp(name, "libopus") == 0) return "OPUS";
  return NULL;
}

static int project_codec_rank(const char *name) {
  if (!name) return 1000;
  if (strcmp(name, "libx264") == 0 || strcmp(name, "libx265") == 0 ||
      strcmp(name, "aac") == 0 || strcmp(name, "libopus") == 0 ||
      strcmp(name, "pcm_alaw") == 0 || strcmp(name, "pcm_mulaw") == 0) {
    return 0;
  }
  if (strcmp(name, "h264") == 0 || strcmp(name, "hevc") == 0 ||
      strcmp(name, "opus") == 0) {
    return 10;
  }
  if (strstr(name, "_mf") != NULL || strstr(name, "_qsv") != NULL ||
      strstr(name, "_nvenc") != NULL || strstr(name, "_amf") != NULL) {
    return 20;
  }
  return 100;
}

static int project_codec_order(const char *label, gbmcd_media_type_t media_type) {
  if (media_type == GBMCD_MEDIA_VIDEO) {
    if (strcmp(label, "H264") == 0) return 0;
    if (strcmp(label, "H265") == 0) return 1;
  } else {
    if (strcmp(label, "G711A") == 0) return 0;
    if (strcmp(label, "G711U") == 0) return 1;
    if (strcmp(label, "AAC") == 0) return 2;
    if (strcmp(label, "OPUS") == 0) return 3;
  }
  return 99;
}

static size_t append_codecs_json(char *buf,
                                 size_t cap,
                                 size_t len,
                                 gbmcd_media_type_t media_type,
                                 gbmcd_codec_role_t role) {
  gbmcd_codec_info_t codecs[512];
  size_t count = 0;
  int rc = gbmcd_list_codecs(NULL,
                             media_type,
                             role,
                             codecs,
                             sizeof(codecs) / sizeof(codecs[0]),
                             &count);
  len = appendf(buf, cap, len, "[");
  if (rc == GBMCD_OK) {
    int emitted = 0;
    const char *labels[8];
    size_t selected[8];
    int ranks[8];
    size_t selected_count = 0;
    size_t n = count < sizeof(codecs) / sizeof(codecs[0])
                 ? count
                 : sizeof(codecs) / sizeof(codecs[0]);
    for (size_t i = 0; i < n; i++) {
      const char *label = project_codec_label(codecs[i].name, media_type);
      int rank;
      if (!label) continue;
      rank = project_codec_rank(codecs[i].name);
      size_t slot = selected_count;
      for (size_t j = 0; j < selected_count; j++) {
        if (strcmp(labels[j], label) == 0) {
          slot = j;
          break;
        }
      }
      if (slot == selected_count) {
        labels[slot] = label;
        selected[slot] = i;
        ranks[slot] = rank;
        selected_count++;
      } else if (rank < ranks[slot]) {
        selected[slot] = i;
        ranks[slot] = rank;
      }
    }
    for (size_t j = 0; j < selected_count; j++) {
      for (size_t k = j + 1; k < selected_count; k++) {
        if (project_codec_order(labels[k], media_type) <
            project_codec_order(labels[j], media_type)) {
          const char *tmp_label = labels[j];
          size_t tmp_selected = selected[j];
          int tmp_rank = ranks[j];
          labels[j] = labels[k];
          selected[j] = selected[k];
          ranks[j] = ranks[k];
          labels[k] = tmp_label;
          selected[k] = tmp_selected;
          ranks[k] = tmp_rank;
        }
      }
    }
    for (size_t j = 0; j < selected_count; j++) {
      size_t i = selected[j];
      const char *label = labels[j];
      len = appendf(buf, cap, len, "%s{\"backend\":", emitted ? "," : "");
      len = append_json_string(buf, cap, len, codecs[i].backend);
      len = appendf(buf, cap, len, ",\"name\":");
      len = append_json_string(buf, cap, len, label);
      len = appendf(buf, cap, len, ",\"ffmpeg_name\":");
      len = append_json_string(buf, cap, len, codecs[i].name);
      len = appendf(buf, cap, len, ",\"description\":");
      len = append_json_string(buf, cap, len, codecs[i].description);
      len = appendf(buf, cap, len, ",\"hardware\":%s}",
                    codecs[i].hardware ? "true" : "false");
      emitted++;
    }
  }
  return appendf(buf, cap, len, "]");
}

static void log_add(const char *level, const char *category, const char *msg) {
  gb_log_ctx_t ctx = {.mu = &g_app.mu, .db = g_app.db, .file = g_app.file_log};
  gb_log_add(&ctx, level, category, msg);
}

static void log_add_tagged(const char *level,
                           const char *channel,
                           const char *module,
                           const char *submodule,
                           const char *msg) {
  gb_log_ctx_t ctx = {.mu = &g_app.mu, .db = g_app.db, .file = g_app.file_log};
  gb_log_add_tagged(&ctx, level, channel, module, submodule, msg);
}

static void gb_log_callback(void *user_data,
                            int platform_id,
                            const char *level,
                            const char *category,
                            const char *message) {
  char channel[64] = {0};
  int exists = 0;
  int channel_id = platform_id < 0 ? -platform_id : 0;
  int client_id = platform_id > 0 && platform_id <= MAX_PLATFORMS ? platform_id : 0;
  (void) user_data;
  gb_mutex_lock(&g_app.mu);
  if (channel_id > 0) {
    exists = channel_id <= MAX_CHANNELS && gb_config_channel_exists(g_app.db, channel_id);
  } else if (client_id > 0) {
    exists = 1;
  } else {
    exists = platform_id <= 0;
  }
  if (!exists) {
    gb_mutex_unlock(&g_app.mu);
    return;
  }
  if (channel_id > 0 && channel_id <= MAX_CHANNELS && g_app.gb_channels[channel_id - 1].name[0]) {
    copy_text(channel, sizeof(channel), g_app.gb_channels[channel_id - 1].name);
  } else if (channel_id > 0) {
    snprintf(channel, sizeof(channel), "CH%d", channel_id);
  } else if (client_id > 0) {
    if (g_app.platforms[client_id - 1].name[0]) {
      copy_text(channel, sizeof(channel), g_app.platforms[client_id - 1].name);
    } else {
      snprintf(channel, sizeof(channel), "GB Client %d", client_id);
    }
  } else {
    copy_text(channel, sizeof(channel), "SYSTEM");
  }
  gb_mutex_unlock(&g_app.mu);
  log_add_tagged(level != NULL ? level : "INFO",
                 channel,
                 category != NULL ? category : "SIP",
                 NULL,
                 message != NULL ? message : "");
}

static void to_gb_agent_platform(const platform_cfg_t *src, gb_agent_platform_t *dst) {
  memset(dst, 0, sizeof(*dst));
  dst->id = src->id;
  dst->enabled = src->enabled;
  snprintf(dst->name, sizeof(dst->name), "%s", src->name);
  snprintf(dst->server_ip, sizeof(dst->server_ip), "%s", src->server_ip);
  dst->sip_port = src->sip_port;
  snprintf(dst->sip_id, sizeof(dst->sip_id), "%s", src->sip_id);
  snprintf(dst->device_id, sizeof(dst->device_id), "%s", src->device_id);
  snprintf(dst->username, sizeof(dst->username), "%s", src->username);
  snprintf(dst->password, sizeof(dst->password), "%s", src->password);
  snprintf(dst->transport, sizeof(dst->transport), "%s", src->transport);
  snprintf(dst->media_proto, sizeof(dst->media_proto), "%s", src->media_proto);
  dst->register_interval = src->register_interval;
  dst->heartbeat_interval = src->heartbeat_interval;
}

static void to_gb_agent_media_source(const device_source_cfg_t *src,
                                     const video_cfg_t *video,
                                     gb_agent_media_source_t *dst) {
  memset(dst, 0, sizeof(*dst));
  snprintf(dst->source_mode, sizeof(dst->source_mode), "%s", src->source_mode);
  if (strcmp(src->source_mode, "none") == 0) {
  } else if (strcmp(src->source_mode, "file") == 0) {
    snprintf(dst->media_file, sizeof(dst->media_file), "%s", src->media_file);
  } else if (strcmp(src->source_mode, "screen") == 0) {
    snprintf(dst->video_device, sizeof(dst->video_device), "%s", src->video_device);
  } else {
    snprintf(dst->video_device, sizeof(dst->video_device), "%s", src->video_device);
    snprintf(dst->audio_device, sizeof(dst->audio_device), "%s", src->audio_device);
  }
  snprintf(dst->file_pacing, sizeof(dst->file_pacing), "%s", src->file_pacing);
  snprintf(dst->resolution, sizeof(dst->resolution), "%s", video->resolution);
  dst->bitrate_kbps = video->bitrate_kbps;
  dst->fps = video->fps > 0 ? video->fps : 25;
  dst->iframe_interval = video->iframe_interval > 0 ? video->iframe_interval : 3;
  dst->gop = video->gop > 0 ? video->gop : dst->fps * dst->iframe_interval;
  dst->loop = src->file_loop;
}

static void to_gb_agent_channel(const gb_channel_cfg_t *src,
                                const video_cfg_t *video,
                                gb_agent_channel_t *dst) {
  int width = 1280;
  int height = 720;
  memset(dst, 0, sizeof(*dst));
  dst->id = src->id;
  dst->client_id = src->client_id;
  dst->ordinal = src->ordinal;
  snprintf(dst->channel_id, sizeof(dst->channel_id), "%s", src->channel_id);
  snprintf(dst->name, sizeof(dst->name), "%s", src->name);
  snprintf(dst->media_proto, sizeof(dst->media_proto), "%s", src->media_proto[0] ? src->media_proto : "RTC");
  if (video != NULL && sscanf(video->resolution, "%dx%d", &width, &height) != 2) {
    width = 1280;
    height = 720;
  }
  dst->width = width;
  dst->height = height;
  dst->fps = video != NULL && video->fps > 0 ? video->fps : 25;
  dst->bitrate_kbps = video != NULL && video->bitrate_kbps > 0 ? video->bitrate_kbps : 2048;
}

static int any_platform_enabled(const platform_cfg_t *platforms, int count) {
  if (platforms == NULL) return 0;
  for (int i = 0; i < count; i++) {
    if (platforms[i].enabled) return 1;
  }
  return 0;
}

static int device_source_equal(const device_source_cfg_t *a, const device_source_cfg_t *b) {
  if (a == NULL || b == NULL) return 0;
  return strcmp(a->source_mode, b->source_mode) == 0 &&
         strcmp(a->video_device, b->video_device) == 0 &&
         strcmp(a->audio_device, b->audio_device) == 0 &&
         strcmp(a->media_file, b->media_file) == 0 &&
         strcmp(a->resolution, b->resolution) == 0 &&
         a->bitrate_kbps == b->bitrate_kbps &&
         a->file_loop == b->file_loop &&
         strcmp(a->file_pacing, b->file_pacing) == 0;
}

static size_t append_gb_status_json(char *buf, size_t cap, size_t len,
                                    const gb_agent_status_t *s) {
  len = appendf(buf, cap, len,
                "{\"id\":%d,\"enabled\":%d,\"configured\":%d,\"registered\":%d,"
                "\"keepalive_ok\":%d,\"keepalive_ok_count\":%d,\"push_active\":%d,"
                "\"media_running\":%d,\"media_generation\":%lu,"
                "\"media_frames_encoded\":%lu,\"media_frames_sent\":%lu,"
                "\"media_encode_errors\":%lu,"
                "\"register_code\":%d,\"last_error_code\":%d,\"local_sip_port\":%d,"
                "\"server_ip\":",
                s->id, s->desired_enabled, s->configured, s->registered,
                s->keepalive_ok, s->keepalive_ok_count, s->push_active,
                s->media_running, s->media_generation,
                s->media_frames_encoded, s->media_frames_sent,
                s->media_encode_errors,
                s->register_code, s->last_error_code, s->local_sip_port);
  len = append_json_string(buf, cap, len, s->server_ip);
  len = appendf(buf, cap, len, ",\"server_port\":%d,\"transport\":", s->server_port);
  len = append_json_string(buf, cap, len, s->transport);
  len = appendf(buf, cap, len, ",\"sdk_state\":");
  len = append_json_string(buf, cap, len, s->sdk_state);
  len = appendf(buf, cap, len, ",\"last_reason\":");
  len = append_json_string(buf, cap, len, s->last_reason);
  len = appendf(buf, cap, len, ",\"started_at\":%lld,\"updated_at\":%lld}",
                (long long) s->started_at, (long long) s->updated_at);
  return len;
}

static size_t append_alarm_json(char *buf, size_t cap, size_t len,
                                int *count,
                                const char *level,
                                const char *category,
                                int channel_id,
                                const char *code,
                                const char *title,
                                const char *message) {
  len = appendf(buf, cap, len, "%s{\"level\":", (*count)++ ? "," : "");
  len = append_json_string(buf, cap, len, level);
  len = appendf(buf, cap, len, ",\"category\":");
  len = append_json_string(buf, cap, len, category);
  len = appendf(buf, cap, len, ",\"channel_id\":%d,\"code\":", channel_id);
  len = append_json_string(buf, cap, len, code);
  len = appendf(buf, cap, len, ",\"title\":");
  len = append_json_string(buf, cap, len, title);
  len = appendf(buf, cap, len, ",\"message\":");
  len = append_json_string(buf, cap, len, message);
  return appendf(buf, cap, len, "}");
}

static void refresh_gb_runtime_status(void) {
  gb_agent_status_t live_status[MAX_PLATFORMS];
  if (g_app.gb_agent == NULL) return;
  if (gb_agent_get_all_status(g_app.gb_agent, live_status, MAX_PLATFORMS) != MAX_PLATFORMS) return;
  gb_mutex_lock(&g_app.mu);
  g_app.streaming = 0;
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    g_app.gb_status[i] = live_status[i];
    g_app.sip_registered[i] = live_status[i].registered;
    if (live_status[i].push_active) g_app.streaming = 1;
  }
  gb_mutex_unlock(&g_app.mu);
}

static size_t append_media_source_json(char *buf,
                                       size_t cap,
                                       size_t len,
                                       int id,
                                       const gb_agent_media_source_t *source) {
  len = appendf(buf, cap, len, "{\"id\":%d,\"source_mode\":", id);
  len = append_json_string(buf, cap, len, source ? source->source_mode : "");
  len = appendf(buf, cap, len, ",\"video_device\":");
  len = append_json_string(buf, cap, len, source ? source->video_device : "");
  len = appendf(buf, cap, len, ",\"audio_device\":");
  len = append_json_string(buf, cap, len, source ? source->audio_device : "");
  len = appendf(buf, cap, len, ",\"media_file\":");
  len = append_json_string(buf, cap, len, source ? source->media_file : "");
  len = appendf(buf, cap, len, ",\"resolution\":");
  len = append_json_string(buf, cap, len, source ? source->resolution : "");
  len = appendf(buf, cap, len,
                ",\"bitrate_kbps\":%d,\"fps\":%d,\"gop\":%d,\"iframe_interval\":%d,\"loop\":%d,\"file_pacing\":",
                source ? source->bitrate_kbps : 0,
                source ? source->fps : 0,
                source ? source->gop : 0,
                source ? source->iframe_interval : 0,
                source ? source->loop : 0);
  len = append_json_string(buf, cap, len, source ? source->file_pacing : "");
  return appendf(buf, cap, len, "}");
}

static size_t append_device_source_json(char *buf,
                                        size_t cap,
                                        size_t len,
                                        const device_source_cfg_t *source,
                                        const char *profile) {
  len = appendf(buf, cap, len, "\"source_profile\":");
  len = append_json_string(buf, cap, len, profile && profile[0] ? profile : "none");
  len = appendf(buf, cap, len, ",\"source_mode\":");
  len = append_json_string(buf, cap, len, source ? source->source_mode : "");
  len = appendf(buf, cap, len, ",\"video_device\":");
  len = append_json_string(buf, cap, len, source ? source->video_device : "");
  len = appendf(buf, cap, len, ",\"audio_device\":");
  len = append_json_string(buf, cap, len, source ? source->audio_device : "");
  len = appendf(buf, cap, len, ",\"media_file\":");
  len = append_json_string(buf, cap, len, source ? source->media_file : "");
  len = appendf(buf, cap, len, ",\"resolution\":");
  len = append_json_string(buf, cap, len, source ? source->resolution : "");
  len = appendf(buf, cap, len, ",\"bitrate_kbps\":%d,\"file_loop\":%d,\"file_pacing\":",
                source ? source->bitrate_kbps : 0,
                source ? source->file_loop : 0);
  return append_json_string(buf, cap, len, source ? source->file_pacing : "");
}

static size_t append_av_channel_json(char *buf,
                                     size_t cap,
                                     size_t len,
                                     int id,
                                     const video_cfg_t *v,
                                     const audio_cfg_t *a) {
  len = appendf(buf, cap, len, "{\"id\":%d,\"video\":{\"codec\":", id);
  len = append_json_string(buf, cap, len, v->codec);
  len = appendf(buf, cap, len, ",\"resolution\":");
  len = append_json_string(buf, cap, len, v->resolution);
  len = appendf(buf, cap, len, ",\"fps\":%d,\"rc_mode\":", v->fps);
  len = append_json_string(buf, cap, len, v->rc_mode);
  len = appendf(buf, cap, len,
                ",\"bitrate_kbps\":%d,\"gop\":%d,\"iframe_interval\":%d,"
                "\"low_latency\":%d,\"prefer_hardware\":%d},\"audio\":{\"enabled\":%d,\"codec\":",
                v->bitrate_kbps, v->gop, v->iframe_interval, v->low_latency, v->prefer_hardware,
                a->enabled);
  len = append_json_string(buf, cap, len, a->codec);
  return appendf(buf, cap, len, ",\"sample_rate\":%d,\"bitrate_kbps\":%d}}",
                 a->sample_rate, a->bitrate_kbps);
}

static void db_init(void) {
  if (gb_config_open(&g_app.db, s_db_path) != 0) {
    fprintf(stderr, "cannot open sqlite db: %s\n", s_db_path);
    exit(2);
  }
}

static void load_config(void) {
  gb_app_config_t config;
  gb_mutex_lock(&g_app.mu);
  if (gb_config_load(g_app.db, &config) == 0) {
    g_app.channel_count = config.channel_count;
    for (int i = 0; i < MAX_PLATFORMS; i++) {
      g_app.platforms[i] = config.platforms[i];
    }
    for (int i = 0; i < MAX_CHANNELS; i++) {
      g_app.gb_channels[i] = config.gb_channels[i];
      g_app.channel_sources[i] = config.channel_sources[i];
      snprintf(g_app.channel_source_profile[i], sizeof(g_app.channel_source_profile[i]), "%s",
               config.channel_source_profile[i]);
      g_app.channel_videos[i] = config.channel_videos[i];
      g_app.channel_audios[i] = config.channel_audios[i];
    }
    g_app.video = config.video;
    g_app.audio = config.audio;
    g_app.device = config.device;
  }
  g_app.platform_generation++;
  g_app.media_generation++;
  gb_mutex_unlock(&g_app.mu);
}

static void *agent_thread(void *arg) {
  (void) arg;
  while (g_app.running) {
    platform_cfg_t platforms[MAX_PLATFORMS];
    gb_channel_cfg_t gb_channels[MAX_CHANNELS];
    gb_agent_platform_t gb_platform;
    gb_agent_status_t statuses[MAX_PLATFORMS];
    device_source_cfg_t device;
    device_source_cfg_t channel_sources[MAX_CHANNELS];
    char source_profiles[MAX_CHANNELS][16];
    video_cfg_t video;
    video_cfg_t channel_videos[MAX_CHANNELS];
    unsigned long platform_generation;
    unsigned long applied_platform_generation;
    unsigned long media_generation;
    unsigned long applied_media_generation;

    gb_mutex_lock(&g_app.mu);
    platform_generation = g_app.platform_generation;
    applied_platform_generation = g_app.applied_platform_generation;
    media_generation = g_app.media_generation;
    applied_media_generation = g_app.applied_media_generation;
    for (int i = 0; i < MAX_PLATFORMS; i++) platforms[i] = g_app.platforms[i];
    for (int i = 0; i < MAX_CHANNELS; i++) {
      gb_channels[i] = g_app.gb_channels[i];
      channel_sources[i] = g_app.channel_sources[i];
      snprintf(source_profiles[i], sizeof(source_profiles[i]), "%s", g_app.channel_source_profile[i]);
      channel_videos[i] = g_app.channel_videos[i];
    }
    device = g_app.device;
    video = g_app.video;
    gb_mutex_unlock(&g_app.mu);

    if (platform_generation != applied_platform_generation) {
      int apply_pending = 0;
      for (int i = 0; i < MAX_PLATFORMS; i++) {
        int ret;
        for (int j = 0; j < MAX_CHANNELS; j++) {
          gb_agent_media_source_t media_source;
          if (gb_channels[j].id <= 0 || gb_channels[j].client_id != platforms[i].id) continue;
          if (platforms[i].enabled) {
            device_source_cfg_t effective = effective_channel_source(&device, &channel_sources[j], source_profiles[j]);
            to_gb_agent_media_source(&effective, &channel_videos[j], &media_source);
            (void) gb_agent_set_platform_media_source(g_app.gb_agent, gb_channels[j].id, &media_source);
          } else {
            (void) gb_agent_stop_platform_media_source(g_app.gb_agent, gb_channels[j].id);
          }
        }
        to_gb_agent_platform(&platforms[i], &gb_platform);
        {
          gb_agent_channel_t agent_channels[MAX_CHANNELS_PER_CLIENT];
          int agent_channel_count = 0;
          for (int j = 0; j < MAX_CHANNELS; j++) {
            if (gb_channels[j].id <= 0 || gb_channels[j].client_id != platforms[i].id) continue;
            to_gb_agent_channel(&gb_channels[j], &channel_videos[j], &agent_channels[agent_channel_count]);
            agent_channel_count++;
            if (agent_channel_count >= MAX_CHANNELS_PER_CLIENT) break;
          }
          if (agent_channel_count > 0) {
            (void) gb_agent_set_platform_channels(g_app.gb_agent,
                                                  platforms[i].id,
                                                  agent_channels,
                                                  agent_channel_count);
          }
        }
        ret = gb_agent_apply_platform(g_app.gb_agent, &gb_platform);
        if (ret > 0) apply_pending = 1;
      }
      if (!apply_pending) {
        gb_mutex_lock(&g_app.mu);
        g_app.applied_platform_generation = platform_generation;
        g_app.applied_media_generation = media_generation;
        gb_mutex_unlock(&g_app.mu);
        applied_media_generation = media_generation;
      }
    }

    if (media_generation != applied_media_generation) {
      for (int i = 0; i < MAX_CHANNELS; i++) {
        gb_agent_media_source_t media_source;
        int client_idx = gb_channels[i].client_id - 1;
        if (gb_channels[i].id <= 0 || client_idx < 0 || client_idx >= MAX_PLATFORMS) continue;
        if (platforms[client_idx].enabled) {
          device_source_cfg_t effective = effective_channel_source(&device, &channel_sources[i], source_profiles[i]);
          to_gb_agent_media_source(&effective, &channel_videos[i], &media_source);
          (void) gb_agent_set_platform_media_source(g_app.gb_agent, gb_channels[i].id, &media_source);
        } else {
          (void) gb_agent_stop_platform_media_source(g_app.gb_agent, gb_channels[i].id);
        }
      }
      gb_mutex_lock(&g_app.mu);
      g_app.applied_media_generation = media_generation;
      gb_mutex_unlock(&g_app.mu);
    }

    (void) gb_agent_poll_reconnect(g_app.gb_agent);

    memset(statuses, 0, sizeof(statuses));
    (void) gb_agent_get_all_status(g_app.gb_agent, statuses, MAX_PLATFORMS);
    gb_mutex_lock(&g_app.mu);
    g_app.streaming = 0;
    for (int i = 0; i < MAX_PLATFORMS; i++) {
      g_app.gb_status[i] = statuses[i];
      g_app.sip_registered[i] = statuses[i].registered;
      if (statuses[i].push_active) g_app.streaming = 1;
    }
    g_app.sip_ticks++;
    g_app.heartbeat_ticks++;
    gb_mutex_unlock(&g_app.mu);
    gb_sleep_ms(1000);
  }
  return NULL;
}

static double cpu_load_percent(void) {
  gb_system_info_t info;
  if (gb_get_system_info(&info) != 0) return 0;
  return info.cpu_load_percent;
}

static void reply_status(struct mg_connection *c) {
  char body[32768];
  size_t len = 0;
  gb_system_info_t sys;
  gb_agent_status_t live_status[MAX_PLATFORMS];
  gb_agent_channel_status_t channel_status[MAX_CHANNELS];
  gb_agent_media_source_t applied_sources[MAX_CHANNELS];
  gb_channel_cfg_t channel_snapshot[MAX_CHANNELS];
  int channel_exists[MAX_CHANNELS] = {0};
  int client_exists[MAX_PLATFORMS] = {0};
  int channel_count = 0;
  int first_channel_idx = 0;
  memset(applied_sources, 0, sizeof(applied_sources));
  memset(channel_status, 0, sizeof(channel_status));
  memset(&sys, 0, sizeof(sys));
  (void) gb_get_system_info(&sys);
  memset(live_status, 0, sizeof(live_status));
  if (g_app.gb_agent != NULL &&
      gb_agent_get_all_status(g_app.gb_agent, live_status, MAX_PLATFORMS) == MAX_PLATFORMS) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
      (void) gb_agent_get_platform_media_source(g_app.gb_agent, i + 1, &applied_sources[i]);
      (void) gb_agent_get_channel_status(g_app.gb_agent, i + 1, &channel_status[i]);
    }
    gb_mutex_lock(&g_app.mu);
    g_app.streaming = 0;
    for (int i = 0; i < MAX_PLATFORMS; i++) {
      g_app.gb_status[i] = live_status[i];
      g_app.sip_registered[i] = live_status[i].registered;
      if (live_status[i].push_active) g_app.streaming = 1;
    }
    gb_mutex_unlock(&g_app.mu);
  }
  gb_mutex_lock(&g_app.mu);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    channel_exists[i] = gb_config_channel_exists(g_app.db, i + 1);
    if (channel_exists[i]) {
      channel_snapshot[i] = g_app.gb_channels[i];
      if (channel_snapshot[i].client_id >= 1 && channel_snapshot[i].client_id <= MAX_PLATFORMS) {
        client_exists[channel_snapshot[i].client_id - 1] = 1;
      }
      if (channel_count == 0) first_channel_idx = i;
      channel_count++;
    }
  }
  long uptime = (long) (time(NULL) - g_app.started_at);
  int streaming = g_app.streaming;
  int reg_snapshot[MAX_PLATFORMS];
  gb_agent_status_t status_snapshot[MAX_PLATFORMS];
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    reg_snapshot[i] = g_app.sip_registered[i];
    status_snapshot[i] = g_app.gb_status[i];
  }
  gb_mutex_unlock(&g_app.mu);
  unsigned long long total = (unsigned long long) sys.total_memory;
  unsigned long long freeb = (unsigned long long) sys.free_memory;
  unsigned long long used = total > freeb ? total - freeb : 0;
  len = appendf(body, sizeof(body), len,
                "{\"uptime_sec\":%ld,\"cpu_load_percent\":%g,"
                "\"version\":\"" GB_AGENT_VERSION "\","
                "\"mem_total\":%llu,\"mem_used\":%llu,\"device_status\":\"running\","
                "\"channel_count\":%d,\"max_channels\":%d,"
                "\"preview_fps\":%d,\"preview_interval_ms\":%d,"
                "\"sip_registered\":[",
                uptime, sys.cpu_load_percent, total, used, channel_count, MAX_CHANNELS,
                GB_PREVIEW_FPS_CLAMPED, GB_PREVIEW_INTERVAL_MS);
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (!client_exists[i]) continue;
    len = appendf(body, sizeof(body), len, "%s%d", len && body[len - 1] == '[' ? "" : ",", reg_snapshot[i]);
  }
  len = appendf(body, sizeof(body), len, "],\"streaming\":%s,\"gb_clients\":[",
                streaming ? "true" : "false");
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (!client_exists[i]) continue;
    len = appendf(body, sizeof(body), len, "%s", len && body[len - 1] == '[' ? "" : ",");
    len = append_gb_status_json(body, sizeof(body), len, &status_snapshot[i]);
  }
  len = appendf(body, sizeof(body), len, "],\"applied_source\":");
  len = append_media_source_json(body, sizeof(body), len, first_channel_idx + 1, &applied_sources[first_channel_idx]);
  len = appendf(body, sizeof(body), len, ",\"channel_statuses\":[");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!channel_exists[i]) continue;
    len = appendf(body,
                  sizeof(body),
                  len,
                  "%s{\"id\":%d,\"push_active\":%d,\"talkback_active\":%d,"
                  "\"broadcast_active\":%d,\"media_running\":%d,\"media_generation\":%lu}",
                  len && body[len - 1] == '[' ? "" : ",",
                  i + 1,
                  channel_status[i].push_active,
                  channel_status[i].talkback_active,
                  channel_status[i].broadcast_active,
                  channel_status[i].media_running,
                  channel_status[i].media_generation);
  }
  len = appendf(body, sizeof(body), len, "]");
  len = appendf(body, sizeof(body), len, ",\"applied_sources\":[");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!channel_exists[i]) continue;
    len = appendf(body, sizeof(body), len, "%s", len && body[len - 1] == '[' ? "" : ",");
    len = append_media_source_json(body, sizeof(body), len, i + 1, &applied_sources[i]);
  }
  len = appendf(body, sizeof(body), len, "],\"source_refs\":");
  len = gb_source_append_status_json(body, sizeof(body), len);
  len = appendf(body, sizeof(body), len, "}\n");
  body[sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}

static void reply_alarms(struct mg_connection *c) {
  char body[16384];
  size_t len = 0;
  int count = 0;
  gb_system_info_t sys;
  platform_cfg_t platforms[MAX_PLATFORMS];
  gb_agent_status_t statuses[MAX_PLATFORMS];
  int exists[MAX_PLATFORMS] = {0};
  int alarm_channel_ids[MAX_PLATFORMS] = {0};
  char title[128];
  char message[256];

  memset(&sys, 0, sizeof(sys));
  memset(platforms, 0, sizeof(platforms));
  memset(statuses, 0, sizeof(statuses));
  (void) gb_get_system_info(&sys);
  refresh_gb_runtime_status();

  gb_mutex_lock(&g_app.mu);
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    platforms[i] = g_app.platforms[i];
    statuses[i] = g_app.gb_status[i];
  }
  for (int i = 0; i < MAX_CHANNELS; i++) {
    int client_idx = g_app.gb_channels[i].client_id - 1;
    if (g_app.gb_channels[i].id <= 0 || client_idx < 0 || client_idx >= MAX_PLATFORMS) continue;
    exists[client_idx] = 1;
    if (alarm_channel_ids[client_idx] == 0 || g_app.gb_channels[i].ordinal == 1) {
      alarm_channel_ids[client_idx] = g_app.gb_channels[i].id;
    }
  }
  gb_mutex_unlock(&g_app.mu);

  len = appendf(body, sizeof(body), len, "{\"generated_at\":%lld,\"alarms\":[", (long long) time(NULL));
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    platform_cfg_t *p = &platforms[i];
    gb_agent_status_t *s = &statuses[i];
    int alarm_channel_id = alarm_channel_ids[i] > 0 ? alarm_channel_ids[i] : i + 1;
    if (!exists[i] || !p->enabled) continue;
    if (s->id == 0) {
      snprintf(title, sizeof(title), "%s runtime missing", p->name[0] ? p->name : "Channel");
      snprintf(message, sizeof(message), "gb_clients status missing for channel %d", alarm_channel_id);
      len = append_alarm_json(body, sizeof(body), len, &count, "warn", "SIP", alarm_channel_id,
                              "runtime_missing", title, message);
    } else if (!s->configured) {
      snprintf(title, sizeof(title), "%s not configured", p->name[0] ? p->name : "Channel");
      snprintf(message, sizeof(message), "%s", s->last_reason[0] ? s->last_reason : "GB client config is incomplete");
      len = append_alarm_json(body, sizeof(body), len, &count, "warn", "CONFIG", alarm_channel_id,
                              "not_configured", title, message);
    } else if (!s->registered) {
      snprintf(title, sizeof(title), "%s offline", p->name[0] ? p->name : "Channel");
      snprintf(message, sizeof(message), "%s", s->last_reason[0] ? s->last_reason : "GB28181 platform is not registered");
      len = append_alarm_json(body, sizeof(body), len, &count, "warn", "SIP", alarm_channel_id,
                              "register_failed", title, message);
    } else if (!s->keepalive_ok) {
      snprintf(title, sizeof(title), "%s keepalive warning", p->name[0] ? p->name : "Channel");
      snprintf(message, sizeof(message), "%s", s->last_reason[0] ? s->last_reason : "GB28181 keepalive is not healthy");
      len = append_alarm_json(body, sizeof(body), len, &count, "warn", "SIP", alarm_channel_id,
                              "keepalive_warn", title, message);
    }
  }

  if (sys.total_memory > 0) {
    double mem_pct = (double) (sys.total_memory - sys.free_memory) * 100.0 / (double) sys.total_memory;
    if (mem_pct > 80.0) {
      snprintf(message, sizeof(message), "%.1f%% used", mem_pct);
      len = append_alarm_json(body, sizeof(body), len, &count, "danger", "SYSTEM", 0,
                              "high_memory", "High memory usage", message);
    }
  }
  if (sys.cpu_load_percent > 85.0) {
    snprintf(message, sizeof(message), "%.1f%%", sys.cpu_load_percent);
    len = append_alarm_json(body, sizeof(body), len, &count, "danger", "SYSTEM", 0,
                            "high_cpu", "High CPU load", message);
  }

  len = appendf(body, sizeof(body), len, "]}\n");
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}

static void reply_av(struct mg_connection *c) {
  char body[16384];
  size_t len = 0;
  int exists[MAX_CHANNELS] = {0};
  gb_mutex_lock(&g_app.mu);
  video_cfg_t videos[MAX_CHANNELS];
  audio_cfg_t audios[MAX_CHANNELS];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    videos[i] = g_app.channel_videos[i];
    audios[i] = g_app.channel_audios[i];
    exists[i] = gb_config_channel_exists(g_app.db, i + 1);
  }
  gb_mutex_unlock(&g_app.mu);
  len = appendf(body, sizeof(body), len, "{\"channels\":[");
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!exists[i]) continue;
    len = appendf(body, sizeof(body), len, "%s", len && body[len - 1] == '[' ? "" : ",");
    len = append_av_channel_json(body, sizeof(body), len, i + 1, &videos[i], &audios[i]);
  }
  len = appendf(body, sizeof(body), len, "],\"capabilities\":{\"video_encoders\":");
  len = append_codecs_json(body, sizeof(body), len,
                           GBMCD_MEDIA_VIDEO, GBMCD_CODEC_ENCODER);
  len = appendf(body, sizeof(body), len, ",\"audio_encoders\":");
  len = append_codecs_json(body, sizeof(body), len,
                           GBMCD_MEDIA_AUDIO, GBMCD_CODEC_ENCODER);
  len = appendf(body, sizeof(body), len, "}}\n");
  body[sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}

static void save_av(struct mg_connection *c, struct mg_http_message *hm, int channel_id) {
  if (mg_json_get(hm->body, "$", NULL) < 0) {
    mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid json"));
    return;
  }
  gb_mutex_lock(&g_app.mu);
  if (channel_id < 1 || channel_id > MAX_CHANNELS || !gb_config_channel_exists(g_app.db, channel_id)) {
    gb_mutex_unlock(&g_app.mu);
    mg_http_reply(c, 404, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("channel not found"));
    return;
  }
  video_cfg_t v = g_app.channel_videos[channel_id - 1];
  audio_cfg_t a = g_app.channel_audios[channel_id - 1];
  gb_mutex_unlock(&g_app.mu);
  char *s = NULL;
  bool b = false;
  if ((s = mg_json_get_str(hm->body, "$.video.codec")) != NULL) { snprintf(v.codec, sizeof(v.codec), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.video.resolution")) != NULL) { snprintf(v.resolution, sizeof(v.resolution), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.video.rc_mode")) != NULL) { snprintf(v.rc_mode, sizeof(v.rc_mode), "%s", s); free(s); }
  v.fps = (int) mg_json_get_long(hm->body, "$.video.fps", v.fps);
  v.bitrate_kbps = (int) mg_json_get_long(hm->body, "$.video.bitrate_kbps", v.bitrate_kbps);
  v.gop = (int) mg_json_get_long(hm->body, "$.video.gop", v.gop);
  v.iframe_interval = (int) mg_json_get_long(hm->body, "$.video.iframe_interval", v.iframe_interval);
  if (mg_json_get_bool(hm->body, "$.video.low_latency", &b)) v.low_latency = b ? 1 : 0;
  if (mg_json_get_bool(hm->body, "$.video.prefer_hardware", &b)) v.prefer_hardware = b ? 1 : 0;
  if (mg_json_get_bool(hm->body, "$.audio.enabled", &b)) a.enabled = b ? 1 : 0;
  if ((s = mg_json_get_str(hm->body, "$.audio.codec")) != NULL) { snprintf(a.codec, sizeof(a.codec), "%s", s); free(s); }
  a.sample_rate = (int) mg_json_get_long(hm->body, "$.audio.sample_rate", a.sample_rate);
  a.bitrate_kbps = (int) mg_json_get_long(hm->body, "$.audio.bitrate_kbps", a.bitrate_kbps);

  gb_mutex_lock(&g_app.mu);
  if (gb_config_save_channel_av(g_app.db, channel_id, &v, &a) != 0) {
    gb_mutex_unlock(&g_app.mu);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("save av failed"));
    return;
  }
  g_app.channel_videos[channel_id - 1] = v;
  g_app.channel_audios[channel_id - 1] = a;
  if (channel_id == 1) {
    g_app.video = v;
    g_app.audio = a;
    (void) gb_config_save_av(g_app.db, &v, &a);
  }
  g_app.media_generation++;
  gb_mutex_unlock(&g_app.mu);
  log_add("INFO", "CONFIG", "audio/video config updated");
  mg_http_reply(c, 200, JSON_HDR, "true\n");
}

static void reply_devices(struct mg_connection *c) {
  char body[8192];
  size_t len = 0;
  device_source_cfg_t d;
  gbms_screen_source_info_t screen;
  int screen_supported = first_screen_source(&screen);
  gb_mutex_lock(&g_app.mu);
  d = g_app.device;
  gb_mutex_unlock(&g_app.mu);
  len = appendf(body, sizeof(body), len, "{\"source_mode\":");
  len = append_json_string(body, sizeof(body), len, d.source_mode);
  len = appendf(body, sizeof(body), len, ",\"video_device\":");
  len = append_json_string(body, sizeof(body), len, d.video_device);
  len = appendf(body, sizeof(body), len, ",\"audio_device\":");
  len = append_json_string(body, sizeof(body), len, d.audio_device);
  len = appendf(body, sizeof(body), len, ",\"media_file\":");
  len = append_json_string(body, sizeof(body), len, d.media_file);
  len = appendf(body, sizeof(body), len, ",\"resolution\":");
  len = append_json_string(body, sizeof(body), len, d.resolution);
  len = appendf(body, sizeof(body), len,
                ",\"bitrate_kbps\":%d,\"file_loop\":%d,\"file_pacing\":",
                d.bitrate_kbps, d.file_loop);
  len = append_json_string(body, sizeof(body), len, d.file_pacing);
  len = appendf(body, sizeof(body), len, ",\"capabilities\":{\"source_types\":");
  len = append_source_types_json(body, sizeof(body), len, screen_supported);
  len = appendf(body, sizeof(body), len, ",\"video_devices\":");
  len = append_capture_devices_json(body, sizeof(body), len, GBMC_MEDIA_VIDEO);
  len = appendf(body, sizeof(body), len, ",\"audio_devices\":");
  len = append_capture_devices_json(body, sizeof(body), len, GBMC_MEDIA_AUDIO);
  len = appendf(body, sizeof(body), len, ",\"screen_sources\":");
  len = append_screen_sources_json(body, sizeof(body), len);
  len = appendf(body, sizeof(body), len, "}");
  len = appendf(body, sizeof(body), len, "}\n");
  body[sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}

static void save_devices(struct mg_connection *c, struct mg_http_message *hm) {
  gb_mutex_lock(&g_app.mu);
  device_source_cfg_t d = g_app.device;
  gb_mutex_unlock(&g_app.mu);
  const media_file_info_t *info;
  char *s = NULL;
  bool b = false;
  if (mg_json_get(hm->body, "$", NULL) < 0) {
    mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid json"));
    return;
  }
  if ((s = mg_json_get_str(hm->body, "$.source_mode")) != NULL) {
    if (strcmp(s, "file") != 0 && strcmp(s, "device") != 0 && strcmp(s, "screen") != 0) {
      free(s);
      mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid source_mode"));
      return;
    }
    snprintf(d.source_mode, sizeof(d.source_mode), "%s", s);
    free(s);
  }
  if ((s = mg_json_get_str(hm->body, "$.video_device")) != NULL) { snprintf(d.video_device, sizeof(d.video_device), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.audio_device")) != NULL) { snprintf(d.audio_device, sizeof(d.audio_device), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.media_file")) != NULL) { snprintf(d.media_file, sizeof(d.media_file), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.resolution")) != NULL) { snprintf(d.resolution, sizeof(d.resolution), "%s", s); free(s); }
  d.bitrate_kbps = (int) mg_json_get_long(hm->body, "$.bitrate_kbps", d.bitrate_kbps);
  if ((s = mg_json_get_str(hm->body, "$.file_pacing")) != NULL) {
    snprintf(d.file_pacing, sizeof(d.file_pacing), "%s", strcmp(s, "fast") == 0 ? "fast" : "realtime");
    free(s);
  }
  if (mg_json_get_bool(hm->body, "$.file_loop", &b)) d.file_loop = b ? 1 : 0;

  info = gb_media_info_by_path(d.media_file);
  if (strcmp(d.source_mode, "file") == 0 && info == NULL) {
    mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("unknown media_file"));
    return;
  }
  if (info != NULL) {
    snprintf(d.resolution, sizeof(d.resolution), "%s", info->resolution);
    d.bitrate_kbps = info->bitrate_kbps;
  }
  if (strcmp(d.source_mode, "screen") == 0) {
    gbms_screen_source_info_t screen;
    if (d.video_device[0] == '\0' && first_screen_source(&screen)) {
      snprintf(d.video_device, sizeof(d.video_device), "%s", screen.id);
    }
    if (!find_screen_source(d.video_device, &screen)) {
      mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("unknown screen_source"));
      return;
    }
    d.audio_device[0] = '\0';
    d.media_file[0] = '\0';
    if (screen.width > 0 && screen.height > 0) {
      snprintf(d.resolution, sizeof(d.resolution), "%dx%d", screen.width, screen.height);
    }
    d.bitrate_kbps = 0;
  } else if (strcmp(d.source_mode, "file") == 0) {
    d.video_device[0] = '\0';
    d.audio_device[0] = '\0';
  } else {
    d.media_file[0] = '\0';
    d.resolution[0] = '\0';
    d.bitrate_kbps = 0;
  }

  gb_mutex_lock(&g_app.mu);
  if (!device_source_equal(&g_app.device, &d)) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
      int client_idx = g_app.gb_channels[i].client_id - 1;
      if (client_idx >= 0 &&
          client_idx < MAX_PLATFORMS &&
          g_app.platforms[client_idx].enabled &&
          g_app.gb_status[client_idx].push_active &&
          strcmp(g_app.channel_source_profile[i], "global") == 0) {
        gb_mutex_unlock(&g_app.mu);
        mg_http_reply(c, 409, JSON_HDR,
                      "{%m:%m,%m:%m}\n",
                      MG_ESC("error"),
                      MG_ESC("channel is pushing; bound source cannot be changed"),
                      MG_ESC("field"),
                      MG_ESC("source_mode"));
        return;
      }
    }
  }
  if (gb_config_save_device(g_app.db, &d) != 0) {
    gb_mutex_unlock(&g_app.mu);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("save device failed"));
    return;
  }
  g_app.device = d;
  g_app.media_generation++;
  gb_mutex_unlock(&g_app.mu);
  log_add("INFO", "CONFIG", "device source config updated");
  reply_devices(c);
}

static void reply_config_export(struct mg_connection *c) {
  char body[65536];
  size_t len = 0;
  gb_mutex_lock(&g_app.mu);
  platform_cfg_t platforms[MAX_PLATFORMS];
  gb_channel_cfg_t channels[MAX_CHANNELS];
  device_source_cfg_t sources[MAX_CHANNELS];
  char profiles[MAX_CHANNELS][16];
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    platforms[i] = g_app.platforms[i];
  }
  for (int i = 0; i < MAX_CHANNELS; i++) {
    channels[i] = g_app.gb_channels[i];
    sources[i] = g_app.channel_sources[i];
    snprintf(profiles[i], sizeof(profiles[i]), "%s", g_app.channel_source_profile[i]);
  }
  video_cfg_t v = g_app.video;
  audio_cfg_t a = g_app.audio;
  gb_mutex_unlock(&g_app.mu);
  len = appendf(body, sizeof(body), len, "{\"platforms\":[");
  int emitted = 0;
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (platforms[i].id <= 0) continue;
    len = appendf(body, sizeof(body), len, "%s", emitted++ ? "," : "");
    len = append_platform_json(body, sizeof(body), len, &platforms[i]);
  }
  len = appendf(body, sizeof(body), len, "],\"channels\":[");
  emitted = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (channels[i].id <= 0) continue;
    len = appendf(body, sizeof(body), len, "%s{\"id\":%d,\"client_id\":%d,\"ordinal\":%d,\"channel_id\":",
                  emitted++ ? "," : "",
                  channels[i].id,
                  channels[i].client_id,
                  channels[i].ordinal);
    len = append_json_string(body, sizeof(body), len, channels[i].channel_id);
    len = appendf(body, sizeof(body), len, ",\"name\":");
    len = append_json_string(body, sizeof(body), len, channels[i].name);
    len = appendf(body, sizeof(body), len, ",\"media_proto\":");
    len = append_json_string(body, sizeof(body), len, channels[i].media_proto);
    len = appendf(body, sizeof(body), len, ",");
    len = append_device_source_json(body, sizeof(body), len, &sources[i], profiles[i]);
    len = appendf(body, sizeof(body), len, "}");
  }
  len = appendf(body, sizeof(body), len, "],\"video\":{\"codec\":");
  len = append_json_string(body, sizeof(body), len, v.codec);
  len = appendf(body, sizeof(body), len, ",\"resolution\":");
  len = append_json_string(body, sizeof(body), len, v.resolution);
  len = appendf(body, sizeof(body), len,
                ",\"fps\":%d,\"rc_mode\":", v.fps);
  len = append_json_string(body, sizeof(body), len, v.rc_mode);
  len = appendf(body, sizeof(body), len,
                ",\"bitrate_kbps\":%d,\"gop\":%d,\"iframe_interval\":%d,"
                "\"low_latency\":%d,\"prefer_hardware\":%d},"
                "\"audio\":{\"enabled\":%d,\"codec\":",
                v.bitrate_kbps, v.gop, v.iframe_interval,
                v.low_latency, v.prefer_hardware, a.enabled);
  len = append_json_string(body, sizeof(body), len, a.codec);
  len = appendf(body, sizeof(body), len,
                ",\"sample_rate\":%d,\"bitrate_kbps\":%d}}\n",
                a.sample_rate, a.bitrate_kbps);
  body[sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Content-Disposition: attachment; filename=\"gb28181-agent-config.json\"\r\n",
                "%s", body);
}

static void system_action(struct mg_connection *c, const char *action) {
  char msg[96];
  snprintf(msg, sizeof(msg), "system action requested: %s", action);
  log_add("WARN", "SYSTEM", msg);
  mg_http_reply(c, 202, JSON_HDR, "{%m:%m,%m:%m}\n", MG_ESC("status"),
                MG_ESC("accepted"), MG_ESC("action"), MG_ESC(action));
}

static void reply_settings(struct mg_connection *c) {
  gb_mutex_lock(&g_app.mu);
  const char *user = g_app.console_username;
  gb_mutex_unlock(&g_app.mu);
  mg_http_reply(c, 200, JSON_HDR,
                "{%m:{%m:%m},%m:{%m:%m,%m:%d}}\n",
                MG_ESC("account"),
                MG_ESC("username"), MG_ESC(user),
                MG_ESC("update"),
                MG_ESC("mode"), MG_ESC(GB_AGENT_EMBEDDED ? "embedded_ota" : "desktop_check"),
                MG_ESC("embedded_ota"), GB_AGENT_EMBEDDED ? 1 : 0);
}

static void save_password(struct mg_connection *c, struct mg_http_message *hm) {
  char *current = mg_json_get_str(hm->body, "$.current_password");
  char *next = mg_json_get_str(hm->body, "$.new_password");
  int ok = 0;
  if (current == NULL || next == NULL || next[0] == '\0') {
    mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid password"));
    free(current);
    free(next);
    return;
  }
  gb_mutex_lock(&g_app.mu);
  ok = strcmp(current, g_app.console_password) == 0;
  gb_mutex_unlock(&g_app.mu);
  if (!ok) {
    mg_http_reply(c, 403, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("current password mismatch"));
    free(current);
    free(next);
    return;
  }
  console_auth_cfg_t auth;
  snprintf(auth.username, sizeof(auth.username), "%s", "admin");
  snprintf(auth.password, sizeof(auth.password), "%s", next);
  gb_mutex_lock(&g_app.mu);
  if (gb_config_save_auth(g_app.db, &auth) != 0) {
    gb_mutex_unlock(&g_app.mu);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("save password failed"));
    free(current);
    free(next);
    return;
  }
  snprintf(g_app.console_username, sizeof(g_app.console_username), "%s", "admin");
  snprintf(g_app.console_password, sizeof(g_app.console_password), "%s", auth.password);
  gb_mutex_unlock(&g_app.mu);
  log_add("INFO", "SYSTEM", "console password updated");
  mg_http_reply(c, 200, JSON_HDR, "true\n");
  free(current);
  free(next);
}

static void oem_config_path(char *out, size_t out_size) {
  if (out == NULL || out_size == 0) return;
  snprintf(out, out_size, "%s/%s", s_web_root, "oem.config.json");
}

static void reply_oem_config(struct mg_connection *c) {
  char path[512];
  FILE *fp;
  long size;
  char *data;
  oem_config_path(path, sizeof(path));
  fp = fopen(path, "rb");
  if (fp == NULL) {
    mg_http_reply(c, 404, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("oem config not found"));
    return;
  }
  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (size < 0 || size > 65536) {
    fclose(fp);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("oem config too large"));
    return;
  }
  data = (char *) malloc((size_t) size + 1);
  if (data == NULL) {
    fclose(fp);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("no memory"));
    return;
  }
  fread(data, 1, (size_t) size, fp);
  data[size] = '\0';
  fclose(fp);
  mg_http_reply(c, 200, JSON_HDR, "%s\n", data);
  free(data);
}

static void save_oem_config(struct mg_connection *c, struct mg_http_message *hm) {
  char path[512];
  FILE *fp;
  if (mg_json_get(hm->body, "$", NULL) < 0) {
    mg_http_reply(c, 400, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid json"));
    return;
  }
  if (hm->body.len > 65536) {
    mg_http_reply(c, 413, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("oem config too large"));
    return;
  }
  oem_config_path(path, sizeof(path));
  fp = fopen(path, "wb");
  if (fp == NULL) {
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("save oem config failed"));
    return;
  }
  fwrite(hm->body.buf, 1, hm->body.len, fp);
  fclose(fp);
  log_add("INFO", "SYSTEM", "oem config updated");
  mg_http_reply(c, 200, JSON_HDR, "true\n");
}

static void check_updates(struct mg_connection *c) {
#if GB_AGENT_EMBEDDED
  mg_http_reply(c, 200, JSON_HDR,
                "{%m:%m,%m:%m,%m:%d,%m:%m}\n",
                MG_ESC("mode"), MG_ESC("embedded_ota"),
                MG_ESC("current_version"), MG_ESC(GB_AGENT_VERSION),
                MG_ESC("update_available"), 0,
                MG_ESC("message"), MG_ESC("Embedded OTA is available on this device"));
#else
  mg_http_reply(c, 200, JSON_HDR,
                "{%m:%m,%m:%m,%m:%d,%m:%m}\n",
                MG_ESC("mode"), MG_ESC("desktop_check"),
                MG_ESC("current_version"), MG_ESC(GB_AGENT_VERSION),
                MG_ESC("update_available"), 0,
                MG_ESC("message"), MG_ESC("Desktop update check completed"));
#endif
}

static void ws_push(struct mg_mgr *mgr) {
  char data[512];
  gb_mutex_lock(&g_app.mu);
  long uptime = (long) (time(NULL) - g_app.started_at);
  int reg1 = g_app.sip_registered[0], reg2 = g_app.sip_registered[1];
  int streaming = g_app.streaming;
  gb_mutex_unlock(&g_app.mu);
  mg_snprintf(data, sizeof(data),
              "{%m:%ld,%m:%g,%m:%d,%m:%d,%m:%d}", MG_ESC("uptime_sec"),
              uptime, MG_ESC("cpu_load_percent"), cpu_load_percent(),
              MG_ESC("sip1"), reg1, MG_ESC("sip2"), reg2, MG_ESC("streaming"),
              streaming);
  for (struct mg_connection *wc = mgr->conns; wc != NULL; wc = wc->next) {
    if (wc->data[0] == 'W') mg_ws_send(wc, data, strlen(data), WEBSOCKET_OP_TEXT);
  }
}

static void timer_fn(void *arg) {
  ws_push((struct mg_mgr *) arg);
}

static int uri_id(struct mg_http_message *hm, const char *prefix) {
  if (hm->uri.len <= strlen(prefix)) return -1;
  const char *p = hm->uri.buf + strlen(prefix);
  return atoi(p);
}

static gb_channel_http_ctx_t channel_http_ctx(void) {
  gb_channel_http_ctx_t ctx = {
      .mu = &g_app.mu,
      .db = g_app.db,
      .gb_agent = g_app.gb_agent,
      .channel_count = &g_app.channel_count,
      .platforms = g_app.platforms,
      .gb_channels = g_app.gb_channels,
      .sources = g_app.channel_sources,
      .source_profiles = g_app.channel_source_profile,
      .gb_status = g_app.gb_status,
      .platform_generation = &g_app.platform_generation,
      .media_generation = &g_app.media_generation,
      .reload_config = load_config,
      .log_add = log_add,
  };
  return ctx;
}

static gb_session_ctx_t session_ctx(void) {
  gb_session_ctx_t ctx = {
      .mu = &g_app.mu,
      .sessions = g_app.sessions,
      .session_count = MAX_SESSIONS,
      .username = g_app.console_username,
      .password = g_app.console_password,
  };
  return ctx;
}

static void http_fn(struct mg_connection *c, int ev, void *ev_data) {
  if (ev != MG_EV_HTTP_MSG) return;
  struct mg_http_message *hm = (struct mg_http_message *) ev_data;
  gb_session_ctx_t sctx = session_ctx();
  if (!gb_public_uri(hm) && !gb_session_is_valid(hm, &sctx)) {
    if (gb_api_uri(hm)) {
      mg_http_reply(c, 401, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("unauthorized"));
    } else {
      mg_http_reply(c, 302, "Location: /login.html\r\nCache-Control: no-store\r\n", "");
    }
    return;
  }
  if (mg_match(hm->uri, mg_str("/api/login"), NULL) &&
      mg_strcmp(hm->method, mg_str("POST")) == 0) {
    gb_session_reply_login(c, hm, &sctx);
  } else if (mg_match(hm->uri, mg_str("/api/logout"), NULL) &&
             mg_strcmp(hm->method, mg_str("POST")) == 0) {
    gb_session_reply_logout(c, hm, &sctx);
  } else if (mg_match(hm->uri, mg_str("/api/session"), NULL)) {
    gb_session_reply_session(c, &sctx);
  } else if (mg_match(hm->uri, mg_str("/api/settings"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    reply_settings(c);
  } else if (mg_match(hm->uri, mg_str("/api/settings/password"), NULL) &&
             mg_strcmp(hm->method, mg_str("POST")) == 0) {
    save_password(c, hm);
  } else if (mg_match(hm->uri, mg_str("/api/oem"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    reply_oem_config(c);
  } else if (mg_match(hm->uri, mg_str("/api/oem"), NULL) &&
             mg_strcmp(hm->method, mg_str("PUT")) == 0) {
    save_oem_config(c, hm);
  } else if (mg_match(hm->uri, mg_str("/api/updates/check"), NULL) &&
             mg_strcmp(hm->method, mg_str("POST")) == 0) {
    check_updates(c);
  } else if (mg_match(hm->uri, mg_str("/api/ws"), NULL)) {
    mg_ws_upgrade(c, hm, NULL);
    c->data[0] = 'W';
  } else if (mg_match(hm->uri, mg_str("/api/status"), NULL)) {
    reply_status(c);
  } else if (mg_match(hm->uri, mg_str("/api/channels/meta"), NULL)) {
    gb_channel_http_ctx_t ctx = channel_http_ctx();
    gb_channels_reply_meta(c, &ctx);
  } else if (mg_match(hm->uri, mg_str("/api/channels"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    gb_channel_http_ctx_t ctx = channel_http_ctx();
    gb_channels_reply(c, &ctx);
  } else if (mg_match(hm->uri, mg_str("/api/channels"), NULL) &&
             mg_strcmp(hm->method, mg_str("POST")) == 0) {
    gb_channel_http_ctx_t ctx = channel_http_ctx();
    gb_channels_create(c, hm, &ctx);
  } else if (mg_match(hm->uri, mg_str("/api/channels/#"), NULL) &&
             mg_strcmp(hm->method, mg_str("PUT")) == 0) {
    gb_channel_http_ctx_t ctx = channel_http_ctx();
    gb_channels_save(c, hm, uri_id(hm, "/api/channels/"), &ctx);
  } else if (mg_match(hm->uri, mg_str("/api/channels/#"), NULL) &&
             mg_strcmp(hm->method, mg_str("DELETE")) == 0) {
    gb_channel_http_ctx_t ctx = channel_http_ctx();
    gb_channels_delete(c, uri_id(hm, "/api/channels/"), &ctx);
  } else if (mg_match(hm->uri, mg_str("/api/av"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    reply_av(c);
  } else if (mg_match(hm->uri, mg_str("/api/av/#"), NULL) &&
             mg_strcmp(hm->method, mg_str("PUT")) == 0) {
    save_av(c, hm, uri_id(hm, "/api/av/"));
  } else if (mg_match(hm->uri, mg_str("/api/media/files"), NULL)) {
    gb_media_reply_files(c);
  } else if (mg_match(hm->uri, mg_str("/api/devices"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    reply_devices(c);
  } else if (mg_match(hm->uri, mg_str("/api/devices"), NULL) &&
             mg_strcmp(hm->method, mg_str("PUT")) == 0) {
    save_devices(c, hm);
  } else if (mg_match(hm->uri, mg_str("/api/preview.jpg"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    gb_preview_http_ctx_t ctx = {.preview = &g_preview, .app_mu = &g_app.mu, .device = &g_app.device};
    gb_preview_reply_jpeg(c, hm, &ctx);
  } else if (mg_match(hm->uri, mg_str("/api/alarms"), NULL) &&
             mg_strcmp(hm->method, mg_str("GET")) == 0) {
    reply_alarms(c);
  } else if (mg_match(hm->uri, mg_str("/api/logs"), NULL)) {
    (void) hm;
    gb_log_ctx_t ctx = {.mu = &g_app.mu, .db = g_app.db, .file = g_app.file_log};
    gb_log_reply(c, &ctx, MAX_LOG_LINES);
  } else if (mg_match(hm->uri, mg_str("/api/config/export"), NULL)) {
    reply_config_export(c);
  } else if (mg_match(hm->uri, mg_str("/api/config/import"), NULL)) {
    log_add("INFO", "SYSTEM", "config import requested");
    mg_http_reply(c, 202, JSON_HDR, "true\n");
  } else if (mg_match(hm->uri, mg_str("/api/ota"), NULL)) {
    log_add("WARN", "OTA", "ota upload endpoint hit");
    mg_http_reply(c, 202, JSON_HDR, "{%m:%m}\n", MG_ESC("status"), MG_ESC("staged"));
  } else if (mg_match(hm->uri, mg_str("/api/system/reboot"), NULL)) {
    system_action(c, "reboot");
  } else if (mg_match(hm->uri, mg_str("/api/system/factory-reset"), NULL)) {
    system_action(c, "factory-reset");
  } else {
    struct mg_http_serve_opts opts = {.root_dir = s_web_root, .page404 = "index.html"};
    mg_http_serve_dir(c, hm, &opts);
  }
}

static void on_signal(int sig) {
  (void) sig;
  g_app.running = false;
}

static void init_console_auth(void) {
  console_auth_cfg_t auth;
  if (gb_config_load_auth(g_app.db, &auth) != 0) {
    snprintf(auth.username, sizeof(auth.username), "%s", "admin");
    snprintf(auth.password, sizeof(auth.password), "%s", "anyrtc");
  }
  snprintf(g_app.console_username, sizeof(g_app.console_username), "%s", "admin");
  snprintf(g_app.console_password, sizeof(g_app.console_password), "%s", auth.password);
}

int main(int argc, char **argv) {
  if (argc > 1) s_listen = argv[1];
  if (argc > 2) s_db_path = argv[2];
  if (argc > 3) s_web_root = argv[3];
  init_log_config(argc, argv);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  gb_mutex_init(&g_app.mu);
  if (gb_source_manager_init() != 0) {
    fprintf(stderr, "failed to initialize source manager\n");
    gb_mutex_destroy(&g_app.mu);
    return 2;
  }
  gb_preview_init(&g_preview);
  g_app.started_at = time(NULL);
  g_app.running = true;
  db_init();
  if (gb_log_file_init(&g_app.file_log, &s_log_config) != 0) {
    fprintf(stderr,
            "failed to initialize file logs dir=%s max=%llu rotate=%d\n",
            s_log_config.directory,
            s_log_config.max_file_bytes,
            s_log_config.rotate_count);
  }
  init_console_auth();
  load_config();
  gb_agent_callbacks_t gb_callbacks = {.user_data = NULL, .on_log = gb_log_callback};
  if (gb_agent_create(&gb_callbacks, &g_app.gb_agent) != 0) {
    fprintf(stderr, "failed to create GB28181 agent\n");
    sqlite3_close(g_app.db);
    gb_mutex_destroy(&g_app.mu);
    return 2;
  }
  log_add("INFO", "SYSTEM", "GB28181 Embedded Agent started");

  gb_thread_t tid;
  gb_preview_start(&g_preview);
  gb_thread_create(&tid, agent_thread, NULL);

  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  mg_log_set(MG_LL_INFO);
  mg_timer_add(&mgr, 1000, MG_TIMER_REPEAT, timer_fn, &mgr);
  if (mg_http_listen(&mgr, s_listen, http_fn, NULL) == NULL) {
    fprintf(stderr, "listen failed: %s\n", s_listen);
    g_app.running = false;
  } else {
    fprintf(stderr, "GB28181 Embedded Agent listening on %s\n", s_listen);
  }
  while (g_app.running) mg_mgr_poll(&mgr, 200);
  mg_mgr_free(&mgr);
  gb_preview_stop(&g_preview);
  gb_thread_join(tid);
  gb_agent_destroy(&g_app.gb_agent);
  gb_source_manager_cleanup();
  gb_log_file_close(&g_app.file_log);
  sqlite3_close(g_app.db);
  gb_mutex_destroy(&g_app.mu);
  return 0;
}
