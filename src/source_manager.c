#include "source_manager.h"

#include "gb_platform.h"
#include "gb_media_capture.h"
#include "gb_media_file_source.h"
#include "preview_jpeg.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GB_SOURCE_MAX_ENTRIES 16

typedef struct {
  char key[512];
  int ref_count;
  int agent_refs;
  int preview_refs;
  int producer_refs;
  unsigned long generation;
  unsigned long raw_generation;
  time_t updated_at;
  uint8_t *jpeg;
  size_t jpeg_size;
  uint8_t *raw;
  size_t raw_size;
  int raw_width;
  int raw_height;
  char raw_format[32];
  gb_source_runtime_config_t config;
  gbmc_capture_t *capture;
  gbmf_file_source_t *file_source;
  gb_thread_t file_thread;
  int file_thread_running;
  int open_state;
} gb_source_entry_t;

static gb_mutex_t s_mu;
static int s_init;
static gb_source_entry_t s_entries[GB_SOURCE_MAX_ENTRIES];

static void runtime_close_entry_locked(int idx);

int gb_source_manager_init(void) {
  if (s_init) return 0;
  gb_mutex_init(&s_mu);
  memset(s_entries, 0, sizeof(s_entries));
  s_init = 1;
  return 0;
}

void gb_source_manager_cleanup(void) {
  if (!s_init) return;
  gb_mutex_lock(&s_mu);
  for (int i = 0; i < GB_SOURCE_MAX_ENTRIES; i++) {
    runtime_close_entry_locked(i);
    free(s_entries[i].jpeg);
    free(s_entries[i].raw);
  }
  memset(s_entries, 0, sizeof(s_entries));
  gb_mutex_unlock(&s_mu);
  gb_mutex_destroy(&s_mu);
  s_init = 0;
}

static int source_manager_ready(void) {
  return s_init;
}

