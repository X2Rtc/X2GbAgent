#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sql_exec(sqlite3 *db, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "sqlite error: %s\n", err ? err : "unknown");
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

static void sql_exec_ignore(sqlite3 *db, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) sqlite3_free(err);
}

static int table_sql_contains(sqlite3 *db, const char *table, const char *needle) {
  sqlite3_stmt *st = NULL;
  int found = 0;
  if (sqlite3_prepare_v2(db,
                         "SELECT sql FROM sqlite_master WHERE type='table' AND name=?",
                         -1,
                         &st,
                         NULL) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *sql = sqlite3_column_text(st, 0);
    found = sql != NULL && strstr((const char *) sql, needle) != NULL;
  }
  sqlite3_finalize(st);
  return found;
}

void init_platform_default(platform_cfg_t *p, int id) {
  memset(p, 0, sizeof(*p));
  p->id = id;
  snprintf(p->transport, sizeof(p->transport), "%s", "UDP");
  snprintf(p->media_proto, sizeof(p->media_proto), "%s", "RTC");
}

int gb_channel_make_id(const char *base_device_id, int ordinal, char *out, size_t out_size) {
  char prefix[15];
  if (out == NULL || out_size == 0 || ordinal < 1 || ordinal > MAX_CHANNELS_PER_CLIENT) return -1;
  out[0] = '\0';
  if (base_device_id == NULL || strlen(base_device_id) < 14) return -1;
  memset(prefix, 0, sizeof(prefix));
  memcpy(prefix, base_device_id, 14);
  snprintf(out, out_size, "%s%06d", prefix, ordinal);
  return 0;
}

void init_gb_channel_default(gb_channel_cfg_t *ch, int id, int client_id, int ordinal, const char *base_device_id) {
  if (ch == NULL) return;
  memset(ch, 0, sizeof(*ch));
  ch->id = id;
  ch->client_id = client_id;
  ch->ordinal = ordinal;
  (void) gb_channel_make_id(base_device_id, ordinal, ch->channel_id, sizeof(ch->channel_id));
  snprintf(ch->name, sizeof(ch->name), "CH%d", ordinal);
  snprintf(ch->media_proto, sizeof(ch->media_proto), "%s", "RTC");
}

void init_device_source_default(device_source_cfg_t *d) {
  memset(d, 0, sizeof(*d));
  snprintf(d->source_mode, sizeof(d->source_mode), "%s", "device");
  snprintf(d->media_file, sizeof(d->media_file), "%s", "data/Big_Buck_Bunny_720_10s_2MB.mp4");
  snprintf(d->resolution, sizeof(d->resolution), "%s", "1280x720");
  snprintf(d->file_pacing, sizeof(d->file_pacing), "%s", "realtime");
  d->bitrate_kbps = 1600;
  d->file_loop = 1;
}

static void init_video_default(video_cfg_t *v) {
  memset(v, 0, sizeof(*v));
  snprintf(v->codec, sizeof(v->codec), "%s", "H264");
  snprintf(v->resolution, sizeof(v->resolution), "%s", "1920x1080");
  snprintf(v->rc_mode, sizeof(v->rc_mode), "%s", "CBR");
  v->fps = 25;
  v->bitrate_kbps = 4096;
  v->gop = 75;
  v->iframe_interval = 3;
  v->low_latency = 1;
  v->prefer_hardware = 0;
}

static void init_audio_default(audio_cfg_t *a) {
  memset(a, 0, sizeof(*a));
  a->enabled = 1;
  snprintf(a->codec, sizeof(a->codec), "%s", "G711A");
  a->sample_rate = 8000;
  a->bitrate_kbps = 64;
}

device_source_cfg_t effective_channel_source(const device_source_cfg_t *global_source,
                                             const device_source_cfg_t *channel_source,
                                             const char *profile) {
  if (profile != NULL && strcmp(profile, "none") == 0) {
    device_source_cfg_t none;
    init_device_source_default(&none);
    snprintf(none.source_mode, sizeof(none.source_mode), "%s", "none");
    none.video_device[0] = '\0';
    none.audio_device[0] = '\0';
    none.media_file[0] = '\0';
    return none;
  }
  if (profile != NULL && strcmp(profile, "custom") == 0) return *channel_source;
  return *global_source;
}

static void reset_config(gb_app_config_t *config) {
  memset(config, 0, sizeof(*config));
  init_video_default(&config->video);
  init_audio_default(&config->audio);
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    init_platform_default(&config->platforms[i], i + 1);
  }
  for (int i = 0; i < MAX_CHANNELS; i++) {
    int client_id = i / MAX_CHANNELS_PER_CLIENT + 1;
    int ordinal = i % MAX_CHANNELS_PER_CLIENT + 1;
    init_gb_channel_default(&config->gb_channels[i], i + 1, client_id, ordinal, config->platforms[client_id - 1].device_id);
    config->gb_channels[i].id = 0;
    init_device_source_default(&config->channel_sources[i]);
    init_video_default(&config->channel_videos[i]);
    init_audio_default(&config->channel_audios[i]);
    snprintf(config->channel_source_profile[i], sizeof(config->channel_source_profile[i]), "%s", "none");
  }
}

