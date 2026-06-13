#include "app_channels.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_HDR "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n"

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
  return appendf(buf, cap, len, ",\"register_interval\":%d,\"heartbeat_interval\":%d}",
                 p->register_interval, p->heartbeat_interval);
}

static size_t append_channel_json(char *buf,
                                  size_t cap,
                                  size_t len,
                                  const platform_cfg_t *p,
                                  const gb_channel_cfg_t *channel,
                                  const device_source_cfg_t *source,
                                  const char *profile,
                                  const gb_agent_channel_status_t *status) {
  len = append_platform_json(buf, cap, len, p);
  if (len > 0 && len < cap && buf[len - 1] == '}') len--;
  if (channel != NULL) {
    len = appendf(buf, cap, len, ",\"client_id\":%d,\"ordinal\":%d,\"channel_id\":",
                  channel->client_id,
                  channel->ordinal);
    len = append_json_string(buf, cap, len, channel->channel_id);
  }
  len = appendf(buf, cap, len, ",\"source_profile\":");
  len = append_json_string(buf, cap, len, profile && profile[0] ? profile : "none");
  len = appendf(buf, cap, len, ",\"source_mode\":");
  len = append_json_string(buf, cap, len, profile && strcmp(profile, "none") == 0 ? "none" : source->source_mode);
  len = appendf(buf, cap, len, ",\"video_device\":");
  len = append_json_string(buf, cap, len, profile && strcmp(profile, "none") == 0 ? "" : source->video_device);
  len = appendf(buf, cap, len, ",\"audio_device\":");
  len = append_json_string(buf, cap, len, profile && strcmp(profile, "none") == 0 ? "" : source->audio_device);
  len = appendf(buf, cap, len, ",\"media_file\":");
  len = append_json_string(buf, cap, len, profile && strcmp(profile, "none") == 0 ? "" : source->media_file);
  len = appendf(buf, cap, len, ",\"resolution\":");
  len = append_json_string(buf, cap, len, source->resolution);
  len = appendf(buf, cap, len, ",\"bitrate_kbps\":%d,\"file_loop\":%d,\"file_pacing\":",
                source->bitrate_kbps, source->file_loop);
  len = append_json_string(buf, cap, len, source->file_pacing);
  if (status != NULL) {
    len = appendf(buf,
                  cap,
                  len,
                  ",\"push_active\":%d,\"talkback_active\":%d,\"broadcast_active\":%d,"
                  "\"media_running\":%d,\"media_generation\":%lu",
                  status->push_active,
                  status->talkback_active,
                  status->broadcast_active,
                  status->media_running,
                  status->media_generation);
  }
  return appendf(buf, cap, len, "}");
}

static int text_blank(const char *s) {
  if (s == NULL) return 1;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  return *s == '\0';
}

