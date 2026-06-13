#include "capture_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  gbmc_capture_t base;
  gbmc_capture_config_t config;
  gbmc_capture_callbacks_t callbacks;
  int started;
} gbmc_null_capture_t;

static int null_probe(void) {
  return GBMC_OK;
}

static int null_list_devices(gbmc_media_type_t media_type,
                             gbmc_device_info_t *devices,
                             size_t max_devices,
                             size_t *count_out) {
  if (!count_out) return GBMC_ERR_INVALID;
  *count_out = 1;
  if (devices && max_devices > 0) {
    memset(&devices[0], 0, sizeof(devices[0]));
    strcpy(devices[0].backend, "null");
    strcpy(devices[0].id, media_type == GBMC_MEDIA_VIDEO ? "null-video" : "null-audio");
    strcpy(devices[0].name, media_type == GBMC_MEDIA_VIDEO
                              ? "Null video capture"
                              : "Null audio capture");
    devices[0].media_type = media_type;
  }
  return GBMC_OK;
}

static int null_open(const gbmc_capture_config_t *config,
                     const gbmc_capture_callbacks_t *callbacks,
                     gbmc_capture_t **capture_out) {
  if (!config || !capture_out) return GBMC_ERR_INVALID;
  gbmc_null_capture_t *capture = (gbmc_null_capture_t *) calloc(1, sizeof(*capture));
  if (!capture) return GBMC_ERR_NO_MEMORY;
  capture->base.backend = gbmc_null_backend();
  capture->config = *config;
  if (callbacks) capture->callbacks = *callbacks;
  *capture_out = &capture->base;
  return GBMC_OK;
}

static void null_close(gbmc_capture_t *capture) {
  free(capture);
}

static int null_start(gbmc_capture_t *capture) {
  if (!capture) return GBMC_ERR_INVALID;
  ((gbmc_null_capture_t *) capture)->started = 1;
  return GBMC_OK;
}

static int null_stop(gbmc_capture_t *capture) {
  if (!capture) return GBMC_ERR_INVALID;
  ((gbmc_null_capture_t *) capture)->started = 0;
  return GBMC_OK;
}

const gbmc_capture_backend_t *gbmc_null_backend(void) {
  static const gbmc_capture_backend_t backend = {
      "null",
      "Portable no-op capture backend for tests and unsupported platforms",
      -1000,
      null_probe,
      null_list_devices,
      null_open,
      null_close,
      null_start,
      null_stop,
  };
  return &backend;
}