int gb_config_open(sqlite3 **db, const char *path) {
  if (sqlite3_open(path, db) != SQLITE_OK) return -1;
  if (sql_exec(*db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") != 0) return -1;
  /* Pre-release schema reset: channel_* tables are the only platform model. */
  if (sql_exec(*db, "DROP TABLE IF EXISTS platforms;") != 0) return -1;
  if (sql_exec(*db,
               "CREATE TABLE IF NOT EXISTS video_config("
               "id INTEGER PRIMARY KEY CHECK(id=1), codec TEXT NOT NULL,"
               "resolution TEXT NOT NULL, fps INTEGER NOT NULL,"
               "rc_mode TEXT NOT NULL CHECK(rc_mode IN ('CBR','VBR')),"
               "bitrate_kbps INTEGER NOT NULL, gop INTEGER NOT NULL,"
               "iframe_interval INTEGER NOT NULL, low_latency INTEGER NOT NULL DEFAULT 1,"
               "prefer_hardware INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS audio_config("
               "id INTEGER PRIMARY KEY CHECK(id=1), enabled INTEGER NOT NULL,"
               "codec TEXT NOT NULL, sample_rate INTEGER NOT NULL,"
               "bitrate_kbps INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS device_source_config("
               "id INTEGER PRIMARY KEY CHECK(id=1),"
               "source_mode TEXT NOT NULL CHECK(source_mode IN ('device','file','screen')),"
               "video_device TEXT NOT NULL, audio_device TEXT NOT NULL,"
               "media_file TEXT NOT NULL, resolution TEXT NOT NULL,"
               "bitrate_kbps INTEGER NOT NULL, file_loop INTEGER NOT NULL,"
               "file_pacing TEXT NOT NULL CHECK(file_pacing IN ('realtime','fast')),"
               "updated_at INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS channels("
               "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS) "),"
               "enabled INTEGER NOT NULL DEFAULT 0,"
               "name TEXT NOT NULL DEFAULT '',"
               "updated_at INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS gb_clients("
               "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND " GB_STRINGIFY(MAX_GB_CLIENTS) "),"
               "enabled INTEGER NOT NULL DEFAULT 0,"
               "name TEXT NOT NULL DEFAULT '',"
               "server_ip TEXT NOT NULL DEFAULT '',"
               "sip_port INTEGER NOT NULL DEFAULT 0,"
               "sip_id TEXT NOT NULL DEFAULT '',"
               "device_id TEXT NOT NULL DEFAULT '',"
               "username TEXT NOT NULL DEFAULT '',"
               "password TEXT NOT NULL DEFAULT '',"
               "transport TEXT NOT NULL DEFAULT 'UDP' CHECK(transport IN ('UDP','TCP','TrUdp')),"
               "media_proto TEXT NOT NULL DEFAULT 'RTC' CHECK(media_proto IN ('RTP','RTC')),"
               "register_interval INTEGER NOT NULL DEFAULT 0,"
               "heartbeat_interval INTEGER NOT NULL DEFAULT 0,"
               "updated_at INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS gb_client_channels("
               "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS) "),"
               "client_id INTEGER NOT NULL CHECK(client_id BETWEEN 1 AND " GB_STRINGIFY(MAX_GB_CLIENTS) "),"
               "ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 1 AND " GB_STRINGIFY(MAX_CHANNELS_PER_CLIENT) "),"
               "channel_id TEXT NOT NULL DEFAULT '',"
               "name TEXT NOT NULL DEFAULT '',"
               "media_proto TEXT NOT NULL DEFAULT 'RTC' CHECK(media_proto IN ('RTP','RTC')),"
               "updated_at INTEGER NOT NULL,"
               "UNIQUE(client_id, ordinal));"
               "CREATE TABLE IF NOT EXISTS channel_platform_config("
               "channel_id INTEGER PRIMARY KEY,"
               "server_ip TEXT NOT NULL DEFAULT '',"
               "sip_port INTEGER NOT NULL DEFAULT 0,"
               "sip_id TEXT NOT NULL DEFAULT '',"
               "device_id TEXT NOT NULL DEFAULT '',"
               "username TEXT NOT NULL DEFAULT '',"
               "password TEXT NOT NULL DEFAULT '',"
               "transport TEXT NOT NULL DEFAULT 'UDP' CHECK(transport IN ('UDP','TCP','TrUdp')),"
               "media_proto TEXT NOT NULL DEFAULT 'RTC' CHECK(media_proto IN ('RTP','RTC')),"
               "register_interval INTEGER NOT NULL DEFAULT 0,"
               "heartbeat_interval INTEGER NOT NULL DEFAULT 0,"
               "updated_at INTEGER NOT NULL,"
               "FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE);"
               "CREATE TABLE IF NOT EXISTS channel_source_config("
               "channel_id INTEGER PRIMARY KEY,"
               "source_profile TEXT NOT NULL DEFAULT 'none' CHECK(source_profile IN ('none','global','custom')),"
                "source_mode TEXT NOT NULL DEFAULT 'device' CHECK(source_mode IN ('device','file','screen')),"
               "video_device TEXT NOT NULL DEFAULT '', audio_device TEXT NOT NULL DEFAULT '',"
               "media_file TEXT NOT NULL DEFAULT '', resolution TEXT NOT NULL DEFAULT '',"
               "bitrate_kbps INTEGER NOT NULL DEFAULT 0, file_loop INTEGER NOT NULL DEFAULT 1,"
               "file_pacing TEXT NOT NULL DEFAULT 'realtime' CHECK(file_pacing IN ('realtime','fast')),"
               "updated_at INTEGER NOT NULL,"
               "FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE);"
               "CREATE TABLE IF NOT EXISTS channel_av_config("
               "channel_id INTEGER PRIMARY KEY,"
               "video_codec TEXT NOT NULL DEFAULT 'H264',"
               "resolution TEXT NOT NULL DEFAULT '1920x1080',"
               "fps INTEGER NOT NULL DEFAULT 25,"
               "rc_mode TEXT NOT NULL DEFAULT 'CBR' CHECK(rc_mode IN ('CBR','VBR')),"
               "video_bitrate_kbps INTEGER NOT NULL DEFAULT 4096,"
               "gop INTEGER NOT NULL DEFAULT 75,"
               "iframe_interval INTEGER NOT NULL DEFAULT 3,"
               "low_latency INTEGER NOT NULL DEFAULT 1,"
               "prefer_hardware INTEGER NOT NULL DEFAULT 0,"
               "audio_enabled INTEGER NOT NULL DEFAULT 1,"
               "audio_codec TEXT NOT NULL DEFAULT 'G711A',"
               "sample_rate INTEGER NOT NULL DEFAULT 8000,"
               "audio_bitrate_kbps INTEGER NOT NULL DEFAULT 64,"
               "updated_at INTEGER NOT NULL,"
               "FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE);"
               "CREATE TABLE IF NOT EXISTS logs("
               "id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,"
               "level TEXT NOT NULL, category TEXT NOT NULL, message TEXT NOT NULL);"
               "CREATE TABLE IF NOT EXISTS ota_jobs("
               "id INTEGER PRIMARY KEY AUTOINCREMENT, filename TEXT NOT NULL,"
               "size_bytes INTEGER NOT NULL, status TEXT NOT NULL,"
               "created_at INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS console_auth("
               "id INTEGER PRIMARY KEY CHECK(id=1),"
               "username TEXT NOT NULL, password TEXT NOT NULL,"
               "updated_at INTEGER NOT NULL);") != 0) return -1;
  if (!table_sql_contains(*db, "device_source_config", "'screen'")) {
    if (sql_exec(*db,
                 "BEGIN;"
                 "ALTER TABLE device_source_config RENAME TO device_source_config_old;"
                 "CREATE TABLE device_source_config("
                 "id INTEGER PRIMARY KEY CHECK(id=1),"
                 "source_mode TEXT NOT NULL CHECK(source_mode IN ('device','file','screen')),"
                 "video_device TEXT NOT NULL, audio_device TEXT NOT NULL,"
                 "media_file TEXT NOT NULL, resolution TEXT NOT NULL,"
                 "bitrate_kbps INTEGER NOT NULL, file_loop INTEGER NOT NULL,"
                 "file_pacing TEXT NOT NULL CHECK(file_pacing IN ('realtime','fast')),"
                 "updated_at INTEGER NOT NULL);"
                 "INSERT INTO device_source_config SELECT * FROM device_source_config_old;"
                 "DROP TABLE device_source_config_old;"
                 "COMMIT;") != 0) {
      sql_exec_ignore(*db, "ROLLBACK;");
      return -1;
    }
  }
  if (!table_sql_contains(*db, "channel_source_config", "'screen'")) {
    if (sql_exec(*db,
                 "BEGIN;"
                 "ALTER TABLE channel_source_config RENAME TO channel_source_config_old;"
                 "CREATE TABLE channel_source_config("
                 "channel_id INTEGER PRIMARY KEY,"
                 "source_profile TEXT NOT NULL DEFAULT 'none' CHECK(source_profile IN ('none','global','custom')),"
                 "source_mode TEXT NOT NULL DEFAULT 'device' CHECK(source_mode IN ('device','file','screen')),"
                 "video_device TEXT NOT NULL DEFAULT '', audio_device TEXT NOT NULL DEFAULT '',"
                 "media_file TEXT NOT NULL DEFAULT '', resolution TEXT NOT NULL DEFAULT '',"
                 "bitrate_kbps INTEGER NOT NULL DEFAULT 0, file_loop INTEGER NOT NULL DEFAULT 1,"
                 "file_pacing TEXT NOT NULL DEFAULT 'realtime' CHECK(file_pacing IN ('realtime','fast')),"
                 "updated_at INTEGER NOT NULL,"
                 "FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE);"
                 "INSERT INTO channel_source_config SELECT * FROM channel_source_config_old;"
                 "DROP TABLE channel_source_config_old;"
                 "COMMIT;") != 0) {
      sql_exec_ignore(*db, "ROLLBACK;");
      return -1;
    }
  }
  if (!table_sql_contains(*db, "channel_platform_config", "'TrUdp'")) {
    if (sql_exec(*db,
                 "BEGIN;"
                 "ALTER TABLE channel_platform_config RENAME TO channel_platform_config_old;"
                 "CREATE TABLE channel_platform_config("
                 "channel_id INTEGER PRIMARY KEY,"
                 "server_ip TEXT NOT NULL DEFAULT '',"
                 "sip_port INTEGER NOT NULL DEFAULT 0,"
                 "sip_id TEXT NOT NULL DEFAULT '',"
                 "device_id TEXT NOT NULL DEFAULT '',"
                 "username TEXT NOT NULL DEFAULT '',"
                 "password TEXT NOT NULL DEFAULT '',"
                 "transport TEXT NOT NULL DEFAULT 'UDP' CHECK(transport IN ('UDP','TCP','TrUdp')),"
                 "media_proto TEXT NOT NULL DEFAULT 'RTC' CHECK(media_proto IN ('RTP','RTC')),"
                 "register_interval INTEGER NOT NULL DEFAULT 0,"
                 "heartbeat_interval INTEGER NOT NULL DEFAULT 0,"
                 "updated_at INTEGER NOT NULL,"
                 "FOREIGN KEY(channel_id) REFERENCES channels(id) ON DELETE CASCADE);"
                 "INSERT INTO channel_platform_config(channel_id,server_ip,sip_port,sip_id,device_id,"
                 "username,password,transport,media_proto,register_interval,heartbeat_interval,updated_at) "
                 "SELECT channel_id,server_ip,sip_port,sip_id,device_id,username,password,transport,"
                 "'RTC',register_interval,heartbeat_interval,updated_at FROM channel_platform_config_old;"
                 "DROP TABLE channel_platform_config_old;"
                 "COMMIT;") != 0) {
      sql_exec_ignore(*db, "ROLLBACK;");
      return -1;
    }
  }
  if (!table_sql_contains(*db, "channels", "BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS))) {
    if (sql_exec(*db,
                 "BEGIN;"
                 "ALTER TABLE channels RENAME TO channels_old;"
                 "CREATE TABLE channels("
                 "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS) "),"
                 "enabled INTEGER NOT NULL DEFAULT 0,"
                 "name TEXT NOT NULL DEFAULT '',"
                 "updated_at INTEGER NOT NULL);"
                 "INSERT OR IGNORE INTO channels(id,enabled,name,updated_at) "
                 "SELECT old.id,old.enabled,old.name,old.updated_at FROM channels_old old "
                 "WHERE old.id BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS) " "
                 "AND EXISTS (SELECT 1 FROM gb_client_channels gcc WHERE gcc.id=old.id);"
                 "DROP TABLE channels_old;"
                 "COMMIT;") != 0) {
      sql_exec_ignore(*db, "ROLLBACK;");
      return -1;
    }
  }
  if (table_sql_contains(*db, "gb_client_channels", "UNIQUE(channel_id)") ||
      !table_sql_contains(*db, "gb_client_channels", "BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS))) {
    if (sql_exec(*db,
                 "BEGIN;"
                 "ALTER TABLE gb_client_channels RENAME TO gb_client_channels_old;"
                 "CREATE TABLE gb_client_channels("
                 "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS) "),"
                 "client_id INTEGER NOT NULL CHECK(client_id BETWEEN 1 AND " GB_STRINGIFY(MAX_GB_CLIENTS) "),"
                 "ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 1 AND " GB_STRINGIFY(MAX_CHANNELS_PER_CLIENT) "),"
                 "channel_id TEXT NOT NULL DEFAULT '',"
                 "name TEXT NOT NULL DEFAULT '',"
                 "media_proto TEXT NOT NULL DEFAULT 'RTC' CHECK(media_proto IN ('RTP','RTC')),"
                 "updated_at INTEGER NOT NULL,"
                 "UNIQUE(client_id, ordinal));"
                 "INSERT OR IGNORE INTO gb_client_channels(id,client_id,ordinal,channel_id,name,media_proto,updated_at) "
                 "SELECT id,client_id,ordinal,channel_id,name,media_proto,updated_at FROM gb_client_channels_old "
                 "WHERE id BETWEEN 1 AND " GB_STRINGIFY(GB_MAX_CHANNELS) ";"
                 "DROP TABLE gb_client_channels_old;"
                 "COMMIT;") != 0) {
      sql_exec_ignore(*db, "ROLLBACK;");
      return -1;
    }
  }
  sql_exec_ignore(*db, "ALTER TABLE channel_platform_config ADD COLUMN media_proto TEXT NOT NULL DEFAULT 'RTC'");
  sql_exec_ignore(*db, "ALTER TABLE video_config ADD COLUMN low_latency INTEGER NOT NULL DEFAULT 1");
  sql_exec_ignore(*db, "ALTER TABLE video_config ADD COLUMN prefer_hardware INTEGER NOT NULL DEFAULT 0");
  /*
   * Intentional schema break:
   * The current release uses gb_clients + gb_client_channels as the only
   * authoritative GB28181 model. Legacy channel_platform_config rows are not
   * migrated on purpose; deployments must use the current install/seed
   * database or recreate platform accounts through the current APIs.
   *
   * Do not add an automatic legacy channels -> gb_client_channels migration
   * here. That would reintroduce obsolete one-channel-per-platform semantics
   * into the new two-client/multi-channel model.
   *
   * Because legacy channels are intentionally not migrated, the legacy
   * occupancy rows must not survive without a gb_client_channels row. Keeping
   * those rows would make the new API show no channel but still report the
   * per-client slots as occupied.
   */
  sql_exec_ignore(*db,
                  "DELETE FROM channels WHERE id NOT IN "
                  "(SELECT id FROM gb_client_channels);");
  sql_exec_ignore(*db,
                  "UPDATE gb_client_channels SET channel_id='',updated_at=strftime('%s','now') "
                  "WHERE substr(channel_id,1,14)='11000000001327' OR substr(channel_id,1,14)='00000000000000';");
  {
    sqlite3_stmt *st = NULL;
    sqlite3_stmt *up = NULL;
    if (sqlite3_prepare_v2(*db, "SELECT id,device_id FROM gb_clients WHERE length(device_id)>=14", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(*db,
                           "UPDATE gb_client_channels SET channel_id=?,updated_at=strftime('%s','now') "
                           "WHERE client_id=? AND ordinal=?",
                           -1,
                           &up,
                           NULL) == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW) {
        int client_id = sqlite3_column_int(st, 0);
        const char *device_id = (const char *) sqlite3_column_text(st, 1);
        for (int ordinal = 1; ordinal <= MAX_CHANNELS_PER_CLIENT; ordinal++) {
          char channel_id[32];
          if (gb_channel_make_id(device_id, ordinal, channel_id, sizeof(channel_id)) != 0) continue;
          sqlite3_reset(up);
          sqlite3_clear_bindings(up);
          sqlite3_bind_text(up, 1, channel_id, -1, SQLITE_TRANSIENT);
          sqlite3_bind_int(up, 2, client_id);
          sqlite3_bind_int(up, 3, ordinal);
          (void) sqlite3_step(up);
        }
      }
    }
    sqlite3_finalize(st);
    sqlite3_finalize(up);
  }
  return sql_exec(*db,
                  "INSERT OR IGNORE INTO video_config(id,codec,resolution,fps,rc_mode,bitrate_kbps,gop,"
                  "iframe_interval,low_latency,prefer_hardware,updated_at) VALUES"
                  "(1,'H264','1920x1080',25,'CBR',4096,75,3,1,0,strftime('%s','now'));"
                  "INSERT OR IGNORE INTO audio_config VALUES"
                  "(1,1,'G711A',8000,64,strftime('%s','now'));"
                  "INSERT OR IGNORE INTO device_source_config VALUES"
                  "(1,'device','','','data/Big_Buck_Bunny_720_10s_2MB.mp4',"
                  "'1280x720',1600,1,'realtime',strftime('%s','now'));"
                  "INSERT OR IGNORE INTO gb_clients(id,enabled,name,server_ip,sip_port,sip_id,device_id,"
                  "username,password,transport,media_proto,register_interval,heartbeat_interval,updated_at) VALUES"
                  "(1,0,'','',0,'','','','','UDP','RTC',0,0,strftime('%s','now')),"
                  "(2,0,'','',0,'','','','','UDP','RTC',0,0,strftime('%s','now'));"
                  "INSERT OR IGNORE INTO console_auth(id,username,password,updated_at) "
                  "VALUES(1,'admin','anyrtc',strftime('%s','now'));"
                  "INSERT OR IGNORE INTO channel_source_config(channel_id,source_profile,source_mode,"
                  "video_device,audio_device,media_file,resolution,bitrate_kbps,file_loop,file_pacing,updated_at) "
                  "SELECT id,'none','device','','','data/Big_Buck_Bunny_720_10s_2MB.mp4',"
                  "'1280x720',1600,1,'realtime',strftime('%s','now') FROM channels;"
                  "INSERT OR IGNORE INTO channel_av_config(channel_id,video_codec,resolution,fps,rc_mode,"
                  "video_bitrate_kbps,gop,iframe_interval,low_latency,prefer_hardware,audio_enabled,"
                  "audio_codec,sample_rate,audio_bitrate_kbps,updated_at) "
                  "SELECT id,'H264','1920x1080',25,'CBR',4096,75,3,1,0,1,'G711A',8000,64,"
                  "strftime('%s','now') FROM channels;");
}

