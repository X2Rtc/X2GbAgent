#ifndef GB_MEDIA_CODEC_H
#define GB_MEDIA_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#define GBMCD_MAX_NAME 64
#define GBMCD_MAX_TEXT 256

typedef struct gbmcd_codec gbmcd_codec_t;

typedef enum {
  GBMCD_OK = 0,
  GBMCD_ERR_INVALID = -1,
  GBMCD_ERR_NOT_FOUND = -2,
  GBMCD_ERR_NO_MEMORY = -3,
  GBMCD_ERR_BACKEND = -4,
  GBMCD_ERR_UNSUPPORTED = -5,
  GBMCD_ERR_AGAIN = -6,
  GBMCD_ERR_EOF = -7,
  GBMCD_ERR_BUSY = -8,
  GBMCD_ERR_PERMISSION = -9
} gbmcd_result_t;

typedef enum {
  GBMCD_MEDIA_VIDEO = 1,
  GBMCD_MEDIA_AUDIO = 2
} gbmcd_media_type_t;

typedef enum {
  GBMCD_CODEC_ENCODER = 1,
  GBMCD_CODEC_DECODER = 2
} gbmcd_codec_role_t;

typedef enum {
  GBMCD_FRAME_RAW = 1,
  GBMCD_FRAME_PACKET = 2
} gbmcd_frame_type_t;

enum {
  GBMCD_FRAME_FLAG_OWNED_DATA = 1 << 0
};

typedef struct {
  char backend[GBMCD_MAX_NAME];
  char name[GBMCD_MAX_NAME];
  char description[GBMCD_MAX_TEXT];
  gbmcd_media_type_t media_type;
  gbmcd_codec_role_t role;
  int hardware;
} gbmcd_codec_info_t;

typedef struct {
  const char *backend_name;
  const char *codec_name;
  gbmcd_media_type_t media_type;
  gbmcd_codec_role_t role;
  int width;
  int height;
  int fps_num;
  int fps_den;
  int sample_rate;
  int channels;
  int bitrate_kbps;
  int gop;
  const char *profile;
  const char *level;
  int low_latency;
  int prefer_hardware;
  const char *pixel_format;
  const char *sample_format;
} gbmcd_codec_config_t;

typedef struct {
  gbmcd_frame_type_t type;
  int64_t pts_us;
  int64_t duration_us;
  const uint8_t *data;
  size_t size;
  int key_frame;
  int width;
  int height;
  int sample_rate;
  int channels;
  const char *format;
  unsigned flags;
} gbmcd_frame_t;

typedef struct {
  int target_bitrate_kbps;
  int min_bitrate_kbps;
  int max_bitrate_kbps;
  int congestion_score;
  int encoder_queue_ms;
  int dropped_frames;
} gbmcd_rate_control_feedback_t;

typedef int (*gbmcd_frame_cb)(void *user_data, const gbmcd_frame_t *frame);
typedef int (*gbmcd_rate_feedback_cb)(void *user_data,
                                      gbmcd_rate_control_feedback_t *feedback);

typedef struct {
  void *user_data;
  gbmcd_frame_cb on_frame;
  gbmcd_rate_feedback_cb on_rate_feedback;
} gbmcd_codec_callbacks_t;

typedef struct gbmcd_codec_backend {
  const char *name;
  const char *description;
  int priority;
  int (*probe)(void);
  int (*list_codecs)(gbmcd_media_type_t media_type,
                     gbmcd_codec_role_t role,
                     gbmcd_codec_info_t *codecs,
                     size_t max_codecs,
                     size_t *count_out);
  int (*open)(const gbmcd_codec_config_t *config,
              const gbmcd_codec_callbacks_t *callbacks,
              gbmcd_codec_t **codec_out);
  void (*close)(gbmcd_codec_t *codec);
  int (*send_frame)(gbmcd_codec_t *codec, const gbmcd_frame_t *frame);
  int (*receive_frame)(gbmcd_codec_t *codec, gbmcd_frame_t *frame_out);
  int (*request_keyframe)(gbmcd_codec_t *codec);
  int (*set_bitrate)(gbmcd_codec_t *codec, int bitrate_kbps);
  int (*poll_feedback)(gbmcd_codec_t *codec,
                       gbmcd_rate_control_feedback_t *feedback_out);
} gbmcd_codec_backend_t;

int gbmcd_register_backend(const gbmcd_codec_backend_t *backend);
int gbmcd_unregister_backend(const char *backend_name);
size_t gbmcd_backend_count(void);
const gbmcd_codec_backend_t *gbmcd_backend_at(size_t index);
const gbmcd_codec_backend_t *gbmcd_find_backend(const char *backend_name);

int gbmcd_register_builtin_backends(void);
int gbmcd_list_codecs(const char *backend_name,
                      gbmcd_media_type_t media_type,
                      gbmcd_codec_role_t role,
                      gbmcd_codec_info_t *codecs,
                      size_t max_codecs,
                      size_t *count_out);
int gbmcd_open(const gbmcd_codec_config_t *config,
               const gbmcd_codec_callbacks_t *callbacks,
               gbmcd_codec_t **codec_out);
void gbmcd_close(gbmcd_codec_t **codec);
int gbmcd_send_frame(gbmcd_codec_t *codec, const gbmcd_frame_t *frame);
int gbmcd_receive_frame(gbmcd_codec_t *codec, gbmcd_frame_t *frame_out);
void gbmcd_frame_release(gbmcd_frame_t *frame);
int gbmcd_request_keyframe(gbmcd_codec_t *codec);
int gbmcd_set_bitrate(gbmcd_codec_t *codec, int bitrate_kbps);
int gbmcd_adapt_bitrate(gbmcd_codec_t *codec,
                        const gbmcd_rate_control_feedback_t *feedback,
                        int *applied_bitrate_kbps);
int gbmcd_poll_feedback(gbmcd_codec_t *codec,
                        gbmcd_rate_control_feedback_t *feedback_out);
const char *gbmcd_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif
