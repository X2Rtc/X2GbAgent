#ifndef GB_MEDIA_CAPTURE_H
#define GB_MEDIA_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define GBMC_MAX_NAME 64
#define GBMC_MAX_TEXT 256

typedef struct gbmc_capture gbmc_capture_t;

typedef enum {
  GBMC_OK = 0,
  GBMC_ERR_INVALID = -1,
  GBMC_ERR_NOT_FOUND = -2,
  GBMC_ERR_NO_MEMORY = -3,
  GBMC_ERR_BACKEND = -4,
  GBMC_ERR_UNSUPPORTED = -5,
  GBMC_ERR_BUSY = -6,
  GBMC_ERR_PERMISSION = -7
} gbmc_result_t;

typedef enum {
  GBMC_MEDIA_VIDEO = 1,
  GBMC_MEDIA_AUDIO = 2
} gbmc_media_type_t;

typedef enum {
  GBMC_SAMPLE_UNKNOWN = 0,
  GBMC_SAMPLE_VIDEO_RAW,
  GBMC_SAMPLE_VIDEO_PACKET,
  GBMC_SAMPLE_AUDIO_RAW,
  GBMC_SAMPLE_AUDIO_PACKET
} gbmc_sample_type_t;

enum {
  GBMC_SAMPLE_FLAG_TRANSIENT_DATA = 1 << 0
};

typedef struct {
  char backend[GBMC_MAX_NAME];
  char id[GBMC_MAX_TEXT];
  char name[GBMC_MAX_TEXT];
  gbmc_media_type_t media_type;
} gbmc_device_info_t;

typedef struct {
  const char *backend_name;
  const char *video_device_id;
  const char *audio_device_id;
  int width;
  int height;
  int fps_num;
  int fps_den;
  int sample_rate;
  int channels;
} gbmc_capture_config_t;

typedef struct {
  gbmc_sample_type_t type;
  int64_t pts_us;
  int64_t duration_us;
  const uint8_t *data;
  size_t size;
  int width;
  int height;
  int sample_rate;
  int channels;
  const char *format;
  unsigned flags;
} gbmc_sample_t;

typedef int (*gbmc_sample_cb)(void *user_data, const gbmc_sample_t *sample);

typedef struct {
  void *user_data;
  gbmc_sample_cb on_sample;
} gbmc_capture_callbacks_t;

typedef struct gbmc_capture_backend {
  const char *name;
  const char *description;
  int priority;
  int (*probe)(void);
  int (*list_devices)(gbmc_media_type_t media_type,
                      gbmc_device_info_t *devices,
                      size_t max_devices,
                      size_t *count_out);
  int (*open)(const gbmc_capture_config_t *config,
              const gbmc_capture_callbacks_t *callbacks,
              gbmc_capture_t **capture_out);
  void (*close)(gbmc_capture_t *capture);
  int (*start)(gbmc_capture_t *capture);
  int (*stop)(gbmc_capture_t *capture);
} gbmc_capture_backend_t;

int gbmc_register_backend(const gbmc_capture_backend_t *backend);
int gbmc_unregister_backend(const char *backend_name);
size_t gbmc_backend_count(void);
const gbmc_capture_backend_t *gbmc_backend_at(size_t index);
const gbmc_capture_backend_t *gbmc_find_backend(const char *backend_name);

int gbmc_register_builtin_backends(void);
int gbmc_list_devices(const char *backend_name,
                      gbmc_media_type_t media_type,
                      gbmc_device_info_t *devices,
                      size_t max_devices,
                      size_t *count_out);
int gbmc_open(const gbmc_capture_config_t *config,
              const gbmc_capture_callbacks_t *callbacks,
              gbmc_capture_t **capture_out);
void gbmc_close(gbmc_capture_t **capture);
int gbmc_start(gbmc_capture_t *capture);
int gbmc_stop(gbmc_capture_t *capture);
const char *gbmc_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif
