#ifndef APP_PREVIEW_H
#define APP_PREVIEW_H

#include "app_config.h"
#include "gb_platform.h"
#include "mongoose.h"
#include "source_manager.h"

#include <stdint.h>

typedef struct {
  gb_mutex_t mu;
  gb_thread_t thread;
  volatile int running;
  unsigned request_generation;
  char request_source[512];
  int request_is_device;
  gb_source_runtime_config_t request_config;
  time_t request_seen_at;
  unsigned active_generation;
  char active_source[512];
  int active_is_device;
  uint8_t *jpeg;
  size_t jpeg_size;
  uint8_t *no_signal_jpeg;
  size_t no_signal_jpeg_size;
} preview_cache_t;

typedef struct {
  preview_cache_t *preview;
  gb_mutex_t *app_mu;
  device_source_cfg_t *device;
} gb_preview_http_ctx_t;

void gb_preview_init(preview_cache_t *preview);
void gb_preview_start(preview_cache_t *preview);
void gb_preview_stop(preview_cache_t *preview);
void gb_preview_reply_jpeg(struct mg_connection *c, struct mg_http_message *hm, gb_preview_http_ctx_t *ctx);

#endif
