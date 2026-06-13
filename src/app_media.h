#ifndef APP_MEDIA_H
#define APP_MEDIA_H

#include "mongoose.h"

typedef struct {
  const char *path;
  const char *label;
  const char *resolution;
  int bitrate_kbps;
  const char *container;
} media_file_info_t;

const media_file_info_t *gb_media_info_by_path(const char *path);
int gb_media_file_allowed(const char *path);
void gb_media_reply_files(struct mg_connection *c);

#endif