static int ascii_ieq(const char *a, const char *b) {
  if (a == NULL || b == NULL) return 0;
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = (char) (ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char) (cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return *a == '\0' && *b == '\0';
}

static void normalize_transport(char *transport, size_t size) {
  if (transport == NULL || size == 0) return;
  if (ascii_ieq(transport, "TCP")) {
    snprintf(transport, size, "%s", "TCP");
  } else if (ascii_ieq(transport, "TrUdp") || ascii_ieq(transport, "TR-UDP") ||
             ascii_ieq(transport, "TR_UDP")) {
    snprintf(transport, size, "%s", "TrUdp");
  } else {
    snprintf(transport, size, "%s", "UDP");
  }
}

static void normalize_media_proto(char *media_proto, size_t size) {
  if (media_proto == NULL || size == 0) return;
  if (ascii_ieq(media_proto, "RTP")) {
    snprintf(media_proto, size, "%s", "RTP");
  } else {
    snprintf(media_proto, size, "%s", "RTC");
  }
}

static int transport_changed(const char *a, const char *b) {
  return strcmp(a != NULL ? a : "", b != NULL ? b : "") != 0;
}

static const char *platform_missing_field(const platform_cfg_t *p) {
  if (p == NULL) return "platform";
  if (text_blank(p->server_ip)) return "server_ip";
  if (p->sip_port <= 0) return "sip_port";
  if (text_blank(p->sip_id)) return "sip_id";
  if (text_blank(p->device_id)) return "device_id";
  if (text_blank(p->username)) return "username";
  if (text_blank(p->password)) return "password";
  if (strcmp(p->transport, "UDP") != 0 && strcmp(p->transport, "TCP") != 0 &&
      strcmp(p->transport, "TrUdp") != 0) return "transport";
  if (strcmp(p->media_proto, "RTP") != 0 && strcmp(p->media_proto, "RTC") != 0) return "media_proto";
  if (p->register_interval <= 0) return "register_interval";
  if (p->heartbeat_interval <= 0) return "heartbeat_interval";
  return NULL;
}

static int channel_source_equal(const device_source_cfg_t *a,
                                const device_source_cfg_t *b,
                                const char *profile_a,
                                const char *profile_b) {
  if (a == NULL || b == NULL) return 0;
  if (strcmp(profile_a ? profile_a : "", profile_b ? profile_b : "") != 0) return 0;
  return strcmp(a->source_mode, b->source_mode) == 0 &&
         strcmp(a->video_device, b->video_device) == 0 &&
         strcmp(a->audio_device, b->audio_device) == 0 &&
         strcmp(a->media_file, b->media_file) == 0 &&
         strcmp(a->resolution, b->resolution) == 0 &&
         a->bitrate_kbps == b->bitrate_kbps &&
         a->file_loop == b->file_loop &&
         strcmp(a->file_pacing, b->file_pacing) == 0;
}

static int any_client_locked(const gb_channel_http_ctx_t *ctx) {
  if (ctx == NULL) return 0;
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if ((ctx->platforms != NULL && ctx->platforms[i].enabled) ||
        (ctx->gb_status != NULL &&
         (ctx->gb_status[i].desired_enabled ||
          ctx->gb_status[i].push_active))) {
      return 1;
    }
  }
  return 0;
}

void gb_channels_reply(struct mg_connection *c, gb_channel_http_ctx_t *ctx) {
  char body[32768];
  size_t len = 0;
  platform_cfg_t platforms[MAX_PLATFORMS];
  gb_channel_cfg_t channels[MAX_CHANNELS];
  device_source_cfg_t sources[MAX_CHANNELS];
  char profiles[MAX_CHANNELS][16];
  gb_mutex_lock(ctx->mu);
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    platforms[i] = ctx->platforms[i];
  }
  for (int i = 0; i < MAX_CHANNELS; i++) {
    channels[i] = ctx->gb_channels[i];
    sources[i] = ctx->sources[i];
    snprintf(profiles[i], sizeof(profiles[i]), "%s", ctx->source_profiles[i]);
  }
  gb_mutex_unlock(ctx->mu);
  len = appendf(body, sizeof(body), len, "[");
  int emitted = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) {
    platform_cfg_t out;
    int client_idx;
    int exists;
    if (channels[i].id <= 0) continue;
    client_idx = channels[i].client_id - 1;
    if (client_idx < 0 || client_idx >= MAX_PLATFORMS) continue;
    gb_mutex_lock(ctx->mu);
    exists = gb_config_channel_exists(ctx->db, i + 1);
    gb_mutex_unlock(ctx->mu);
    if (!exists) continue;
    out = platforms[client_idx];
    out.id = channels[i].id;
    snprintf(out.name, sizeof(out.name), "%s", channels[i].name);
    snprintf(out.media_proto, sizeof(out.media_proto), "%s", channels[i].media_proto);
    len = appendf(body, sizeof(body), len, "%s", emitted++ ? "," : "");
    {
      gb_agent_channel_status_t status;
      memset(&status, 0, sizeof(status));
      if (ctx->gb_agent != NULL) {
        (void) gb_agent_get_channel_status(ctx->gb_agent, channels[i].id, &status);
      }
      len = append_channel_json(body, sizeof(body), len, &out, &channels[i], &sources[i], profiles[i], &status);
    }
  }
  len = appendf(body, sizeof(body), len, "]\n");
  body[sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}

