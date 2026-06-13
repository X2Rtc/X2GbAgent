#ifndef APP_LOG_H
#define APP_LOG_H

#include "gb_platform.h"
#include "mongoose.h"

#include <sqlite3.h>

typedef struct {
  gb_mutex_t *mu;
  sqlite3 *db;
  struct gb_log_file *file;
} gb_log_ctx_t;

typedef struct {
  char directory[512];
  unsigned long long max_file_bytes;
  int rotate_count;
} gb_log_file_config_t;

typedef struct gb_log_file gb_log_file_t;

void gb_log_file_default_config(gb_log_file_config_t *config, const char *install_dir);
int gb_log_file_init(gb_log_file_t **log_out, const gb_log_file_config_t *config);
void gb_log_file_close(gb_log_file_t **log);

void gb_log_add(gb_log_ctx_t *ctx, const char *level, const char *category, const char *message);
void gb_log_add_tagged(gb_log_ctx_t *ctx,
                       const char *level,
                       const char *channel,
                       const char *module,
                       const char *submodule,
                       const char *message);
void gb_log_reply(struct mg_connection *c, gb_log_ctx_t *ctx, int max_lines);

#endif