int gb_config_load(sqlite3 *db, gb_app_config_t *config) {
  sqlite3_stmt *st = NULL;
  reset_config(config);
  if (sqlite3_prepare_v2(db, "SELECT id,enabled,name,server_ip,sip_port,sip_id,device_id,"
                             "username,password,transport,media_proto,register_interval,heartbeat_interval "
                             "FROM gb_clients ORDER BY id", -1, &st, NULL) != SQLITE_OK) return -1;
  while (sqlite3_step(st) == SQLITE_ROW) {
    int idx = sqlite3_column_int(st, 0) - 1;
    if (idx < 0 || idx >= MAX_PLATFORMS) continue;
    platform_cfg_t *p = &config->platforms[idx];
    p->id = idx + 1;
    p->enabled = sqlite3_column_int(st, 1);
    snprintf(p->name, sizeof(p->name), "%s", sqlite3_column_text(st, 2));
    snprintf(p->server_ip, sizeof(p->server_ip), "%s", sqlite3_column_text(st, 3));
    p->sip_port = sqlite3_column_int(st, 4);
    snprintf(p->sip_id, sizeof(p->sip_id), "%s", sqlite3_column_text(st, 5));
    snprintf(p->device_id, sizeof(p->device_id), "%s", sqlite3_column_text(st, 6));
    snprintf(p->username, sizeof(p->username), "%s", sqlite3_column_text(st, 7));
    snprintf(p->password, sizeof(p->password), "%s", sqlite3_column_text(st, 8));
    snprintf(p->transport, sizeof(p->transport), "%s", sqlite3_column_text(st, 9));
    snprintf(p->media_proto, sizeof(p->media_proto), "%s",
             sqlite3_column_text(st, 10) ? (const char *) sqlite3_column_text(st, 10) : "RTC");
    p->register_interval = sqlite3_column_int(st, 11);
    p->heartbeat_interval = sqlite3_column_int(st, 12);
  }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(db, "SELECT c.id,sc.source_profile,sc.source_mode,sc.video_device,sc.audio_device,"
                             "sc.media_file,sc.resolution,sc.bitrate_kbps,sc.file_loop,sc.file_pacing "
                             "FROM channels c "
                             "LEFT JOIN channel_source_config sc ON sc.channel_id=c.id "
                             "ORDER BY c.id", -1, &st, NULL) != SQLITE_OK) return -1;
  while (sqlite3_step(st) == SQLITE_ROW) {
    int idx = sqlite3_column_int(st, 0) - 1;
    if (idx < 0 || idx >= MAX_CHANNELS) continue;
    snprintf(config->channel_source_profile[idx], sizeof(config->channel_source_profile[idx]), "%s",
             sqlite3_column_text(st, 1) ? (const char *) sqlite3_column_text(st, 1) : "none");
    snprintf(config->channel_sources[idx].source_mode, sizeof(config->channel_sources[idx].source_mode), "%s",
             sqlite3_column_text(st, 2) ? (const char *) sqlite3_column_text(st, 2) : "device");
    snprintf(config->channel_sources[idx].video_device, sizeof(config->channel_sources[idx].video_device), "%s",
             sqlite3_column_text(st, 3) ? (const char *) sqlite3_column_text(st, 3) : "");
    snprintf(config->channel_sources[idx].audio_device, sizeof(config->channel_sources[idx].audio_device), "%s",
             sqlite3_column_text(st, 4) ? (const char *) sqlite3_column_text(st, 4) : "");
    snprintf(config->channel_sources[idx].media_file, sizeof(config->channel_sources[idx].media_file), "%s",
             sqlite3_column_text(st, 5) ? (const char *) sqlite3_column_text(st, 5) : "");
    snprintf(config->channel_sources[idx].resolution, sizeof(config->channel_sources[idx].resolution), "%s",
             sqlite3_column_text(st, 6) ? (const char *) sqlite3_column_text(st, 6) : "");
    config->channel_sources[idx].bitrate_kbps = sqlite3_column_int(st, 7);
    config->channel_sources[idx].file_loop = sqlite3_column_int(st, 8);
    snprintf(config->channel_sources[idx].file_pacing, sizeof(config->channel_sources[idx].file_pacing), "%s",
             sqlite3_column_text(st, 9) ? (const char *) sqlite3_column_text(st, 9) : "realtime");
    config->channel_count++;
  }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(db,
                         "SELECT id,client_id,ordinal,channel_id,name,media_proto "
                         "FROM gb_client_channels ORDER BY client_id,ordinal",
                         -1,
                         &st,
                         NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      int idx = sqlite3_column_int(st, 0) - 1;
      if (idx < 0 || idx >= MAX_CHANNELS) continue;
      gb_channel_cfg_t *ch = &config->gb_channels[idx];
      ch->id = idx + 1;
      ch->client_id = sqlite3_column_int(st, 1);
      ch->ordinal = sqlite3_column_int(st, 2);
      snprintf(ch->channel_id, sizeof(ch->channel_id), "%s",
               sqlite3_column_text(st, 3) ? (const char *) sqlite3_column_text(st, 3) : "");
      snprintf(ch->name, sizeof(ch->name), "%s",
               sqlite3_column_text(st, 4) ? (const char *) sqlite3_column_text(st, 4) : "");
      snprintf(ch->media_proto, sizeof(ch->media_proto), "%s",
               sqlite3_column_text(st, 5) ? (const char *) sqlite3_column_text(st, 5) : "RTC");
      if (ch->channel_id[0] == '\0') {
        int client_idx = ch->client_id >= 1 && ch->client_id <= MAX_PLATFORMS ? ch->client_id - 1 : 0;
        (void) gb_channel_make_id(config->platforms[client_idx].device_id,
                                  ch->ordinal,
                                  ch->channel_id,
                                  sizeof(ch->channel_id));
      }
    }
  }
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db, "SELECT codec,resolution,fps,rc_mode,bitrate_kbps,"
                         "gop,iframe_interval,low_latency,prefer_hardware "
                         "FROM video_config WHERE id=1", -1, &st, NULL);
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(config->video.codec, sizeof(config->video.codec), "%s", sqlite3_column_text(st, 0));
    snprintf(config->video.resolution, sizeof(config->video.resolution), "%s", sqlite3_column_text(st, 1));
    config->video.fps = sqlite3_column_int(st, 2);
    snprintf(config->video.rc_mode, sizeof(config->video.rc_mode), "%s", sqlite3_column_text(st, 3));
    config->video.bitrate_kbps = sqlite3_column_int(st, 4);
    config->video.gop = sqlite3_column_int(st, 5);
    config->video.iframe_interval = sqlite3_column_int(st, 6);
    config->video.low_latency = sqlite3_column_int(st, 7);
    config->video.prefer_hardware = sqlite3_column_int(st, 8);
  }
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db, "SELECT enabled,codec,sample_rate,bitrate_kbps FROM audio_config WHERE id=1", -1, &st, NULL);
  if (sqlite3_step(st) == SQLITE_ROW) {
    config->audio.enabled = sqlite3_column_int(st, 0);
    snprintf(config->audio.codec, sizeof(config->audio.codec), "%s", sqlite3_column_text(st, 1));
    config->audio.sample_rate = sqlite3_column_int(st, 2);
    config->audio.bitrate_kbps = sqlite3_column_int(st, 3);
  }
  sqlite3_finalize(st);

  for (int i = 0; i < MAX_CHANNELS; i++) {
    config->channel_videos[i] = config->video;
    config->channel_audios[i] = config->audio;
  }
  if (sqlite3_prepare_v2(db, "SELECT channel_id,video_codec,resolution,fps,rc_mode,video_bitrate_kbps,"
                             "gop,iframe_interval,low_latency,prefer_hardware,audio_enabled,audio_codec,"
                             "sample_rate,audio_bitrate_kbps FROM channel_av_config",
                         -1, &st, NULL) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      int idx = sqlite3_column_int(st, 0) - 1;
      if (idx < 0 || idx >= MAX_CHANNELS) continue;
      snprintf(config->channel_videos[idx].codec, sizeof(config->channel_videos[idx].codec), "%s",
               sqlite3_column_text(st, 1) ? (const char *) sqlite3_column_text(st, 1) : "H264");
      snprintf(config->channel_videos[idx].resolution, sizeof(config->channel_videos[idx].resolution), "%s",
               sqlite3_column_text(st, 2) ? (const char *) sqlite3_column_text(st, 2) : "1920x1080");
      config->channel_videos[idx].fps = sqlite3_column_int(st, 3);
      snprintf(config->channel_videos[idx].rc_mode, sizeof(config->channel_videos[idx].rc_mode), "%s",
               sqlite3_column_text(st, 4) ? (const char *) sqlite3_column_text(st, 4) : "CBR");
      config->channel_videos[idx].bitrate_kbps = sqlite3_column_int(st, 5);
      config->channel_videos[idx].gop = sqlite3_column_int(st, 6);
      config->channel_videos[idx].iframe_interval = sqlite3_column_int(st, 7);
      config->channel_videos[idx].low_latency = sqlite3_column_int(st, 8);
      config->channel_videos[idx].prefer_hardware = sqlite3_column_int(st, 9);
      config->channel_audios[idx].enabled = sqlite3_column_int(st, 10);
      snprintf(config->channel_audios[idx].codec, sizeof(config->channel_audios[idx].codec), "%s",
               sqlite3_column_text(st, 11) ? (const char *) sqlite3_column_text(st, 11) : "G711A");
      config->channel_audios[idx].sample_rate = sqlite3_column_int(st, 12);
      config->channel_audios[idx].bitrate_kbps = sqlite3_column_int(st, 13);
    }
  }
  sqlite3_finalize(st);

  sqlite3_prepare_v2(db, "SELECT source_mode,video_device,audio_device,media_file,"
                         "resolution,bitrate_kbps,file_loop,file_pacing "
                         "FROM device_source_config WHERE id=1", -1, &st, NULL);
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(config->device.source_mode, sizeof(config->device.source_mode), "%s", sqlite3_column_text(st, 0));
    snprintf(config->device.video_device, sizeof(config->device.video_device), "%s", sqlite3_column_text(st, 1));
    snprintf(config->device.audio_device, sizeof(config->device.audio_device), "%s", sqlite3_column_text(st, 2));
    snprintf(config->device.media_file, sizeof(config->device.media_file), "%s", sqlite3_column_text(st, 3));
    snprintf(config->device.resolution, sizeof(config->device.resolution), "%s", sqlite3_column_text(st, 4));
    config->device.bitrate_kbps = sqlite3_column_int(st, 5);
    config->device.file_loop = sqlite3_column_int(st, 6);
    snprintf(config->device.file_pacing, sizeof(config->device.file_pacing), "%s", sqlite3_column_text(st, 7));
  }
  sqlite3_finalize(st);
  return 0;
}