static void copy_text(char *dst, size_t size, const char *src) {
  size_t i;
  if (dst == NULL || size == 0) return;
  if (src == NULL) src = "";
  for (i = 0; i + 1 < size && src[i] != '\0'; i++) dst[i] = src[i];
  dst[i] = '\0';
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

static const char *default_capture_backend(const gb_source_runtime_config_t *config) {
#if GBMEDIA_WITH_DV500
  if (config != NULL && strcmp(config->source_mode, "device") == 0 &&
      (config->video_device[0] == '\0' ||
       strncmp(config->video_device, "dv500://", 8) == 0)) {
    return "dv500";
  }
#else
  (void) config;
#endif
  return "ffmpeg";
}

void gb_source_key(char *out, size_t out_size, const char *mode, const char *id) {
  if (!out || out_size == 0) return;
  snprintf(out, out_size, "%s:%s", mode && mode[0] ? mode : "device", id && id[0] ? id : "");
}

static int find_entry_locked(const char *key) {
  for (int i = 0; i < GB_SOURCE_MAX_ENTRIES; i++) {
    if (s_entries[i].key[0] && strcmp(s_entries[i].key, key) == 0) return i;
  }
  return -1;
}

static int find_or_create_entry_locked(const char *key) {
  int idx = find_entry_locked(key);
  if (idx >= 0) return idx;
  for (int i = 0; i < GB_SOURCE_MAX_ENTRIES; i++) {
    if (!s_entries[i].key[0]) {
      snprintf(s_entries[i].key, sizeof(s_entries[i].key), "%s", key);
      s_entries[i].updated_at = time(NULL);
      return i;
    }
  }
  return -1;
}

static int on_runtime_capture_sample(void *user_data, const gbmc_sample_t *sample) {
  const char *key = (const char *) user_data;
  if (key == NULL || sample == NULL) return -1;
  if (sample->type == GBMC_SAMPLE_VIDEO_RAW) {
    (void) gb_source_publish_raw(key,
                                 sample->data,
                                 sample->size,
                                 sample->width,
                                 sample->height,
                                 sample->format);
    (void) gb_source_publish_preview_jpeg_from_raw(key,
                                                   sample->data,
                                                   sample->size,
                                                   sample->width,
                                                   sample->height,
                                                   sample->format,
                                                   960,
                                                   540);
  }
  return 0;
}

static int on_runtime_file_frame(void *user_data, const gbmf_frame_t *frame) {
  const char *key = (const char *) user_data;
  if (key == NULL || frame == NULL) return -1;
  if (frame->type == GBMF_FRAME_VIDEO_RAW) {
    (void) gb_source_publish_raw(key,
                                 frame->data,
                                 frame->size,
                                 frame->width,
                                 frame->height,
                                 frame->format);
    (void) gb_source_publish_preview_jpeg_from_raw(key,
                                                   frame->data,
                                                   frame->size,
                                                   frame->width,
                                                   frame->height,
                                                   frame->format,
                                                   960,
                                                   540);
  }
  return 0;
}

static void *runtime_file_thread(void *arg) {
  gb_source_entry_t *entry = (gb_source_entry_t *) arg;
  if (entry == NULL) return NULL;
  while (entry->file_thread_running) {
    int rc = entry->file_source ? gbmf_read(entry->file_source) : GBMF_ERR_INVALID;
    if (rc != GBMF_OK) gb_sleep_ms(rc == GBMF_ERR_EOF ? 20 : 5);
  }
  return NULL;
}

static int runtime_open_locked_index(int idx, const gb_source_runtime_config_t *config) {
  gb_source_entry_t *entry;
  int width = 0;
  int height = 0;
  int fps = config != NULL ? config->fps : 0;
  if (idx < 0 || idx >= GB_SOURCE_MAX_ENTRIES || config == NULL) return -1;
  entry = &s_entries[idx];
  if (entry->open_state == 2) return 0;
  if (entry->open_state == 1) return 0;
  entry->config = *config;
  (void) parse_resolution(entry->config.resolution, &width, &height);
  entry->open_state = 1;
  gb_mutex_unlock(&s_mu);

  int rc = -1;
  if (strcmp(entry->config.source_mode, "file") == 0) {
    gbmf_file_source_config_t cfg;
    gbmf_file_source_callbacks_t cb;
    memset(&cfg, 0, sizeof(cfg));
    memset(&cb, 0, sizeof(cb));
    cfg.path = entry->config.media_file;
    cfg.loop = entry->config.loop;
    cfg.realtime = strcmp(entry->config.file_pacing, "fast") != 0;
    cfg.width = width;
    cfg.height = height;
    cfg.fps_num = fps > 0 ? fps : 25;
    cfg.fps_den = 1;
    cfg.video_pixel_format = "yuv420p";
    cb.user_data = entry->key;
    cb.on_frame = on_runtime_file_frame;
    rc = gbmf_open(&cfg, &cb, &entry->file_source);
    if (rc == GBMF_OK) {
      entry->file_thread_running = 1;
      if (gb_thread_create(&entry->file_thread, runtime_file_thread, entry) != 0) {
        entry->file_thread_running = 0;
        gbmf_close(&entry->file_source);
        rc = -1;
      }
    }
  } else if (strcmp(entry->config.source_mode, "none") != 0) {
    gbmc_capture_config_t cfg;
    gbmc_capture_callbacks_t cb;
    memset(&cfg, 0, sizeof(cfg));
    memset(&cb, 0, sizeof(cb));
    cfg.backend_name = entry->config.backend_name[0]
                         ? entry->config.backend_name
                         : default_capture_backend(&entry->config);
    cfg.video_device_id = entry->config.video_device;
    cfg.audio_device_id = entry->config.audio_device;
    cfg.width = width;
    cfg.height = height;
    cfg.fps_num = fps;
    cfg.fps_den = 1;
    cfg.sample_rate = 8000;
    cfg.channels = 1;
    cb.user_data = entry->key;
    cb.on_sample = on_runtime_capture_sample;
    rc = gbmc_open(&cfg, &cb, &entry->capture);
    if (rc == GBMC_OK) rc = gbmc_start(entry->capture);
    if (rc != GBMC_OK && entry->capture) gbmc_close(&entry->capture);
  } else {
    rc = 0;
  }

  gb_mutex_lock(&s_mu);
  entry->open_state = rc == 0 ? 2 : 0;
  if (entry->ref_count == 0) {
    runtime_close_entry_locked(idx);
    free(entry->jpeg);
    free(entry->raw);
    memset(entry, 0, sizeof(*entry));
    return -1;
  }
  return rc == 0 ? 0 : -1;
}

static void runtime_close_entry_locked(int idx) {
  gb_source_entry_t *entry;
  gb_thread_t file_thread;
  int join_file = 0;
  if (idx < 0 || idx >= GB_SOURCE_MAX_ENTRIES) return;
  entry = &s_entries[idx];
  if (entry->file_thread_running) {
    entry->file_thread_running = 0;
    file_thread = entry->file_thread;
    join_file = 1;
  }
  gbmc_capture_t *capture = entry->capture;
  gbmf_file_source_t *file_source = entry->file_source;
  entry->capture = NULL;
  entry->file_source = NULL;
  entry->open_state = 0;
  gb_mutex_unlock(&s_mu);
  if (capture) {
    (void) gbmc_stop(capture);
    gbmc_close(&capture);
  }
  if (join_file) gb_thread_join(file_thread);
  if (file_source) gbmf_close(&file_source);
  gb_mutex_lock(&s_mu);
}

int gb_source_acquire(const char *key, const char *owner) {
  int refs = -1;
  if (!key || !key[0]) return -1;
  if (!source_manager_ready()) return -1;
  gb_mutex_lock(&s_mu);
  int idx = find_or_create_entry_locked(key);
  if (idx >= 0) {
    s_entries[idx].ref_count++;
    if (owner && strcmp(owner, "agent") == 0) s_entries[idx].agent_refs++;
    if (owner && strcmp(owner, "preview") == 0) s_entries[idx].preview_refs++;
    s_entries[idx].updated_at = time(NULL);
    refs = s_entries[idx].ref_count;
  }
  gb_mutex_unlock(&s_mu);
  return refs;
}

int gb_source_acquire_runtime(const char *key,
                              const char *owner,
                              const gb_source_runtime_config_t *config) {
  int refs = -1;
  int idx;
  int first_ref;
  if (!key || !key[0]) return -1;
  if (!source_manager_ready()) return -1;
  gb_mutex_lock(&s_mu);
  idx = find_or_create_entry_locked(key);
  if (idx >= 0) {
    first_ref = s_entries[idx].ref_count == 0;
    s_entries[idx].ref_count++;
    if (owner && strcmp(owner, "agent") == 0) s_entries[idx].agent_refs++;
    if (owner && strcmp(owner, "preview") == 0) s_entries[idx].preview_refs++;
    s_entries[idx].updated_at = time(NULL);
    refs = s_entries[idx].ref_count;
    if (first_ref && config != NULL && runtime_open_locked_index(idx, config) != 0) {
      if (s_entries[idx].ref_count > 0) s_entries[idx].ref_count--;
      if (owner && strcmp(owner, "agent") == 0 && s_entries[idx].agent_refs > 0) s_entries[idx].agent_refs--;
      if (owner && strcmp(owner, "preview") == 0 && s_entries[idx].preview_refs > 0) s_entries[idx].preview_refs--;
      refs = -1;
    }
  }
  gb_mutex_unlock(&s_mu);
  return refs;
}

int gb_source_release(const char *key, const char *owner) {
  int refs = -1;
  if (!key || !key[0]) return -1;
  if (!source_manager_ready()) return -1;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0) {
    if (s_entries[idx].ref_count > 0) s_entries[idx].ref_count--;
    if (owner && strcmp(owner, "agent") == 0 && s_entries[idx].agent_refs > 0) s_entries[idx].agent_refs--;
    if (owner && strcmp(owner, "preview") == 0 && s_entries[idx].preview_refs > 0) s_entries[idx].preview_refs--;
    refs = s_entries[idx].ref_count;
    s_entries[idx].updated_at = time(NULL);
    if (s_entries[idx].ref_count == 0) {
      if (s_entries[idx].open_state == 1) {
        refs = 0;
      } else {
        runtime_close_entry_locked(idx);
        free(s_entries[idx].jpeg);
        free(s_entries[idx].raw);
        memset(&s_entries[idx], 0, sizeof(s_entries[idx]));
      }
    }
  }
  gb_mutex_unlock(&s_mu);
  return refs;
}

