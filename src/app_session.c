#include "app_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_HDR "Content-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n"

static int mg_str_eq_cstr(struct mg_str s, const char *cstr) {
  size_t n = cstr ? strlen(cstr) : 0;
  return s.len == n && n > 0 && memcmp(s.buf, cstr, n) == 0;
}

static void make_session_token(char *out, size_t out_size) {
  unsigned char rnd[24];
  static const char hex[] = "0123456789abcdef";
  if (!out || out_size < sizeof(rnd) * 2 + 1) return;
  if (!mg_random(rnd, sizeof(rnd))) {
    uint64_t seed = (uint64_t) time(NULL) ^ mg_millis();
    for (size_t i = 0; i < sizeof(rnd); i++) {
      seed = seed * 6364136223846793005ULL + 1;
      rnd[i] = (unsigned char) (seed >> 32);
    }
  }
  for (size_t i = 0; i < sizeof(rnd); i++) {
    out[i * 2] = hex[(rnd[i] >> 4) & 0x0f];
    out[i * 2 + 1] = hex[rnd[i] & 0x0f];
  }
  out[sizeof(rnd) * 2] = '\0';
}

static void session_create(char *token, size_t token_size, gb_session_ctx_t *ctx) {
  int slot = 0;
  time_t oldest;
  make_session_token(token, token_size);
  gb_mutex_lock(ctx->mu);
  oldest = ctx->sessions[0].expires_at;
  for (int i = 0; i < ctx->session_count; i++) {
    if (ctx->sessions[i].token[0] == '\0' ||
        ctx->sessions[i].expires_at <= time(NULL)) {
      slot = i;
      break;
    }
    if (ctx->sessions[i].expires_at < oldest) {
      oldest = ctx->sessions[i].expires_at;
      slot = i;
    }
  }
  snprintf(ctx->sessions[slot].token, sizeof(ctx->sessions[slot].token), "%s", token);
  ctx->sessions[slot].expires_at = time(NULL) + SESSION_TTL_SEC;
  gb_mutex_unlock(ctx->mu);
}

static void session_destroy(struct mg_http_message *hm, gb_session_ctx_t *ctx) {
  char user[64], token[64];
  mg_http_creds(hm, user, sizeof(user), token, sizeof(token));
  if (token[0] == '\0') return;
  gb_mutex_lock(ctx->mu);
  for (int i = 0; i < ctx->session_count; i++) {
    if (strcmp(ctx->sessions[i].token, token) == 0) {
      ctx->sessions[i].token[0] = '\0';
      ctx->sessions[i].expires_at = 0;
    }
  }
  gb_mutex_unlock(ctx->mu);
}

int gb_session_is_valid(struct mg_http_message *hm, gb_session_ctx_t *ctx) {
  char user[64], token[64];
  time_t now = time(NULL);
  int ok = 0;
  mg_http_creds(hm, user, sizeof(user), token, sizeof(token));
  if (token[0] == '\0') return 0;
  gb_mutex_lock(ctx->mu);
  for (int i = 0; i < ctx->session_count; i++) {
    if (ctx->sessions[i].token[0] &&
        strcmp(ctx->sessions[i].token, token) == 0 &&
        ctx->sessions[i].expires_at > now) {
      ctx->sessions[i].expires_at = now + SESSION_TTL_SEC;
      ok = 1;
      break;
    }
  }
  gb_mutex_unlock(ctx->mu);
  return ok;
}

void gb_session_reply_login(struct mg_connection *c, struct mg_http_message *hm, gb_session_ctx_t *ctx) {
  char *user = mg_json_get_str(hm->body, "$.username");
  char *pass = mg_json_get_str(hm->body, "$.password");
  char token[64] = {0};
  char headers[256];
  const char *expected_user = ctx != NULL && ctx->username != NULL ? ctx->username : "";
  const char *expected_pass = ctx != NULL && ctx->password != NULL ? ctx->password : "";
  if (user && pass && expected_user[0] && expected_pass[0] &&
      strcmp(user, expected_user) == 0 && strcmp(pass, expected_pass) == 0) {
    session_create(token, sizeof(token), ctx);
    mg_snprintf(headers,
                sizeof(headers),
                "Set-Cookie: access_token=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d\r\n%s",
                token,
                SESSION_TTL_SEC,
                JSON_HDR);
    mg_http_reply(c, 200, headers, "{%m:%m,%m:%d}\n",
                  MG_ESC("user"), MG_ESC(expected_user),
                  MG_ESC("expires_in"), SESSION_TTL_SEC);
  } else {
    mg_http_reply(c, 401, JSON_HDR, "{%m:%m}\n", MG_ESC("error"), MG_ESC("invalid credentials"));
  }
  free(user);
  free(pass);
}

void gb_session_reply_logout(struct mg_connection *c, struct mg_http_message *hm, gb_session_ctx_t *ctx) {
  session_destroy(hm, ctx);
  mg_http_reply(c, 204, "Set-Cookie: access_token=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n", "");
}

void gb_session_reply_session(struct mg_connection *c, gb_session_ctx_t *ctx) {
  const char *user = ctx != NULL && ctx->username != NULL ? ctx->username : "";
  mg_http_reply(c, 200, JSON_HDR, "{%m:%m}\n", MG_ESC("user"), MG_ESC(user));
}

int gb_public_uri(struct mg_http_message *hm) {
  return mg_str_eq_cstr(hm->uri, "/login.html") ||
         mg_str_eq_cstr(hm->uri, "/style.css") ||
         mg_str_eq_cstr(hm->uri, "/i18n.js") ||
         mg_str_eq_cstr(hm->uri, "/api/login") ||
         mg_str_eq_cstr(hm->uri, "/oem.config.json") ||
         (hm->uri.len >= 8 && memcmp(hm->uri.buf, "/assets/", 8) == 0) ||
         mg_str_eq_cstr(hm->uri, "/favicon.ico");
}

int gb_api_uri(struct mg_http_message *hm) {
  return hm->uri.len >= 5 && memcmp(hm->uri.buf, "/api/", 5) == 0;
}
