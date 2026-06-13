#include "app_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#define GB_MKDIR(path) _mkdir(path)
#define GB_PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define GB_MKDIR(path) mkdir(path, 0755)
#define GB_PATH_SEP '/'
#endif

#define JSON_HDR "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n"
#define GB_LOG_DEFAULT_MAX_FILE_BYTES (10ULL * 1024ULL * 1024ULL)
#define GB_LOG_DEFAULT_ROTATE_COUNT 3
#define GB_LOG_FILE_NAME "gb28181-agent.log"

struct gb_log_file {
  gb_mutex_t mu;
  gb_log_file_config_t config;
  FILE *fp;
  unsigned long long current_size;
  char active_path[768];
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

static int text_eq(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

static const char *normalize_level(const char *level) {
  if (text_eq(level, "INFO")) return "INFO";
  if (text_eq(level, "WARN")) return "WARN";
  if (text_eq(level, "ERROR")) return "ERROR";
  if (text_eq(level, "DEBUG")) return "DEBUG";
  return "INFO";
}

static const char *normalize_module(const char *module) {
  if (text_eq(module, "CONFIG")) return "CONFIG";
  if (text_eq(module, "MEDIA")) return "MEDIA";
  if (text_eq(module, "OTA")) return "OTA";
  if (text_eq(module, "RTP")) return "RTP";
  if (text_eq(module, "SIP")) return "SIP";
  if (text_eq(module, "SYSTEM")) return "SYSTEM";
  return module && module[0] ? module : "SYSTEM";
}

static const char *default_submodule(const char *module) {
  if (text_eq(module, "SIP")) return "STATE";
  if (text_eq(module, "RTP")) return "STREAM";
  if (text_eq(module, "MEDIA")) return "PIPELINE";
  if (text_eq(module, "CONFIG")) return "UPDATE";
  if (text_eq(module, "OTA")) return "UPDATE";
  return "GENERAL";
}

static const char *safe_channel(const char *channel) {
  return channel && channel[0] ? channel : "SYSTEM";
}

static int path_join(char *out, size_t out_size, const char *dir, const char *name) {
  size_t n;
  if (!out || out_size == 0 || !dir || !dir[0] || !name || !name[0]) return -1;
  n = strlen(dir);
  if (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) {
    snprintf(out, out_size, "%s%s", dir, name);
  } else {
    snprintf(out, out_size, "%s%c%s", dir, GB_PATH_SEP, name);
  }
  return 0;
}

static int ensure_dir(const char *path) {
  char tmp[512];
  size_t len;
  if (!path || !path[0]) return -1;
  copy_text(tmp, sizeof(tmp), path);
  len = strlen(tmp);
  while (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[--len] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/' && *p != '\\') continue;
#if defined(_WIN32)
    if (p == tmp + 2 && tmp[1] == ':') continue;
#endif
    char saved = *p;
    *p = '\0';
    if (tmp[0] && GB_MKDIR(tmp) != 0 && errno != EEXIST) return -1;
    *p = saved;
  }
  if (GB_MKDIR(tmp) != 0 && errno != EEXIST) return -1;
  return 0;
}

static unsigned long long file_size(const char *path) {
  FILE *fp = fopen(path, "rb");
  long size;
  if (!fp) return 0;
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return 0;
  }
  size = ftell(fp);
  fclose(fp);
  return size > 0 ? (unsigned long long) size : 0;
}

static void rotated_path(const gb_log_file_t *log, int index, char *out, size_t out_size) {
  char name[128];
  if (index <= 0) {
    copy_text(out, out_size, log->active_path);
    return;
  }
  snprintf(name, sizeof(name), "%s.%d", GB_LOG_FILE_NAME, index);
  (void) path_join(out, out_size, log->config.directory, name);
}

static int open_active_file(gb_log_file_t *log) {
  if (!log) return -1;
  log->fp = fopen(log->active_path, "ab");
  if (!log->fp) return -1;
  log->current_size = file_size(log->active_path);
  return 0;
}

static void rotate_files(gb_log_file_t *log) {
  char src[768];
  char dst[768];
  int count;
  if (!log) return;
  if (log->fp) {
    fclose(log->fp);
    log->fp = NULL;
  }
  count = log->config.rotate_count;
  if (count <= 1) {
    remove(log->active_path);
    log->current_size = 0;
    (void) open_active_file(log);
    return;
  }
  rotated_path(log, count - 1, dst, sizeof(dst));
  remove(dst);
  for (int i = count - 2; i >= 1; i--) {
    rotated_path(log, i, src, sizeof(src));
    rotated_path(log, i + 1, dst, sizeof(dst));
    (void) rename(src, dst);
  }
  rotated_path(log, 1, dst, sizeof(dst));
  (void) rename(log->active_path, dst);
  log->current_size = 0;
  (void) open_active_file(log);
}

static void format_time_utc(char *out, size_t out_size, time_t now) {
  struct tm tmv;
#if defined(_WIN32)
  gmtime_s(&tmv, &now);
#else
  gmtime_r(&now, &tmv);
#endif
  strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void gb_log_file_default_config(gb_log_file_config_t *config, const char *install_dir) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  if (install_dir && install_dir[0]) {
    (void) path_join(config->directory, sizeof(config->directory), install_dir, "logs");
  } else {
    copy_text(config->directory, sizeof(config->directory), "logs");
  }
  config->max_file_bytes = GB_LOG_DEFAULT_MAX_FILE_BYTES;
  config->rotate_count = GB_LOG_DEFAULT_ROTATE_COUNT;
}

int gb_log_file_init(gb_log_file_t **log_out, const gb_log_file_config_t *config) {
  gb_log_file_t *log;
  if (!log_out || !config) return -1;
  *log_out = NULL;
  if (!config->directory[0] || config->max_file_bytes == 0 || config->rotate_count <= 0) return -1;
  if (ensure_dir(config->directory) != 0) return -1;
  log = (gb_log_file_t *) calloc(1, sizeof(*log));
  if (!log) return -1;
  log->config = *config;
  if (log->config.rotate_count < 1) log->config.rotate_count = 1;
  gb_mutex_init(&log->mu);
  if (path_join(log->active_path, sizeof(log->active_path), log->config.directory, GB_LOG_FILE_NAME) != 0 ||
      open_active_file(log) != 0) {
    gb_mutex_destroy(&log->mu);
    free(log);
    return -1;
  }
  *log_out = log;
  return 0;
}

void gb_log_file_close(gb_log_file_t **log) {
  if (!log || !*log) return;
  gb_log_file_t *local = *log;
  *log = NULL;
  gb_mutex_lock(&local->mu);
  if (local->fp) fclose(local->fp);
  local->fp = NULL;
  gb_mutex_unlock(&local->mu);
  gb_mutex_destroy(&local->mu);
  free(local);
}

static void gb_log_file_write(gb_log_file_t *log,
                              const char *level,
                              const char *channel,
                              const char *module,
                              const char *submodule,
                              const char *message) {
  char line[1400];
  char ts[32];
  int n;
  if (!log) return;
  format_time_utc(ts, sizeof(ts), time(NULL));
  n = snprintf(line, sizeof(line), "%s %-5s [%s][%s][%s] %s\n",
               ts,
               level,
               safe_channel(channel),
               module,
               submodule,
               message ? message : "");
  if (n <= 0) return;
  if ((size_t) n >= sizeof(line)) {
    n = (int) sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = '\0';
  }
  gb_mutex_lock(&log->mu);
  if (!log->fp && open_active_file(log) != 0) {
    gb_mutex_unlock(&log->mu);
    return;
  }
  if (log->current_size > 0 &&
      log->current_size + (unsigned long long) n > log->config.max_file_bytes) {
    rotate_files(log);
  }
  if (log->fp) {
    fwrite(line, 1, (size_t) n, log->fp);
    fflush(log->fp);
    log->current_size += (unsigned long long) n;
  }
  gb_mutex_unlock(&log->mu);
}

void gb_log_add_tagged(gb_log_ctx_t *ctx,
                       const char *level,
                       const char *channel,
                       const char *module,
                       const char *submodule,
                       const char *message) {
  sqlite3_stmt *st = NULL;
  const char *normalized_level = normalize_level(level);
  const char *normalized_module = normalize_module(module);
  const char *normalized_submodule = submodule && submodule[0] ? submodule : default_submodule(normalized_module);
  char tagged_message[1024];
  snprintf(tagged_message, sizeof(tagged_message), "[%s][%s][%s] %s",
           safe_channel(channel),
           normalized_module,
           normalized_submodule,
           message ? message : "");
  if (ctx && ctx->db && ctx->mu) {
    gb_mutex_lock(ctx->mu);
    sqlite3_prepare_v2(ctx->db,
                       "INSERT INTO logs(ts, level, category, message) "
                       "VALUES(strftime('%s','now'), ?, ?, ?)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, normalized_level, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, normalized_module, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, tagged_message, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_exec(ctx->db,
                 "DELETE FROM logs WHERE id NOT IN "
                 "(SELECT id FROM logs ORDER BY id DESC LIMIT 1000)",
                 NULL, NULL, NULL);
    gb_mutex_unlock(ctx->mu);
  }
  if (ctx) {
    gb_log_file_write(ctx->file,
                      normalized_level,
                      safe_channel(channel),
                      normalized_module,
                      normalized_submodule,
                      message);
  }
}

void gb_log_add(gb_log_ctx_t *ctx, const char *level, const char *category, const char *message) {
  gb_log_add_tagged(ctx, level, "SYSTEM", category, NULL, message);
}

void gb_log_reply(struct mg_connection *c, gb_log_ctx_t *ctx, int max_lines) {
  char body[32768];
  size_t len = 0;
  sqlite3_stmt *st = NULL;
  int n = 0;
  body[len++] = '[';
  gb_mutex_lock(ctx->mu);
  sqlite3_prepare_v2(ctx->db, "SELECT ts,level,category,message FROM logs "
                              "ORDER BY id DESC LIMIT ?", -1, &st, NULL);
  sqlite3_bind_int(st, 1, max_lines);
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (len >= sizeof(body) - 256) break;
    len = appendf(body, sizeof(body), len, "%s{\"ts\":%lld,\"level\":",
                  n++ ? "," : "", sqlite3_column_int64(st, 0));
    len = append_json_string(body, sizeof(body), len, (const char *) sqlite3_column_text(st, 1));
    len = appendf(body, sizeof(body), len, ",\"category\":");
    len = append_json_string(body, sizeof(body), len, (const char *) sqlite3_column_text(st, 2));
    len = appendf(body, sizeof(body), len, ",\"message\":");
    len = append_json_string(body, sizeof(body), len, (const char *) sqlite3_column_text(st, 3));
    len = appendf(body, sizeof(body), len, "}");
  }
  sqlite3_finalize(st);
  gb_mutex_unlock(ctx->mu);
  if (len < sizeof(body) - 2) {
    body[len++] = ']';
    body[len++] = '\n';
  }
  body[len < sizeof(body) ? len : sizeof(body) - 1] = '\0';
  mg_http_reply(c, 200, JSON_HDR, "%s", body);
}
