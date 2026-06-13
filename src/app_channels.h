#ifndef APP_CHANNELS_H
#define APP_CHANNELS_H

#include "app_config.h"
#include "gb_agent.h"
#include "gb_platform.h"
#include "mongoose.h"

#include <sqlite3.h>

typedef struct {
  gb_mutex_t *mu;
  sqlite3 *db;
  gb_agent_t *gb_agent;
  int *channel_count;
  platform_cfg_t *platforms;
  gb_channel_cfg_t *gb_channels;
  device_source_cfg_t *sources;
  char (*source_profiles)[16];
  gb_agent_status_t *gb_status;
  unsigned long *platform_generation;
  unsigned long *media_generation;
  void (*reload_config)(void);
  void (*log_add)(const char *level, const char *category, const char *message);
} gb_channel_http_ctx_t;

void gb_channels_reply(struct mg_connection *c, gb_channel_http_ctx_t *ctx);
void gb_channels_reply_meta(struct mg_connection *c, gb_channel_http_ctx_t *ctx);
void gb_channels_save(struct mg_connection *c, struct mg_http_message *hm, int id, gb_channel_http_ctx_t *ctx);
void gb_channels_create(struct mg_connection *c, struct mg_http_message *hm, gb_channel_http_ctx_t *ctx);
void gb_channels_delete(struct mg_connection *c, int id, gb_channel_http_ctx_t *ctx);

#endif