int gb_source_ref_count(const char *key) {
  int refs = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0) refs = s_entries[idx].ref_count;
  gb_mutex_unlock(&s_mu);
  return refs;
}

int gb_source_agent_ref_count(const char *key) {
  int refs = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0) refs = s_entries[idx].agent_refs;
  gb_mutex_unlock(&s_mu);
  return refs;
}

int gb_source_claim_producer(const char *key) {
  int claimed = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_or_create_entry_locked(key);
  if (idx >= 0 && s_entries[idx].producer_refs == 0) {
    s_entries[idx].producer_refs = 1;
    s_entries[idx].updated_at = time(NULL);
    claimed = 1;
  }
  gb_mutex_unlock(&s_mu);
  return claimed;
}

int gb_source_release_producer(const char *key) {
  int producers = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0) {
    if (s_entries[idx].producer_refs > 0) s_entries[idx].producer_refs--;
    producers = s_entries[idx].producer_refs;
    s_entries[idx].updated_at = time(NULL);
  }
  gb_mutex_unlock(&s_mu);
  return producers;
}

int gb_source_producer_count(const char *key) {
  int producers = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0) producers = s_entries[idx].producer_refs;
  gb_mutex_unlock(&s_mu);
  return producers;
}

int gb_source_snapshot_jpeg(const char *key, uint8_t **jpeg_out, size_t *jpeg_size_out) {
  int ok = 0;
  if (!jpeg_out || !jpeg_size_out) return 0;
  *jpeg_out = NULL;
  *jpeg_size_out = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0 && s_entries[idx].jpeg && s_entries[idx].jpeg_size > 0) {
    uint8_t *copy = (uint8_t *) malloc(s_entries[idx].jpeg_size);
    if (copy) {
      memcpy(copy, s_entries[idx].jpeg, s_entries[idx].jpeg_size);
      *jpeg_out = copy;
      *jpeg_size_out = s_entries[idx].jpeg_size;
      ok = 1;
    }
  }
  gb_mutex_unlock(&s_mu);
  return ok;
}