int gb_config_channel_exists(sqlite3 *db, int id) {
  sqlite3_stmt *st = NULL;
  int exists = 0;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM channels WHERE id=?", -1, &st, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_int(st, 1, id);
  exists = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return exists;
}

int gb_config_client_channel_count(sqlite3 *db, int client_id) {
  sqlite3_stmt *st = NULL;
  int count = 0;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM gb_client_channels WHERE client_id=?", -1, &st, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_int(st, 1, client_id);
  if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return count;
}

static int gb_config_update_client_channel_ids(sqlite3 *db, int client_id, const char *device_id) {
  sqlite3_stmt *st = NULL;
  if (db == NULL || client_id < 1 || client_id > MAX_GB_CLIENTS) return -1;
  if (device_id == NULL || strlen(device_id) < 14) return -1;
  if (sqlite3_prepare_v2(db,
                         "UPDATE gb_client_channels SET channel_id=?,updated_at=strftime('%s','now') "
                         "WHERE client_id=? AND ordinal=?",
                         -1,
                         &st,
                         NULL) != SQLITE_OK) return -1;
  for (int i = 1; i <= MAX_CHANNELS_PER_CLIENT; i++) {
    char channel_id[32] = {0};
    if (gb_channel_make_id(device_id, i, channel_id, sizeof(channel_id)) != 0) {
      sqlite3_finalize(st);
      return -1;
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, channel_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, client_id);
    sqlite3_bind_int(st, 3, i);
    if (sqlite3_step(st) != SQLITE_DONE) {
      sqlite3_finalize(st);
      return -1;
    }
  }
  sqlite3_finalize(st);
  return 0;
}

