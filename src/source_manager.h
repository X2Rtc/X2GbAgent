#ifndef SOURCE_MANAGER_H
#define SOURCE_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifndef GB_PREVIEW_FPS
#define GB_PREVIEW_FPS 5
#endif

#define GB_PREVIEW_FPS_CLAMPED ((GB_PREVIEW_FPS) > 0 ? (GB_PREVIEW_FPS) : 5)
#define GB_PREVIEW_INTERVAL_MS (1000 / GB_PREVIEW_FPS_CLAMPED)
#define GB_PREVIEW_INTERVAL_US (1000000 / GB_PREVIEW_FPS_CLAMPED)

int gb_source_manager_init(void);
void gb_source_manager_cleanup(void);
void gb_source_key(char *out, size_t out_size, const char *mode, const char *id);

typedef struct {
  char source_mode[16];
  char backend_name[32];
  char video_device[256];
  char audio_device[256];
  char media_file[256];
  char resolution[32];
  int bitrate_kbps;
  int fps;
  int loop;
  char file_pacing[16];
} gb_source_runtime_config_t;

int gb_source_acquire(const char *key, const char *owner);
int gb_source_acquire_runtime(const char *key,
                              const char *owner,
                              const gb_source_runtime_config_t *config);
int gb_source_release(const char *key, const char *owner);
int gb_source_ref_count(const char *key);
int gb_source_agent_ref_count(const char *key);
int gb_source_claim_producer(const char *key);
int gb_source_release_producer(const char *key);
int gb_source_producer_count(const char *key);
int gb_source_snapshot_jpeg(const char *key, uint8_t **jpeg_out, size_t *jpeg_size_out);
int gb_source_publish_jpeg(const char *key, const uint8_t *jpeg, size_t jpeg_size);
int gb_source_publish_raw(const char *key,
                          const uint8_t *data,
                          size_t size,
                          int width,
                          int height,
                          const char *format);
int gb_source_publish_preview_jpeg_from_raw(const char *key,
                                            const uint8_t *data,
                                            size_t size,
                                            int width,
                                            int height,
                                            const char *format,
                                            int max_width,
                                            int max_height);

int gb_source_snapshot_raw(const char *key,
                           uint8_t **data_out,
                           size_t *size_out,
                           int *width_out,
                           int *height_out,
                           char *format_out,
                           size_t format_out_size);
int gb_source_snapshot_raw_meta(const char *key,
                                uint8_t **data_out,
                                size_t *size_out,
                                int *width_out,
                                int *height_out,
                                char *format_out,
                                size_t format_out_size,
                                unsigned long *generation_out,
                                time_t *updated_at_out);
size_t gb_source_append_status_json(char *buf, size_t cap, size_t len);

#endif