int gb_source_publish_jpeg(const char *key, const uint8_t *jpeg, size_t jpeg_size) {
  int rc = -1;
  if (!key || !key[0] || !jpeg || jpeg_size == 0) return -1;
  if (!source_manager_ready()) return -1;
  gb_mutex_lock(&s_mu);
  int idx = find_or_create_entry_locked(key);
  if (idx >= 0) {
    uint8_t *copy = (uint8_t *) malloc(jpeg_size);
    if (copy) {
      memcpy(copy, jpeg, jpeg_size);
      free(s_entries[idx].jpeg);
      s_entries[idx].jpeg = copy;
      s_entries[idx].jpeg_size = jpeg_size;
      s_entries[idx].generation++;
      s_entries[idx].updated_at = time(NULL);
      rc = 0;
    }
  }
  gb_mutex_unlock(&s_mu);
  return rc;
}

int gb_source_publish_raw(const char *key,
                          const uint8_t *data,
                          size_t size,
                          int width,
                          int height,
                          const char *format) {
  if (gb_source_ref_count(key) <= 0) return -1;
  if (!source_manager_ready()) return -1;
  gb_mutex_lock(&s_mu);
  int idx = find_or_create_entry_locked(key);
  if (idx >= 0 && data && size > 0) {
    uint8_t *copy = (uint8_t *) malloc(size);
    if (copy) {
      memcpy(copy, data, size);
      free(s_entries[idx].raw);
      s_entries[idx].raw = copy;
      s_entries[idx].raw_size = size;
      s_entries[idx].raw_width = width;
      s_entries[idx].raw_height = height;
      snprintf(s_entries[idx].raw_format, sizeof(s_entries[idx].raw_format), "%s", format && format[0] ? format : "yuv420p");
      s_entries[idx].generation++;
      s_entries[idx].raw_generation++;
      s_entries[idx].updated_at = time(NULL);
    }
  }
  gb_mutex_unlock(&s_mu);
  return idx >= 0 ? 0 : -1;
}

