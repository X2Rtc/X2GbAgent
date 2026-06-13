#ifndef APP_SESSION_H
#define APP_SESSION_H

#include "gb_platform.h"
#include "mongoose.h"

#include <time.h>

#define MAX_SESSIONS 16
#define SESSION_TTL_SEC (24 * 60 * 60)

typedef struct {
  char token[64];
  time_t expires_at;
} session_t;

typedef struct {
  gb_mutex_t *mu;
  session_t *sessions;
  int session_count;
  const char *username;
  const char *password;
} gb_session_ctx_t;

int gb_session_is_valid(struct mg_http_message *hm, gb_session_ctx_t *ctx);
void gb_session_reply_login(struct mg_connection *c, struct mg_http_message *hm, gb_session_ctx_t *ctx);
void gb_session_reply_logout(struct mg_connection *c, struct mg_http_message *hm, gb_session_ctx_t *ctx);
void gb_session_reply_session(struct mg_connection *c, gb_session_ctx_t *ctx);
int gb_public_uri(struct mg_http_message *hm);
int gb_api_uri(struct mg_http_message *hm);

#endif
