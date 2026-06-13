#include "app_preview.h"

#include "app_media.h"
#include "gb_platform.h"
#include "preview_jpeg.h"
#include "source_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void send_preview_bytes(struct mg_connection *c, uint8_t *jpeg, size_t jpeg_size) {
  mg_printf(c,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "Content-Length: %llu\r\n\r\n",
            (unsigned long long) jpeg_size);
  mg_send(c, jpeg, jpeg_size);
  c->is_resp = 0;
  c->is_draining = 1;
}

static int preview_no_signal_snapshot(preview_cache_t *preview, uint8_t **jpeg_out, size_t *jpeg_size_out) {
  int ok = 0;
  *jpeg_out = NULL;
  *jpeg_size_out = 0;
  gb_mutex_lock(&preview->mu);
  if (preview->no_signal_jpeg && preview->no_signal_jpeg_size > 0) {
    uint8_t *copy = (uint8_t *) malloc(preview->no_signal_jpeg_size);
    if (copy) {
      memcpy(copy, preview->no_signal_jpeg, preview->no_signal_jpeg_size);
      *jpeg_out = copy;
      *jpeg_size_out = preview->no_signal_jpeg_size;
      ok = 1;
    }
  }
  gb_mutex_unlock(&preview->mu);
  return ok;
}

static int preview_no_signal_make(preview_cache_t *preview, uint8_t **jpeg_out, size_t *jpeg_size_out) {
  uint8_t *jpeg = NULL;
  size_t jpeg_size = 0;
  if (preview_no_signal_snapshot(preview, jpeg_out, jpeg_size_out)) return 0;
  if (gb_preview_jpeg_no_signal(960, 540, &jpeg, &jpeg_size) != 0) return -1;
  gb_mutex_lock(&preview->mu);
  if (!preview->no_signal_jpeg) {
    preview->no_signal_jpeg = jpeg;
    preview->no_signal_jpeg_size = jpeg_size;
    jpeg = NULL;
  }
  gb_mutex_unlock(&preview->mu);
  gb_preview_jpeg_free(&jpeg);
  return preview_no_signal_snapshot(preview, jpeg_out, jpeg_size_out) ? 0 : -1;
}

static void preview_cache_store(preview_cache_t *preview,
                                unsigned generation,
                                const char *source,
                                int is_device,
                                uint8_t *jpeg,
                                size_t jpeg_size) {
  gb_mutex_lock(&preview->mu);
  if (preview->request_generation == generation) {
    gb_preview_jpeg_free(&preview->jpeg);
    preview->jpeg = jpeg;
    preview->jpeg_size = jpeg_size;
    preview->active_generation = generation;
    snprintf(preview->active_source, sizeof(preview->active_source), "%s", source ? source : "");
    preview->active_is_device = is_device;
    jpeg = NULL;
  }
  gb_mutex_unlock(&preview->mu);
  gb_preview_jpeg_free(&jpeg);
}