int gb_source_publish_preview_jpeg_from_raw(const char *key,
                                            const uint8_t *data,
                                            size_t size,
                                            int width,
                                            int height,
                                            const char *format,
                                            int max_width,
                                            int max_height) {
  uint8_t *jpeg = NULL;
  size_t jpeg_size = 0;
  int rc;
  if (!data || size == 0) return -1;
  if (!key || !key[0]) return -1;
  if (!source_manager_ready()) return -1;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  int preview_refs = idx >= 0 ? s_entries[idx].preview_refs : 0;
  gb_mutex_unlock(&s_mu);
  if (preview_refs <= 0) return -1;
  rc = gb_preview_jpeg_from_raw(data, size, width, height, format, max_width, max_height, &jpeg, &jpeg_size);
  if (rc == 0 && jpeg_size > 0) rc = gb_source_publish_jpeg(key, jpeg, jpeg_size);
  gb_preview_jpeg_free(&jpeg);
  return rc;
}

int gb_source_snapshot_raw(const char *key,
                           uint8_t **data_out,
                           size_t *size_out,
                           int *width_out,
                           int *height_out,
                           char *format_out,
                           size_t format_out_size) {
  return gb_source_snapshot_raw_meta(key,
                                     data_out,
                                     size_out,
                                     width_out,
                                     height_out,
                                     format_out,
                                     format_out_size,
                                     NULL,
                                     NULL);
}

int gb_source_snapshot_raw_meta(const char *key,
                                uint8_t **data_out,
                                size_t *size_out,
                                int *width_out,
                                int *height_out,
                                char *format_out,
                                size_t format_out_size,
                                unsigned long *generation_out,
                                time_t *updated_at_out) {
  int ok = 0;
  if (!data_out || !size_out) return 0;
  *data_out = NULL;
  *size_out = 0;
  if (width_out) *width_out = 0;
  if (height_out) *height_out = 0;
  if (format_out && format_out_size > 0) format_out[0] = '\0';
  if (generation_out) *generation_out = 0;
  if (updated_at_out) *updated_at_out = 0;
  if (!key || !key[0]) return 0;
  if (!source_manager_ready()) return 0;
  gb_mutex_lock(&s_mu);
  int idx = find_entry_locked(key);
  if (idx >= 0 && s_entries[idx].raw && s_entries[idx].raw_size > 0) {
    uint8_t *copy = (uint8_t *) malloc(s_entries[idx].raw_size);
    if (copy) {
      memcpy(copy, s_entries[idx].raw, s_entries[idx].raw_size);
      *data_out = copy;
      *size_out = s_entries[idx].raw_size;
      if (width_out) *width_out = s_entries[idx].raw_width;
      if (height_out) *height_out = s_entries[idx].raw_height;
      if (format_out && format_out_size > 0) {
        snprintf(format_out, format_out_size, "%s", s_entries[idx].raw_format);
      }
      if (generation_out) *generation_out = s_entries[idx].raw_generation;
      if (updated_at_out) *updated_at_out = s_entries[idx].updated_at;
      ok = 1;
    }
  }
  gb_mutex_unlock(&s_mu);
  return ok;
}

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

size_t gb_source_append_status_json(char *buf, size_t cap, size_t len) {
  int emitted = 0;
  len = appendf(buf, cap, len, "[");
  if (!source_manager_ready()) return appendf(buf, cap, len, "]");
  gb_mutex_lock(&s_mu);
  for (int i = 0; i < GB_SOURCE_MAX_ENTRIES; i++) {
    if (!s_entries[i].key[0]) continue;
    len = appendf(buf, cap, len, "%s{\"key\":", emitted++ ? "," : "");
    len = append_json_string(buf, cap, len, s_entries[i].key);
    len = appendf(buf, cap, len,
                  ",\"ref_count\":%d,\"agent_refs\":%d,\"preview_refs\":%d,\"producer_refs\":%d,\"open_state\":%d,\"generation\":%lu,\"jpeg_size\":%llu,\"raw_size\":%llu,\"updated_at\":%lld}",
                  s_entries[i].ref_count,
                  s_entries[i].agent_refs,
                  s_entries[i].preview_refs,
                  s_entries[i].producer_refs,
                  s_entries[i].open_state,
                  s_entries[i].generation,
                  (unsigned long long) s_entries[i].jpeg_size,
                  (unsigned long long) s_entries[i].raw_size,
                  (long long) s_entries[i].updated_at);
  }
  gb_mutex_unlock(&s_mu);
  return appendf(buf, cap, len, "]");
}
