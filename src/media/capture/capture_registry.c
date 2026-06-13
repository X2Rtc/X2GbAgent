#include "capture_internal.h"

#include <stdlib.h>
#include <string.h>

#define GBMC_MAX_BACKENDS 16

static const gbmc_capture_backend_t *s_backends[GBMC_MAX_BACKENDS];
static size_t s_backend_count;
static int s_builtins_registered;

static int backend_name_matches(const gbmc_capture_backend_t *backend,
                                const char *name) {
  return backend && backend->name && name && strcmp(backend->name, name) == 0;
}

int gbmc_register_backend(const gbmc_capture_backend_t *backend) {
  if (!backend || !backend->name || !backend->open || !backend->close) {
    return GBMC_ERR_INVALID;
  }
  if (gbmc_find_backend(backend->name)) return GBMC_OK;
  if (s_backend_count >= GBMC_MAX_BACKENDS) return GBMC_ERR_NO_MEMORY;

  size_t pos = s_backend_count;
  while (pos > 0 && s_backends[pos - 1]->priority < backend->priority) {
    s_backends[pos] = s_backends[pos - 1];
    pos--;
  }
  s_backends[pos] = backend;
  s_backend_count++;
  return GBMC_OK;
}

int gbmc_unregister_backend(const char *backend_name) {
  if (!backend_name) return GBMC_ERR_INVALID;
  for (size_t i = 0; i < s_backend_count; i++) {
    if (backend_name_matches(s_backends[i], backend_name)) {
      for (size_t j = i + 1; j < s_backend_count; j++) {
        s_backends[j - 1] = s_backends[j];
      }
      s_backends[--s_backend_count] = NULL;
      return GBMC_OK;
    }
  }
  return GBMC_ERR_NOT_FOUND;
}

size_t gbmc_backend_count(void) {
  return s_backend_count;
}

const gbmc_capture_backend_t *gbmc_backend_at(size_t index) {
  return index < s_backend_count ? s_backends[index] : NULL;
}

const gbmc_capture_backend_t *gbmc_find_backend(const char *backend_name) {
  if (!backend_name || !backend_name[0]) return NULL;
  for (size_t i = 0; i < s_backend_count; i++) {
    if (backend_name_matches(s_backends[i], backend_name)) return s_backends[i];
  }
  return NULL;
}

int gbmc_register_builtin_backends(void) {
  if (s_builtins_registered) return GBMC_OK;
  s_builtins_registered = 1;
  (void) gbmc_register_backend(gbmc_null_backend());
#if defined(GBMEDIA_ENABLE_DV500_BACKEND) && GBMEDIA_ENABLE_DV500_BACKEND
  (void) gbmc_register_backend(gbmc_dv500_backend());
#endif
#if defined(GBMEDIA_ENABLE_FFMPEG_BACKEND) && GBMEDIA_ENABLE_FFMPEG_BACKEND
  (void) gbmc_register_backend(gbmc_ffmpeg_backend());
#endif
  return GBMC_OK;
}

int gbmc_list_devices(const char *backend_name,
                      gbmc_media_type_t media_type,
                      gbmc_device_info_t *devices,
                      size_t max_devices,
                      size_t *count_out) {
  if (!count_out) return GBMC_ERR_INVALID;
  *count_out = 0;
  gbmc_register_builtin_backends();

  if (backend_name && backend_name[0]) {
    const gbmc_capture_backend_t *backend = gbmc_find_backend(backend_name);
    if (!backend) return GBMC_ERR_NOT_FOUND;
    if (!backend->list_devices) return GBMC_ERR_UNSUPPORTED;
    return backend->list_devices(media_type, devices, max_devices, count_out);
  }

  size_t total = 0;
  for (size_t i = 0; i < s_backend_count; i++) {
    const gbmc_capture_backend_t *backend = s_backends[i];
    if (!backend->list_devices) continue;
    size_t written = 0;
    size_t room = total < max_devices ? max_devices - total : 0;
    int rc = backend->list_devices(media_type,
                                   room ? devices + total : NULL,
                                   room,
                                   &written);
    if (rc != GBMC_OK && rc != GBMC_ERR_UNSUPPORTED) return rc;
    total += written;
  }
  *count_out = total;
  return GBMC_OK;
}

int gbmc_open(const gbmc_capture_config_t *config,
              const gbmc_capture_callbacks_t *callbacks,
              gbmc_capture_t **capture_out) {
  if (!config || !capture_out) return GBMC_ERR_INVALID;
  *capture_out = NULL;
  gbmc_register_builtin_backends();

  if (config->backend_name && config->backend_name[0]) {
    const gbmc_capture_backend_t *backend = gbmc_find_backend(config->backend_name);
    if (!backend) return GBMC_ERR_NOT_FOUND;
    return backend->open(config, callbacks, capture_out);
  }

  for (size_t i = 0; i < s_backend_count; i++) {
    const gbmc_capture_backend_t *backend = s_backends[i];
    if (backend->probe && backend->probe() != GBMC_OK) continue;
    int rc = backend->open(config, callbacks, capture_out);
    if (rc == GBMC_OK) return GBMC_OK;
  }
  return GBMC_ERR_NOT_FOUND;
}

void gbmc_close(gbmc_capture_t **capture) {
  if (!capture || !*capture) return;
  gbmc_capture_t *local = *capture;
  *capture = NULL;
  if (local->backend && local->backend->close) local->backend->close(local);
}

int gbmc_start(gbmc_capture_t *capture) {
  if (!capture || !capture->backend) return GBMC_ERR_INVALID;
  return capture->backend->start ? capture->backend->start(capture) : GBMC_OK;
}

int gbmc_stop(gbmc_capture_t *capture) {
  if (!capture || !capture->backend) return GBMC_ERR_INVALID;
  return capture->backend->stop ? capture->backend->stop(capture) : GBMC_OK;
}

const char *gbmc_result_name(int result) {
  switch (result) {
    case GBMC_OK: return "OK";
    case GBMC_ERR_INVALID: return "INVALID";
    case GBMC_ERR_NOT_FOUND: return "NOT_FOUND";
    case GBMC_ERR_NO_MEMORY: return "NO_MEMORY";
    case GBMC_ERR_BACKEND: return "BACKEND";
    case GBMC_ERR_UNSUPPORTED: return "UNSUPPORTED";
    case GBMC_ERR_BUSY: return "BUSY";
    case GBMC_ERR_PERMISSION: return "PERMISSION";
    default: return "UNKNOWN";
  }
}