void gb_channels_reply_meta(struct mg_connection *c, gb_channel_http_ctx_t *ctx) {
  (void) ctx;
  mg_http_reply(c,
                200,
                JSON_HDR,
                "{%m:%d,%m:%d,%m:%d}\n",
                MG_ESC("max_clients"),
                MAX_GB_CLIENTS,
                MG_ESC("channels_per_client"),
                MAX_CHANNELS_PER_CLIENT,
                MG_ESC("max_channels"),
                MAX_CHANNELS);
}

void gb_channels_save(struct mg_connection *c, struct mg_http_message *hm, int id, gb_channel_http_ctx_t *ctx) {
  if (id < 1 || id > MAX_CHANNELS) {
    mg_http_reply(c, 404, JSON_HDR, "{\"error\":\"channel not found\"}\n");
    return;
  }
  if (mg_json_get(hm->body, "$", NULL) < 0) {
    mg_http_reply(c, 400, JSON_HDR, "{\"error\":\"invalid json\"}\n");
    return;
  }
  platform_cfg_t p;
  gb_channel_cfg_t ch;
  gb_agent_status_t old_status;
  device_source_cfg_t source;
  device_source_cfg_t old_source;
  char source_profile[16];
  char old_source_profile[16];
  gb_mutex_lock(ctx->mu);
  ch = ctx->gb_channels[id - 1];
  if (ch.client_id < 1 || ch.client_id > MAX_PLATFORMS) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 404, JSON_HDR, "{\"error\":\"channel not found\"}\n");
    return;
  }
  p = ctx->platforms[ch.client_id - 1];
  p.id = id;
  if (ch.id > 0) {
    snprintf(p.name, sizeof(p.name), "%s", ch.name);
    snprintf(p.media_proto, sizeof(p.media_proto), "%s", ch.media_proto);
  }
  memset(&old_status, 0, sizeof(old_status));
  if (ctx->gb_status != NULL) old_status = ctx->gb_status[ch.client_id - 1];
  source = ctx->sources[id - 1];
  old_source = source;
  snprintf(source_profile, sizeof(source_profile), "%s", ctx->source_profiles[id - 1]);
  snprintf(old_source_profile, sizeof(old_source_profile), "%s", source_profile);
  gb_mutex_unlock(ctx->mu);

  char *s = NULL;
  bool b = false;
  p.enabled = (int) mg_json_get_long(hm->body, "$.enabled", p.enabled);
  p.sip_port = (int) mg_json_get_long(hm->body, "$.sip_port", p.sip_port);
  p.register_interval = (int) mg_json_get_long(hm->body, "$.register_interval", p.register_interval);
  p.heartbeat_interval = (int) mg_json_get_long(hm->body, "$.heartbeat_interval", p.heartbeat_interval);
  if (mg_json_get_bool(hm->body, "$.enabled", &b)) p.enabled = b ? 1 : 0;
  if ((s = mg_json_get_str(hm->body, "$.name")) != NULL) { snprintf(p.name, sizeof(p.name), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.server_ip")) != NULL) { snprintf(p.server_ip, sizeof(p.server_ip), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.sip_id")) != NULL) { snprintf(p.sip_id, sizeof(p.sip_id), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.device_id")) != NULL) { snprintf(p.device_id, sizeof(p.device_id), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.username")) != NULL) { snprintf(p.username, sizeof(p.username), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.password")) != NULL) {
    if (s[0] != '\0') snprintf(p.password, sizeof(p.password), "%s", s);
    free(s);
  }
  if ((s = mg_json_get_str(hm->body, "$.transport")) != NULL) { snprintf(p.transport, sizeof(p.transport), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.media_proto")) != NULL) { snprintf(p.media_proto, sizeof(p.media_proto), "%s", s); free(s); }
  normalize_transport(p.transport, sizeof(p.transport));
  normalize_media_proto(p.media_proto, sizeof(p.media_proto));
  if (old_status.push_active && transport_changed(old_status.transport, p.transport)) {
    mg_http_reply(c, 409, JSON_HDR,
                  "{\"error\":\"channel is pushing; transport cannot be changed\","
                  "\"field\":\"transport\"}\n");
    return;
  }
  if ((s = mg_json_get_str(hm->body, "$.source_profile")) != NULL) {
    snprintf(source_profile,
             sizeof(source_profile),
             "%s",
             strcmp(s, "custom") == 0 ? "custom" : (strcmp(s, "global") == 0 ? "global" : "none"));
    free(s);
  }
  if ((s = mg_json_get_str(hm->body, "$.source_mode")) != NULL) {
    snprintf(source.source_mode,
             sizeof(source.source_mode),
             "%s",
             strcmp(s, "file") == 0 ? "file" : (strcmp(s, "screen") == 0 ? "screen" : "device"));
    free(s);
  }
  if ((s = mg_json_get_str(hm->body, "$.video_device")) != NULL) { snprintf(source.video_device, sizeof(source.video_device), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.audio_device")) != NULL) { snprintf(source.audio_device, sizeof(source.audio_device), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.media_file")) != NULL) { snprintf(source.media_file, sizeof(source.media_file), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.resolution")) != NULL) { snprintf(source.resolution, sizeof(source.resolution), "%s", s); free(s); }
  if ((s = mg_json_get_str(hm->body, "$.file_pacing")) != NULL) {
    snprintf(source.file_pacing, sizeof(source.file_pacing), "%s", strcmp(s, "fast") == 0 ? "fast" : "realtime");
    free(s);
  }
  source.bitrate_kbps = (int) mg_json_get_long(hm->body, "$.bitrate_kbps", source.bitrate_kbps);
  if (mg_json_get_bool(hm->body, "$.file_loop", &b)) source.file_loop = b ? 1 : 0;
  if (strcmp(source_profile, "none") == 0) {
    snprintf(source.source_mode, sizeof(source.source_mode), "%s", "device");
    source.video_device[0] = '\0';
    source.audio_device[0] = '\0';
    source.media_file[0] = '\0';
    source.resolution[0] = '\0';
    source.bitrate_kbps = 0;
    source.file_loop = 1;
    snprintf(source.file_pacing, sizeof(source.file_pacing), "%s", "realtime");
  } else if (strcmp(source.source_mode, "screen") == 0) {
    source.audio_device[0] = '\0';
    source.media_file[0] = '\0';
  }
  if (p.enabled) {
    const char *missing = platform_missing_field(&p);
    if (missing != NULL) {
      mg_http_reply(c, 400, JSON_HDR,
                    "{\"error\":\"platform config incomplete\","
                    "\"field\":\"%s\"}\n",
                    missing);
      return;
    }
  }
  if (old_status.push_active &&
      !channel_source_equal(&old_source, &source, old_source_profile, source_profile)) {
    mg_http_reply(c, 409, JSON_HDR,
                  "{\"error\":\"channel is pushing; bound source cannot be changed\","
                  "\"field\":\"source_profile\"}\n");
    return;
  }

  gb_mutex_lock(ctx->mu);
  if (ch.id <= 0) {
    int created_id = 0;
    if (any_client_locked(ctx)) {
      gb_mutex_unlock(ctx->mu);
      mg_http_reply(c, 409, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("client registered; channels are locked"));
      return;
    }
    if (gb_config_save_client(ctx->db, ch.client_id, &p, &created_id) != 0) {
      gb_mutex_unlock(ctx->mu);
      mg_http_reply(c, 500, JSON_HDR, "{\"error\":\"save client failed\"}\n");
      return;
    }
    id = created_id > 0 ? created_id : id;
    ch = ctx->gb_channels[id - 1];
    init_gb_channel_default(&ctx->gb_channels[id - 1], id, ch.client_id, 1, p.device_id);
  } else if (gb_config_save_channel(ctx->db, id, &p, &source, source_profile) != 0) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 500, JSON_HDR, "{\"error\":\"save channel failed\"}\n");
    return;
  }
  if (ch.client_id >= 1 && ch.client_id <= MAX_PLATFORMS) {
    platform_cfg_t client_platform = p;
    client_platform.id = ch.client_id;
    ctx->platforms[ch.client_id - 1] = client_platform;
  }
  snprintf(ctx->gb_channels[id - 1].name, sizeof(ctx->gb_channels[id - 1].name), "%s", p.name);
  snprintf(ctx->gb_channels[id - 1].media_proto, sizeof(ctx->gb_channels[id - 1].media_proto), "%s", p.media_proto);
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (ctx->gb_channels[i].client_id == ch.client_id) {
      (void) gb_channel_make_id(p.device_id,
                                ctx->gb_channels[i].ordinal,
                                ctx->gb_channels[i].channel_id,
                                sizeof(ctx->gb_channels[i].channel_id));
    }
  }
  ctx->sources[id - 1] = source;
  snprintf(ctx->source_profiles[id - 1], 16, "%s", source_profile);
  (*ctx->platform_generation)++;
  (*ctx->media_generation)++;
  gb_mutex_unlock(ctx->mu);
  if (ctx->log_add) ctx->log_add("INFO", "CONFIG", "channel config updated");
  mg_http_reply(c, 200, JSON_HDR, "true\n");
}

