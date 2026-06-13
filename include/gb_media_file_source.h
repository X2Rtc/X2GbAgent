#ifndef GB_MEDIA_FILE_SOURCE_H
#define GB_MEDIA_FILE_SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct gbmf_file_source gbmf_file_source_t;

typedef enum {
  GBMF_OK = 0,
  GBMF_ERR_INVALID = -1,
  GBMF_ERR_NOT_FOUND = -2,
  GBMF_ERR_NO_MEMORY = -3,
  GBMF_ERR_BACKEND = -4,
  GBMF_ERR_UNSUPPORTED = -5,
  GBMF_ERR_EOF = -6
} gbmf_result_t;

typedef enum {
  GBMF_FRAME_VIDEO_RAW = 1,
  GBMF_FRAME_AUDIO_RAW = 2
} gbmf_frame_type_t;

enum {
  GBMF_FRAME_FLAG_TRANSIENT_DATA = 1 << 0
};

typedef struct {
  int has_video;
  int has_audio;
  int width;
  int height;
  int fps_num;
  int fps_den;
  int sample_rate;
  int channels;
  char video_codec[32];
  char audio_codec[32];
} gbmf_file_info_t;

typedef struct {
  gbmf_frame_type_t type;
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
} gbmf_frame_t;

typedef int (*gbmf_frame_cb)(void *user_data, const gbmf_frame_t *frame);

typedef struct {
  void *user_data;
  gbmf_frame_cb on_frame;
} gbmf_file_source_callbacks_t;

typedef struct {
  const char *path;
  int loop;
  int realtime;
  int width;
  int height;
  int fps_num;
  int fps_den;
  const char *video_pixel_format;
  const char *audio_sample_format;
} gbmf_file_source_config_t;

int gbmf_probe_file(const char *path, gbmf_file_info_t *info_out);
int gbmf_open(const gbmf_file_source_config_t *config,
              const gbmf_file_source_callbacks_t *callbacks,
              gbmf_file_source_t **source_out);
void gbmf_close(gbmf_file_source_t **source);
int gbmf_read(gbmf_file_source_t *source);
const char *gbmf_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif
