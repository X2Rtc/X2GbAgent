#ifndef GB_AGENT_H
#define GB_AGENT_H

#include <stdbool.h>
#include <time.h>

#ifndef GB_MAX_CHANNELS
#define GB_MAX_CHANNELS 16
#endif

#define GB_AGENT_MAX_CLIENTS 2
#define GB_AGENT_MAX_CHANNELS GB_MAX_CHANNELS
#define GB_AGENT_MAX_CHANNELS_PER_CLIENT 8
#define GB_AGENT_MAX_PLATFORMS GB_AGENT_MAX_CLIENTS
#define GB_AGENT_TEXT_LEN 128

typedef struct gb_agent gb_agent_t;

typedef struct {
  int id;
  int enabled;
  char name[GB_AGENT_TEXT_LEN];
  char server_ip[GB_AGENT_TEXT_LEN];
  int sip_port;
  char sip_id[GB_AGENT_TEXT_LEN];
  char device_id[GB_AGENT_TEXT_LEN];
  char username[GB_AGENT_TEXT_LEN];
  char password[GB_AGENT_TEXT_LEN];
  char transport[8];
  char media_proto[8];
  int register_interval;
  int heartbeat_interval;
} gb_agent_platform_t;

typedef struct {
  char source_mode[16];
  char video_device[256];
  char audio_device[256];
  char media_file[256];
  char resolution[32];
  int bitrate_kbps;
  int fps;
  int gop;
  int iframe_interval;
  int loop;
  char file_pacing[16];
} gb_agent_media_source_t;

typedef struct {
  int id;
  int client_id;
  int ordinal;
  char channel_id[32];
  char name[GB_AGENT_TEXT_LEN];
  char media_proto[8];
  int width;
  int height;
  int fps;
  int bitrate_kbps;
} gb_agent_channel_t;

typedef struct {
  int id;
  int desired_enabled;
  int configured;
  int registered;
  int keepalive_ok;
  int keepalive_ok_count;
  int push_active;
  int media_running;
  unsigned long media_generation;
  unsigned long media_frames_encoded;
  unsigned long media_frames_sent;
  unsigned long media_encode_errors;
  int register_code;
  int last_error_code;
  char sdk_state[32];
  char last_reason[GB_AGENT_TEXT_LEN];
  char server_ip[GB_AGENT_TEXT_LEN];
  int server_port;
  char transport[8];
  int local_sip_port;
  time_t started_at;
  time_t updated_at;
} gb_agent_status_t;

typedef struct {
  int id;
  int push_active;
  int talkback_active;
  int broadcast_active;
  int media_running;
  unsigned long media_generation;
  unsigned long media_frames_encoded;
  unsigned long media_frames_sent;
  unsigned long media_encode_errors;
  char channel_id[32];
} gb_agent_channel_status_t;

typedef void (*gb_agent_log_fn)(void *user_data,
                                int platform_id,
                                const char *level,
                                const char *category,
                                const char *message);

typedef struct {
  void *user_data;
  gb_agent_log_fn on_log;
} gb_agent_callbacks_t;

int gb_agent_create(const gb_agent_callbacks_t *callbacks, gb_agent_t **agent_out);
void gb_agent_destroy(gb_agent_t **agent);

int gb_agent_apply_platform(gb_agent_t *agent, const gb_agent_platform_t *platform);
int gb_agent_set_platform_channels(gb_agent_t *agent,
                                   int platform_id,
                                   const gb_agent_channel_t *channels,
                                   int channel_count);
int gb_agent_poll_reconnect(gb_agent_t *agent);
int gb_agent_stop_platform(gb_agent_t *agent, int platform_id);
void gb_agent_stop_all(gb_agent_t *agent);
int gb_agent_set_media_source(gb_agent_t *agent, const gb_agent_media_source_t *source);
int gb_agent_stop_media_source(gb_agent_t *agent);
int gb_agent_get_media_source(gb_agent_t *agent, gb_agent_media_source_t *source_out);
int gb_agent_set_platform_media_source(gb_agent_t *agent, int platform_id, const gb_agent_media_source_t *source);
int gb_agent_stop_platform_media_source(gb_agent_t *agent, int platform_id);
int gb_agent_get_platform_media_source(gb_agent_t *agent, int platform_id, gb_agent_media_source_t *source_out);

int gb_agent_get_status(gb_agent_t *agent, int platform_id, gb_agent_status_t *status_out);
int gb_agent_get_all_status(gb_agent_t *agent,
                            gb_agent_status_t *statuses,
                            int max_statuses);
int gb_agent_get_channel_status(gb_agent_t *agent,
                                int channel_id,
                                gb_agent_channel_status_t *status_out);

#endif