int gb_config_save_client(sqlite3 *db, int client_id, const platform_cfg_t *p, int *created_channel_id) {
  sqlite3_stmt *st = NULL;
  if (db == NULL || p == NULL || client_id < 1 || client_id > MAX_GB_CLIENTS) return -1;
  if (strlen(p->device_id) < 14) return -1;
  if (created_channel_id != NULL) *created_channel_id = 0;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO gb_clients(id,enabled,name,server_ip,sip_port,sip_id,device_id,"
                         "username,password,transport,media_proto,register_interval,heartbeat_interval,updated_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,strftime('%s','now')) "
                         "ON CONFLICT(id) DO UPDATE SET enabled=excluded.enabled,"
                         "server_ip=excluded.server_ip,sip_port=excluded.sip_port,sip_id=excluded.sip_id,"
                         "device_id=excluded.device_id,username=excluded.username,password=excluded.password,"
                         "transport=excluded.transport,media_proto=excluded.media_proto,"
                         "register_interval=excluded.register_interval,heartbeat_interval=excluded.heartbeat_interval,"
                         "updated_at=strftime('%s','now')",
                         -1,
                         &st,
                         NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, client_id);
  sqlite3_bind_int(st, 2, p->enabled);
  sqlite3_bind_text(st, 3, "", -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, p->server_ip, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, p->sip_port);
  sqlite3_bind_text(st, 6, p->sip_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, p->device_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, p->username, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 9, p->password, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 10, p->transport, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 11, p->media_proto, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 12, p->register_interval);
  sqlite3_bind_int(st, 13, p->heartbeat_interval);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);

  if (gb_config_client_channel_count(db, client_id) == 0) {
    int id = 0;
    int rc = gb_config_create_client_channel(db, client_id, &id);
    if (rc != 0) return -1;
    if (created_channel_id != NULL) *created_channel_id = id;
  }
  if (gb_config_update_client_channel_ids(db, client_id, p->device_id) != 0) return -1;
  return 0;
}