static const char *preview_capture_backend(const device_source_cfg_t *source) {
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

static void preview_runtime_config_from_device(const device_source_cfg_t *source,
                                               gb_source_runtime_config_t *config) {
  if (config == NULL) return;
  memset(config, 0, sizeof(*config));
  if (source == NULL) return;
  snprintf(config->source_mode, sizeof(config->source_mode), "%s", source->source_mode);
  snprintf(config->backend_name, sizeof(config->backend_name), "%s", preview_capture_backend(source));
  snprintf(config->video_device, sizeof(config->video_device), "%s", source->video_device);
  config->audio_device[0] = '\0';
  snprintf(config->media_file, sizeof(config->media_file), "%s", source->media_file);
  config->resolution[0] = '\0';
  config->bitrate_kbps = source->bitrate_kbps;
  config->fps = 0;
  config->loop = source->file_loop;
  snprintf(config->file_pacing, sizeof(config->file_pacing), "%s", source->file_pacing);
}

static void preview_cache_request(preview_cache_t *preview,
                                  const char *source,
                                  int is_device,
                                  const gb_source_runtime_config_t *config) {
  gb_mutex_lock(&preview->mu);
  preview->request_seen_at = time(NULL);
  if (strcmp(preview->request_source, source ? source : "") != 0 ||
      preview->request_is_device != is_device) {
    snprintf(preview->request_source, sizeof(preview->request_source), "%s", source ? source : "");
    preview->request_is_device = is_device;
    preview->request_generation++;
  }
  if (config != NULL) preview->request_config = *config;
  gb_mutex_unlock(&preview->mu);
}

static int preview_cache_snapshot(preview_cache_t *preview,
                                  const char *source,
                                  int is_device,
                                  uint8_t **jpeg_out,
                                  size_t *jpeg_size_out) {
  int ok = 0;
  *jpeg_out = NULL;
  *jpeg_size_out = 0;
  gb_mutex_lock(&preview->mu);
  if (preview->jpeg && preview->jpeg_size > 0 &&
      strcmp(preview->active_source, source ? source : "") == 0 &&
      preview->active_is_device == is_device) {
    uint8_t *copy = (uint8_t *) malloc(preview->jpeg_size);
    if (copy) {
      memcpy(copy, preview->jpeg, preview->jpeg_size);
      *jpeg_out = copy;
      *jpeg_size_out = preview->jpeg_size;
      ok = 1;
    }
  }
  gb_mutex_unlock(&preview->mu);
  return ok;
}

static void *preview_thread(void *arg) {
  preview_cache_t *preview = (preview_cache_t *) arg;
  unsigned last_generation = 0;
  int64_t file_position_ms = 0;
  char lease_key[512] = {0};
  while (preview->running) {
    unsigned generation;
    char source[512];
    int is_device;
    gb_source_runtime_config_t runtime_config;
    time_t request_seen_at;
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    int rc = -1;
    int fallback_no_signal = 0;

    gb_mutex_lock(&preview->mu);
    generation = preview->request_generation;
    snprintf(source, sizeof(source), "%s", preview->request_source);
    is_device = preview->request_is_device;
    runtime_config = preview->request_config;
    request_seen_at = preview->request_seen_at;
    gb_mutex_unlock(&preview->mu);

    if (generation != last_generation) {
      file_position_ms = 0;
      if (lease_key[0]) {
        (void) gb_source_release(lease_key, "preview");
        lease_key[0] = '\0';
      }
      last_generation = generation;
    }

    if (request_seen_at == 0 || time(NULL) - request_seen_at > 2) {
      if (lease_key[0]) {
        (void) gb_source_release(lease_key, "preview");
        lease_key[0] = '\0';
      }
      gb_sleep_ms(GB_PREVIEW_INTERVAL_MS);
      continue;
    }

    if (source[0] == '\0') {
      rc = preview_no_signal_make(preview, &jpeg, &jpeg_size);
    } else if (is_device) {
      char key[512];
      gb_source_key(key, sizeof(key), is_device == 2 ? "screen" : "device", source);
      if (strcmp(lease_key, key) != 0) {
        if (lease_key[0]) (void) gb_source_release(lease_key, "preview");
        snprintf(lease_key, sizeof(lease_key), "%s", key);
        (void) gb_source_acquire_runtime(lease_key, "preview", &runtime_config);
      }
      if (gb_source_snapshot_jpeg(key, &jpeg, &jpeg_size)) {
        rc = 0;
      } else if (gb_source_ref_count(key) > 1) {
        uint8_t *raw = NULL;
        size_t raw_size = 0;
        int raw_width = 0;
        int raw_height = 0;
        char raw_format[32] = {0};
        if (gb_source_snapshot_raw(key, &raw, &raw_size, &raw_width, &raw_height, raw_format, sizeof(raw_format))) {
          rc = gb_preview_jpeg_from_raw(raw, raw_size, raw_width, raw_height, raw_format, 960, 540, &jpeg, &jpeg_size);
          free(raw);
        } else {
          rc = -1;
        }
      }
      if (rc != 0 || jpeg_size == 0) {
        gb_preview_jpeg_free(&jpeg);
        rc = preview_no_signal_make(preview, &jpeg, &jpeg_size);
        fallback_no_signal = 1;
      }
    } else {
      rc = gb_preview_jpeg_make_at_ms(source, file_position_ms, 960, 540, &jpeg, &jpeg_size);
      file_position_ms += GB_PREVIEW_INTERVAL_MS;
      if (rc != 0 || jpeg_size == 0) {
        gb_preview_jpeg_free(&jpeg);
        rc = preview_no_signal_make(preview, &jpeg, &jpeg_size);
        fallback_no_signal = 1;
      }
    }

    if (rc == 0 && jpeg_size > 0 && !fallback_no_signal) {
      preview_cache_store(preview, generation, source, is_device, jpeg, jpeg_size);
    } else {
      gb_preview_jpeg_free(&jpeg);
    }
    gb_sleep_ms(GB_PREVIEW_INTERVAL_MS);
  }
  if (lease_key[0]) (void) gb_source_release(lease_key, "preview");
  return NULL;
}

void gb_preview_init(preview_cache_t *preview) {
  memset(preview, 0, sizeof(*preview));
  gb_mutex_init(&preview->mu);
}

void gb_preview_start(preview_cache_t *preview) {
  preview->running = 1;
  gb_thread_create(&preview->thread, preview_thread, preview);
}

void gb_preview_stop(preview_cache_t *preview) {
  preview->running = 0;
  gb_thread_join(preview->thread);
  gb_preview_jpeg_free(&preview->jpeg);
  gb_preview_jpeg_free(&preview->no_signal_jpeg);
  gb_mutex_destroy(&preview->mu);
}

void gb_preview_reply_jpeg(struct mg_connection *c, struct mg_http_message *hm, gb_preview_http_ctx_t *ctx) {
  device_source_cfg_t d;
  uint8_t *jpeg = NULL;
  size_t jpeg_size = 0;
  char source[512];
  char mode[16];
  int is_device = 0;

  gb_mutex_lock(ctx->app_mu);
  d = *ctx->device;
  gb_mutex_unlock(ctx->app_mu);

  if (mg_http_get_var(&hm->query, "source_mode", mode, sizeof(mode)) > 0) {
    snprintf(d.source_mode,
             sizeof(d.source_mode),
             "%s",
             strcmp(mode, "none") == 0 ? "none" : (strcmp(mode, "file") == 0 ? "file" : (strcmp(mode, "screen") == 0 ? "screen" : "device")));
  }
  if (strcmp(d.source_mode, "none") == 0) {
    if (gb_preview_jpeg_no_source(960, 540, &jpeg, &jpeg_size) != 0) {
      mg_http_reply(c, 503,
                    "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n",
                    "No source\n");
      return;
    }
    send_preview_bytes(c, jpeg, jpeg_size);
    gb_preview_jpeg_free(&jpeg);
    return;
  }
  if (strcmp(d.source_mode, "file") == 0) {
    (void) mg_http_get_var(&hm->query, "media_file", d.media_file, sizeof(d.media_file));
    snprintf(source, sizeof(source), "%s", d.media_file);
    if (!gb_media_file_allowed(source)) source[0] = '\0';
  } else {
    (void) mg_http_get_var(&hm->query, "video_device", d.video_device, sizeof(d.video_device));
    snprintf(source, sizeof(source), "%s", d.video_device);
    is_device = strcmp(d.source_mode, "screen") == 0 ? 2 : 1;
  }

  gb_source_runtime_config_t runtime_config;
  preview_runtime_config_from_device(&d, &runtime_config);
  preview_cache_request(ctx->preview, source, is_device, &runtime_config);
  for (int i = 0; i < 8 && !preview_cache_snapshot(ctx->preview, source, is_device, &jpeg, &jpeg_size); i++) {
    gb_sleep_ms(100);
  }
  if (jpeg == NULL && preview_no_signal_make(ctx->preview, &jpeg, &jpeg_size) != 0) {
    mg_http_reply(c, 503,
                  "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n",
                  "No signal\n");
    return;
  }
  send_preview_bytes(c, jpeg, jpeg_size);
  gb_preview_jpeg_free(&jpeg);
}
