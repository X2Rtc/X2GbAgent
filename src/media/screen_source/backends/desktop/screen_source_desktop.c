#include "gb_media_screen_source.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  if (!src) src = "";
  snprintf(dst, dst_size, "%s", src);
}

int gbms_list_screens(gbms_screen_source_info_t *screens,
                      size_t max_screens,
                      size_t *count_out) {
  if (!count_out) return -1;
  *count_out = 0;
#if defined(_WIN32)
  if (screens && max_screens > 0) {
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    memset(&screens[0], 0, sizeof(screens[0]));
    copy_text(screens[0].id, sizeof(screens[0].id), "screen://desktop");
    copy_text(screens[0].name, sizeof(screens[0].name), "Desktop Screen");
    screens[0].width = width > 0 ? width : GetSystemMetrics(SM_CXSCREEN);
    screens[0].height = height > 0 ? height : GetSystemMetrics(SM_CYSCREEN);
    screens[0].primary = 1;
  }
  *count_out = 1;
  return 0;
#else
  (void) screens;
  (void) max_screens;
  return 0;
#endif
}

int gbms_screen_allowed(const char *id) {
  gbms_screen_source_info_t screens[8];
  size_t count = 0;
  if (!id || !id[0]) return 0;
  if (gbms_list_screens(screens, sizeof(screens) / sizeof(screens[0]), &count) != 0) return 0;
  for (size_t i = 0; i < count; i++) {
    if (strcmp(screens[i].id, id) == 0) return 1;
  }
  return 0;
}
