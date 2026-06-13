#include "app_media.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define JSON_HDR "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n"

static const media_file_info_t s_media_files[] = {
    {"data/Big_Buck_Bunny_720_10s_2MB.mp4", "Big Buck Bunny 720p MP4", "1280x720", 1600, "mp4"},
};

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
    } else if (ch == '\n') {
      buf[len++] = '\\';
      buf[len++] = 'n';
    } else if (ch == '\r') {
      buf[len++] = '\\';
      buf[len++] = 'r';
    } else if (ch == '\t') {
      buf[len++] = '\\';
      buf[len++] = 't';
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

const media_file_info_t *gb_media_info_by_path(const char *path) {
  for (size_t i = 0; i < sizeof(s_media_files) / sizeof(s_media_files[0]); i++) {
    if (strcmp(s_media_files[i].path, path) == 0) return &s_media_files[i];
  }
  return NULL;
}

int gb_media_file_allowed(const char *path) {
  return gb_media_info_by_path(path) != NULL;
}

void gb_media_reply_files(struct mg_connection *c) {
  char body[4096];
  size_t len = 0;
  body[len++] = '[';
  for (size_t i = 0; i < sizeof(s_media_files) / sizeof(s_media_files[0]); i++) {
    struct stat st;
    long long size = stat(s_media_files[i].path, &st) == 0 ? (long long) st.st_size : 0;
    len = appendf(body, sizeof(body), len, "%s{\"path\":", i ? "," : "");
    len = append_json_string(body, sizeof(body), len, s_media_files[i].path);
    len = appendf(body, sizeof(body), len, ",\"label\":");
    len = append_json_string(body, sizeof(body), len, s_media_files[i].label);
    len = appendf(body, sizeof(body), len, ",\"resolution\":");
    len = append_json_string(body, sizeof(body), len, s_media_files[i].resolution);
    len = appendf(body, sizeof(body), len, ",\"container\":");
    len = append_json_string(body, sizeof(body), len, s_media_files[i].container);
    len = appendf(body, sizeof(body), len,
                  ",\"bitrate_kbps\":%d,\"size_bytes\":%lld}",
                  s_media_files[i].bitrate_kbps, size);
  }
  len = appendf(body, sizeof(body), len, "]\n");
  body[sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}
