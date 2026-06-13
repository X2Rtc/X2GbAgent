#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <sqlite3.h>
#include <stddef.h>

#ifndef GB_MAX_CHANNELS
#define GB_MAX_CHANNELS 16
#endif

/* Current product layout is fixed at 2 GB clients * 8 channels. Keep any
 * external GB_MAX_CHANNELS definition clamped to the supported schema size. */
#if GB_MAX_CHANNELS != 16
#undef GB_MAX_CHANNELS
#define GB_MAX_CHANNELS 16
#endif

#define GB_STRINGIFY2(x) #x
#define GB_STRINGIFY(x) GB_STRINGIFY2(x)
#define MAX_GB_CLIENTS 2
#define MAX_CHANNELS_PER_CLIENT 8
#define MAX_CHANNELS (MAX_GB_CLIENTS * MAX_CHANNELS_PER_CLIENT)
#define MAX_PLATFORMS MAX_GB_CLIENTS

typedef struct {
  int id;
  int enabled;
  char name[32];
  char server_ip[64];
  int sip_port;
  char sip_id[32];
  char device_id[32];
  char username[32];
  char password[64];
  char transport[8];
  char media_proto[8];
  int register_interval;
  int heartbeat_interval;
} platform_cfg_t;

typedef struct {
  int id;
  int client_id;
  int ordinal;
  char channel_id[32];
  char name[32];
  char media_proto[8];
} gb_channel_cfg_t;

typedef struct {
  char codec[8];
  char resolution[16];
  int fps;
  char rc_mode[4];
  int bitrate_kbps;
  int gop;
  int iframe_interval;
  int low_latency;
  int prefer_hardware;
} video_cfg_t;

typedef struct {
  int enabled;
  char codec[8];
  int sample_rate;
  int bitrate_kbps;
} audio_cfg_t;

typedef struct {
  char source_mode[16];
  char video_device[256];
  char audio_device[256];
  char media_file[256];
  char resolution[16];
  int bitrate_kbps;
  int file_loop;
  char file_pacing[16];
} device_source_cfg_t;

typedef struct {
  char username[64];
  char password[128];
} console_auth_cfg_t;

typedef struct {
  int channel_count;
  platform_cfg_t platforms[MAX_PLATFORMS];
  gb_channel_cfg_t gb_channels[MAX_CHANNELS];
  device_source_cfg_t channel_sources[MAX_CHANNELS];
  char channel_source_profile[MAX_CHANNELS][16];
  video_cfg_t channel_videos[MAX_CHANNELS];
  audio_cfg_t channel_audios[MAX_CHANNELS];
  video_cfg_t video;
  audio_cfg_t audio;
  device_source_cfg_t device;
} gb_app_config_t;

void init_platform_default(platform_cfg_t *p, int id);
void init_gb_channel_default(gb_channel_cfg_t *ch, int id, int client_id, int ordinal, const char *base_device_id);
int gb_channel_make_id(const char *base_device_id, int ordinal, char *out, size_t out_size);
void init_device_source_default(device_source_cfg_t *d);
device_source_cfg_t effective_channel_source(const device_source_cfg_t *global_source,
                                             const device_source_cfg_t *channel_source,
                                             const char *profile);

int gb_config_open(sqlite3 **db, const char *path);
int gb_config_load(sqlite3 *db, gb_app_config_t *config);
int gb_config_channel_exists(sqlite3 *db, int id);
int gb_config_client_channel_count(sqlite3 *db, int client_id);
int gb_config_create_channel(sqlite3 *db, int *created_id);
int gb_config_create_client_channel(sqlite3 *db, int client_id, int *created_id);
int gb_config_delete_channel(sqlite3 *db, int id);
int gb_config_save_client(sqlite3 *db, int client_id, const platform_cfg_t *platform, int *created_channel_id);
int gb_config_save_channel(sqlite3 *db,
                           int id,
                           const platform_cfg_t *platform,
                           const device_source_cfg_t *source,
                           const char *source_profile);
int gb_config_save_av(sqlite3 *db, const video_cfg_t *video, const audio_cfg_t *audio);
int gb_config_save_channel_av(sqlite3 *db, int channel_id, const video_cfg_t *video, const audio_cfg_t *audio);
int gb_config_save_device(sqlite3 *db, const device_source_cfg_t *device);
int gb_config_load_auth(sqlite3 *db, console_auth_cfg_t *auth);
int gb_config_save_auth(sqlite3 *db, const console_auth_cfg_t *auth);

#endif