int gb_config_save_channel(sqlite3 *db,
                           int id,
                           const platform_cfg_t *p,
                           const device_source_cfg_t *source,
                           const char *source_profile) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db,
                         "INSERT OR IGNORE INTO channels(id,enabled,name,updated_at) "
                         "VALUES(?,?,?,strftime('%s','now'))",
                         -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_bind_int(st, 2, p->enabled);
  sqlite3_bind_text(st, 3, p->name, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(db,
                         "UPDATE channels SET enabled=?,name=?,updated_at=strftime('%s','now') WHERE id=?",
                         -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, p->enabled);
  sqlite3_bind_text(st, 2, p->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, id);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);

  int client_id = (id - 1) / MAX_CHANNELS_PER_CLIENT + 1;
  int ordinal = (id - 1) % MAX_CHANNELS_PER_CLIENT + 1;
  char generated_channel_id[32] = {0};
  char client_media_proto[8];
  if (gb_channel_make_id(p->device_id, ordinal, generated_channel_id, sizeof(generated_channel_id)) != 0) return -1;
  snprintf(client_media_proto, sizeof(client_media_proto), "%s", p->media_proto[0] ? p->media_proto : "RTC");
  if (ordinal != 1 &&
      sqlite3_prepare_v2(db, "SELECT media_proto FROM gb_clients WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int(st, 1, client_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
      snprintf(client_media_proto,
               sizeof(client_media_proto),
               "%s",
               sqlite3_column_text(st, 0) ? (const char *) sqlite3_column_text(st, 0) : client_media_proto);
    }
    sqlite3_finalize(st);
    st = NULL;
  }

  if (sqlite3_prepare_v2(db,
                         "INSERT INTO gb_clients(id,enabled,name,server_ip,sip_port,sip_id,device_id,"
                         "username,password,transport,media_proto,register_interval,heartbeat_interval,updated_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,strftime('%s','now')) "
                         "ON CONFLICT(id) DO UPDATE SET enabled=excluded.enabled,"
                         "server_ip=excluded.server_ip,sip_port=excluded.sip_port,sip_id=excluded.sip_id,"
                         "device_id=excluded.device_id,username=excluded.username,password=excluded.password,"
                         "transport=excluded.transport,media_proto=excluded.media_proto,"
                         "register_interval=excluded.register_interval,heartbeat_interval=excluded.heartbeat_interval,"
                         "updated_at=strftime('%s','now')",
                         -1,
                         &st,
                         NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, client_id);
  sqlite3_bind_int(st, 2, p->enabled);
  sqlite3_bind_text(st, 3, "", -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, p->server_ip, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, p->sip_port);
  sqlite3_bind_text(st, 6, p->sip_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, p->device_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, p->username, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 9, p->password, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 10, p->transport, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 11, client_media_proto, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 12, p->register_interval);
  sqlite3_bind_int(st, 13, p->heartbeat_interval);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(db,
                         "INSERT INTO gb_client_channels(id,client_id,ordinal,channel_id,name,media_proto,updated_at) "
                         "VALUES(?,?,?,?,?,?,strftime('%s','now')) "
                         "ON CONFLICT(id) DO UPDATE SET channel_id=excluded.channel_id,name=excluded.name,"
                         "media_proto=excluded.media_proto,updated_at=strftime('%s','now')",
                         -1,
                         &st,
                         NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_bind_int(st, 2, client_id);
  sqlite3_bind_int(st, 3, ordinal);
  sqlite3_bind_text(st, 4, generated_channel_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, p->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, p->media_proto, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);

  if (gb_config_update_client_channel_ids(db, client_id, p->device_id) != 0) return -1;

  if (sqlite3_prepare_v2(db,
                         "INSERT INTO channel_platform_config(channel_id,server_ip,sip_port,sip_id,"
                         "device_id,username,password,transport,media_proto,register_interval,heartbeat_interval,updated_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,strftime('%s','now')) "
                         "ON CONFLICT(channel_id) DO UPDATE SET server_ip=excluded.server_ip,"
                         "sip_port=excluded.sip_port,sip_id=excluded.sip_id,device_id=excluded.device_id,"
                         "username=excluded.username,password=excluded.password,transport=excluded.transport,"
                         "media_proto=excluded.media_proto,"
                         "register_interval=excluded.register_interval,heartbeat_interval=excluded.heartbeat_interval,"
                         "updated_at=strftime('%s','now')",
                         -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_bind_text(st, 2, p->server_ip, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, p->sip_port);
  sqlite3_bind_text(st, 4, p->sip_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, p->device_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, p->username, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, p->password, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, p->transport, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 9, p->media_proto, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 10, p->register_interval);
  sqlite3_bind_int(st, 11, p->heartbeat_interval);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(db,
                         "INSERT INTO channel_source_config(channel_id,source_profile,source_mode,video_device,"
                         "audio_device,media_file,resolution,bitrate_kbps,file_loop,file_pacing,updated_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,strftime('%s','now')) "
                         "ON CONFLICT(channel_id) DO UPDATE SET source_profile=excluded.source_profile,"
                         "source_mode=excluded.source_mode,video_device=excluded.video_device,"
                         "audio_device=excluded.audio_device,media_file=excluded.media_file,"
                         "resolution=excluded.resolution,bitrate_kbps=excluded.bitrate_kbps,"
                         "file_loop=excluded.file_loop,file_pacing=excluded.file_pacing,"
                         "updated_at=strftime('%s','now')",
                         -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_bind_text(st, 2, source_profile, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, source->source_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, source->video_device, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, source->audio_device, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, source->media_file, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, source->resolution, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 8, source->bitrate_kbps);
  sqlite3_bind_int(st, 9, source->file_loop);
  sqlite3_bind_text(st, 10, source->file_pacing, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    return -1;
  }
  sqlite3_finalize(st);
  return 0;
}

int gb_config_create_channel(sqlite3 *db, int *created_id) {
  return gb_config_create_client_channel(db, 1, created_id);
}

int gb_config_create_client_channel(sqlite3 *db, int client_id, int *created_id) {
  int id = 0;
  if (client_id < 1 || client_id > MAX_GB_CLIENTS) return -1;
  for (int ordinal = 1; ordinal <= MAX_CHANNELS_PER_CLIENT; ordinal++) {
    int candidate = (client_id - 1) * MAX_CHANNELS_PER_CLIENT + ordinal;
    if (!gb_config_channel_exists(db, candidate)) {
      id = candidate;
      break;
    }
  }
  if (id == 0) return 1;
  platform_cfg_t platform;
  device_source_cfg_t source;
  init_platform_default(&platform, id);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "SELECT enabled,server_ip,sip_port,sip_id,device_id,username,password,"
                             "transport,media_proto,register_interval,heartbeat_interval "
                             "FROM gb_clients WHERE id=?",
                         -1,
                         &st,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_int(st, 1, client_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
      platform.enabled = sqlite3_column_int(st, 0);
      snprintf(platform.server_ip, sizeof(platform.server_ip), "%s",
               sqlite3_column_text(st, 1) ? (const char *) sqlite3_column_text(st, 1) : "");
      platform.sip_port = sqlite3_column_int(st, 2);
      snprintf(platform.sip_id, sizeof(platform.sip_id), "%s",
               sqlite3_column_text(st, 3) ? (const char *) sqlite3_column_text(st, 3) : "");
      snprintf(platform.device_id, sizeof(platform.device_id), "%s",
               sqlite3_column_text(st, 4) ? (const char *) sqlite3_column_text(st, 4) : "");
      snprintf(platform.username, sizeof(platform.username), "%s",
               sqlite3_column_text(st, 5) ? (const char *) sqlite3_column_text(st, 5) : "");
      snprintf(platform.password, sizeof(platform.password), "%s",
               sqlite3_column_text(st, 6) ? (const char *) sqlite3_column_text(st, 6) : "");
      snprintf(platform.transport, sizeof(platform.transport), "%s",
               sqlite3_column_text(st, 7) ? (const char *) sqlite3_column_text(st, 7) : "UDP");
      snprintf(platform.media_proto, sizeof(platform.media_proto), "%s",
               sqlite3_column_text(st, 8) ? (const char *) sqlite3_column_text(st, 8) : "RTC");
      platform.register_interval = sqlite3_column_int(st, 9);
      platform.heartbeat_interval = sqlite3_column_int(st, 10);
    }
  }
  sqlite3_finalize(st);
  snprintf(platform.name, sizeof(platform.name), "CH%d", (id - 1) % MAX_CHANNELS_PER_CLIENT + 1);
  init_device_source_default(&source);
  if (gb_config_save_channel(db, id, &platform, &source, "none") != 0) return -1;
  video_cfg_t video;
  audio_cfg_t audio;
  init_video_default(&video);
  init_audio_default(&audio);
  if (gb_config_save_channel_av(db, id, &video, &audio) != 0) return -1;
  if (created_id) *created_id = id;
  return 0;
}

int gb_config_delete_channel(sqlite3 *db, int id) {
  sqlite3_stmt *st = NULL;
  int count = 0;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM channels", -1, &st, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  if (count <= 1) return 1;

  if (sqlite3_prepare_v2(db, "DELETE FROM channel_source_config WHERE channel_id=?", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_step(st);
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db, "DELETE FROM channel_av_config WHERE channel_id=?", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_step(st);
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db, "DELETE FROM channel_platform_config WHERE channel_id=?", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_step(st);
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db, "DELETE FROM gb_client_channels WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_step(st);
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db, "DELETE FROM channels WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, id);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return 0;
}

int gb_config_save_av(sqlite3 *db, const video_cfg_t *v, const audio_cfg_t *a) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "UPDATE video_config SET codec=?,resolution=?,fps=?,"
                             "rc_mode=?,bitrate_kbps=?,gop=?,iframe_interval=?,"
                             "low_latency=?,prefer_hardware=?,updated_at=strftime('%s','now') "
                             "WHERE id=1", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, v->codec, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, v->resolution, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, v->fps);
  sqlite3_bind_text(st, 4, v->rc_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, v->bitrate_kbps);
  sqlite3_bind_int(st, 6, v->gop);
  sqlite3_bind_int(st, 7, v->iframe_interval);
  sqlite3_bind_int(st, 8, v->low_latency);
  sqlite3_bind_int(st, 9, v->prefer_hardware);
  sqlite3_step(st);
  sqlite3_finalize(st);
  if (sqlite3_prepare_v2(db, "UPDATE audio_config SET enabled=?,codec=?,"
                             "sample_rate=?,bitrate_kbps=?,updated_at=strftime('%s','now') "
                             "WHERE id=1", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, a->enabled);
  sqlite3_bind_text(st, 2, a->codec, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, a->sample_rate);
  sqlite3_bind_int(st, 4, a->bitrate_kbps);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return 0;
}

int gb_config_save_channel_av(sqlite3 *db, int channel_id, const video_cfg_t *v, const audio_cfg_t *a) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO channel_av_config(channel_id,video_codec,resolution,fps,rc_mode,"
                         "video_bitrate_kbps,gop,iframe_interval,low_latency,prefer_hardware,audio_enabled,"
                         "audio_codec,sample_rate,audio_bitrate_kbps,updated_at) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,strftime('%s','now')) "
                         "ON CONFLICT(channel_id) DO UPDATE SET video_codec=excluded.video_codec,"
                         "resolution=excluded.resolution,fps=excluded.fps,rc_mode=excluded.rc_mode,"
                         "video_bitrate_kbps=excluded.video_bitrate_kbps,gop=excluded.gop,"
                         "iframe_interval=excluded.iframe_interval,low_latency=excluded.low_latency,"
                         "prefer_hardware=excluded.prefer_hardware,audio_enabled=excluded.audio_enabled,"
                         "audio_codec=excluded.audio_codec,sample_rate=excluded.sample_rate,"
                         "audio_bitrate_kbps=excluded.audio_bitrate_kbps,updated_at=strftime('%s','now')",
                         -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(st, 1, channel_id);
  sqlite3_bind_text(st, 2, v->codec, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, v->resolution, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, v->fps);
  sqlite3_bind_text(st, 5, v->rc_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 6, v->bitrate_kbps);
  sqlite3_bind_int(st, 7, v->gop);
  sqlite3_bind_int(st, 8, v->iframe_interval);
  sqlite3_bind_int(st, 9, v->low_latency);
  sqlite3_bind_int(st, 10, v->prefer_hardware);
  sqlite3_bind_int(st, 11, a->enabled);
  sqlite3_bind_text(st, 12, a->codec, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 13, a->sample_rate);
  sqlite3_bind_int(st, 14, a->bitrate_kbps);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return 0;
}

int gb_config_save_device(sqlite3 *db, const device_source_cfg_t *d) {
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, "UPDATE device_source_config SET source_mode=?,video_device=?,"
                             "audio_device=?,media_file=?,resolution=?,bitrate_kbps=?,"
                             "file_loop=?,file_pacing=?,updated_at=strftime('%s','now') "
                             "WHERE id=1", -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, d->source_mode, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, d->video_device, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, d->audio_device, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, d->media_file, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, d->resolution, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 6, d->bitrate_kbps);
  sqlite3_bind_int(st, 7, d->file_loop);
  sqlite3_bind_text(st, 8, d->file_pacing, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return 0;
}

int gb_config_load_auth(sqlite3 *db, console_auth_cfg_t *auth) {
  sqlite3_stmt *st = NULL;
  if (db == NULL || auth == NULL) return -1;
  memset(auth, 0, sizeof(*auth));
  if (sqlite3_prepare_v2(db, "SELECT username,password FROM console_auth WHERE id=1", -1, &st, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(auth->username, sizeof(auth->username), "%s", sqlite3_column_text(st, 0));
    snprintf(auth->password, sizeof(auth->password), "%s", sqlite3_column_text(st, 1));
  }
  sqlite3_finalize(st);
  if (auth->username[0] == '\0') snprintf(auth->username, sizeof(auth->username), "%s", "admin");
  if (auth->password[0] == '\0') snprintf(auth->password, sizeof(auth->password), "%s", "anyrtc");
  return 0;
}

int gb_config_save_auth(sqlite3 *db, const console_auth_cfg_t *auth) {
  sqlite3_stmt *st = NULL;
  if (db == NULL || auth == NULL || auth->username[0] == '\0' || auth->password[0] == '\0') return -1;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO console_auth(id,username,password,updated_at) "
                         "VALUES(1,?,?,strftime('%s','now')) "
                         "ON CONFLICT(id) DO UPDATE SET username=excluded.username,"
                         "password=excluded.password,updated_at=strftime('%s','now')",
                         -1, &st, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, "admin", -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, auth->password, -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return 0;
}