void gb_channels_create(struct mg_connection *c, struct mg_http_message *hm, gb_channel_http_ctx_t *ctx) {
  int id = 0;
  int rc;
  int client_id = 1;
  if (ctx == NULL) {
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid context"));
    return;
  }
  gb_mutex_lock(ctx->mu);
  if (any_client_locked(ctx)) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 409, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("client registered; channels are locked"));
    return;
  }
  if (hm != NULL && mg_json_get(hm->body, "$", NULL) >= 0) {
    client_id = (int) mg_json_get_long(hm->body, "$.client_id", 1);
  }
  if (client_id < 1 || client_id > MAX_GB_CLIENTS) client_id = 1;
  rc = gb_config_create_client_channel(ctx->db, client_id, &id);
  if (rc == 1) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 409, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("channel limit reached"));
    return;
  }
  if (rc != 0) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("create channel failed"));
    return;
  }
  (*ctx->platform_generation)++;
  (*ctx->media_generation)++;
  gb_mutex_unlock(ctx->mu);
  if (ctx->reload_config) ctx->reload_config();
  if (ctx->log_add) ctx->log_add("INFO", "CONFIG", "channel created");
  gb_channels_reply(c, ctx);
}

void gb_channels_delete(struct mg_connection *c, int id, gb_channel_http_ctx_t *ctx) {
  if (id < 1 || id > MAX_CHANNELS) {
    mg_http_reply(c, 404, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("channel not found"));
    return;
  }
  gb_mutex_lock(ctx->mu);
  if (!gb_config_channel_exists(ctx->db, id)) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 404, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("channel not found"));
    return;
  }
  if (ctx->gb_channels[id - 1].ordinal == 1 && ctx->gb_channels[id - 1].client_id == 1) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 409, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("default channel cannot be deleted"));
    return;
  }
  if (any_client_locked(ctx)) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 409, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("client registered; channels are locked"));
    return;
  }
  int rc = gb_config_delete_channel(ctx->db, id);
  if (rc == 1) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 409, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("at least one channel is required"));
    return;
  }
  if (rc != 0) {
    gb_mutex_unlock(ctx->mu);
    mg_http_reply(c, 500, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("delete channel failed"));
    return;
  }
  init_gb_channel_default(&ctx->gb_channels[id - 1],
                          id,
                          (id - 1) / MAX_CHANNELS_PER_CLIENT + 1,
                          (id - 1) % MAX_CHANNELS_PER_CLIENT + 1,
                          ctx->platforms[(id - 1) / MAX_CHANNELS_PER_CLIENT].device_id);
  ctx->gb_channels[id - 1].id = 0;
  init_device_source_default(&ctx->sources[id - 1]);
  snprintf(ctx->source_profiles[id - 1], 16, "%s", "none");
  (*ctx->platform_generation)++;
  (*ctx->media_generation)++;
  gb_mutex_unlock(ctx->mu);
  if (ctx->gb_agent != NULL) (void) gb_agent_stop_platform_media_source(ctx->gb_agent, id);
  if (ctx->reload_config) ctx->reload_config();
  if (ctx->log_add) ctx->log_add("INFO", "CONFIG", "channel deleted");
  gb_channels_reply(c, ctx);
}
