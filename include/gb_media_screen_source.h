#ifndef GB_MEDIA_SCREEN_SOURCE_H
#define GB_MEDIA_SCREEN_SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define GBMS_MAX_TEXT 256

typedef struct {
  char id[GBMS_MAX_TEXT];
  char name[GBMS_MAX_TEXT];
  int width;
  int height;
  int primary;
} gbms_screen_source_info_t;

int gbms_list_screens(gbms_screen_source_info_t *screens,
                      size_t max_screens,
                      size_t *count_out);
int gbms_screen_allowed(const char *id);

#ifdef __cplusplus
}
#endif

#endif
