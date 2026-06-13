#ifndef C_GB28181_API_TYPES_H
#define C_GB28181_API_TYPES_H

#include "c_gb28181_base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct c_gb28181_api c_gb28181_api_t;

typedef enum c_gb28181_api_state_e {
  C_GB28181_API_STATE_IDLE = 0,
  C_GB28181_API_STATE_STARTING = 1,
  C_GB28181_API_STATE_CONNECTING = 2,
  C_GB28181_API_STATE_REGISTERING = 3,
  C_GB28181_API_STATE_REGISTERED = 4,
  C_GB28181_API_STATE_PUSHING = 5,
  C_GB28181_API_STATE_TALKBACK = 6,
  C_GB28181_API_STATE_STOPPING = 7,
  C_GB28181_API_STATE_STOPPED = 8,
  C_GB28181_API_STATE_ERROR = 9
} c_gb28181_api_state_t;

typedef enum c_gb28181_api_sip_transport_e {
  C_GB28181_API_SIP_TRANSPORT_UDP = 0,
  C_GB28181_API_SIP_TRANSPORT_TCP = 1,
  C_GB28181_API_SIP_TRANSPORT_TRUDP = 2
} c_gb28181_api_sip_transport_t;

typedef enum c_gb28181_api_media_proto_e {
  C_GB28181_API_MEDIA_PROTO_RTP = 0,
  C_GB28181_API_MEDIA_PROTO_RTC = 1
} c_gb28181_api_media_proto_t;

typedef enum c_gb28181_api_pushing_proto_type_e {
  C_GB28181_API_PUSHING_PROTO_STANDARD_GB = 0,
  C_GB28181_API_PUSHING_PROTO_X2RTC = 1
} c_gb28181_api_pushing_proto_type_t;

typedef enum c_gb28181_api_codec_type_e {
  C_GB28181_API_CODEC_NONE = 0,
  C_GB28181_API_CODEC_PS = 1,
  C_GB28181_API_CODEC_PCMA = 2,
  C_GB28181_API_CODEC_PCMU = 3,
  C_GB28181_API_CODEC_OPUS = 4,
  C_GB28181_API_CODEC_H264 = 10,
  C_GB28181_API_CODEC_H265 = 11

} c_gb28181_api_codec_type_t;

typedef enum c_gb28181_api_media_type_e {
  C_GB28181_API_MEDIA_TYPE_AUDIO = 1,
  C_GB28181_API_MEDIA_TYPE_VIDEO = 2
} c_gb28181_api_media_type_t;

typedef struct c_gb28181_api_runtime_options_t {
  const char *server_id;
  const char *server_domain;
  const char *ipc_id;
  const char *ipc_password;
  const char *ipc_ip;
  int ipc_sip_port;
  const char *device_id;
  c_gb28181_api_sip_transport_t transport;
  c_gb28181_api_media_proto_t media_proto;
  int expires;
  int keepalive_interval_ms;
  bool sip_trace_enabled;
} c_gb28181_api_runtime_options_t;

typedef struct c_gb28181_api_video_config_t {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t bitrate_kbps;
} c_gb28181_api_video_config_t;

typedef struct c_gb28181_api_channel_config_t {
  const char *channel_id;
  const char *name;
  c_gb28181_api_media_proto_t media_proto;
  c_gb28181_api_video_config_t video_config;
} c_gb28181_api_channel_config_t;

typedef struct c_gb28181_api_device_info_t {
  const char *server_id;
  const char *server_ip;
  int server_port;
  c_gb28181_api_sip_transport_t sip_transport;
  const char *ipc_id;
  const char *ipc_pwd;
  const char *ipc_ip;
  int ipc_sip_port;
  int ipc_media_port;
  const char *device_name;
  const char *device_manufacturer;
  const char *device_model;
  const char *device_firmware;
  const char *device_encode;
  const char *device_record;
  const char *device_chan_id;
  int ipc_channel_count;
  double longitude;
  double latitude;
  int pushing_proto_type;
} c_gb28181_api_device_info_t;

typedef enum c_gb28181_api_device_control_type_e {
  C_GB28181_API_DEVICE_CONTROL_UNKNOWN = 0,
  C_GB28181_API_DEVICE_CONTROL_PTZ_LEFT = 1,
  C_GB28181_API_DEVICE_CONTROL_PTZ_RIGHT = 2,
  C_GB28181_API_DEVICE_CONTROL_PTZ_UP = 3,
  C_GB28181_API_DEVICE_CONTROL_PTZ_DOWN = 4,
  C_GB28181_API_DEVICE_CONTROL_PTZ_ZOOM_IN = 5,
  C_GB28181_API_DEVICE_CONTROL_PTZ_ZOOM_OUT = 6,
  C_GB28181_API_DEVICE_CONTROL_PTZ_STOP = 7,
  C_GB28181_API_DEVICE_CONTROL_RECORD_START = 8,
  C_GB28181_API_DEVICE_CONTROL_RECORD_STOP = 9,
  C_GB28181_API_DEVICE_CONTROL_GUARD_SET = 10,
  C_GB28181_API_DEVICE_CONTROL_GUARD_RESET = 11,
  C_GB28181_API_DEVICE_CONTROL_ALARM_RESET = 12,
  C_GB28181_API_DEVICE_CONTROL_TELEBOOT = 13
} c_gb28181_api_device_control_type_t;

typedef struct c_gb28181_api_record_info_request_t {
  const char *device_id;
  const char *file_path;
  const char *start_time;
  const char *end_time;
  const char *recorder_id;
} c_gb28181_api_record_info_request_t;

typedef struct c_gb28181_api_record_item_t {
  const char *device_id;
  const char *name;
  const char *file_path;
  const char *address;
  const char *start_time;
  const char *end_time;
  int secrecy;
  const char *type;
  const char *recorder_id;
} c_gb28181_api_record_item_t;

typedef struct c_gb28181_api_push_config_t {
  const char *channel_id;
  const char *target_id;
  const char *session_name;
  c_gb28181_api_media_proto_t proto;
  const char *remote_ip;
  int remote_port;
  uint32_t ssrc;
  uint32_t payload_type;
  uint32_t clock_rate;
  c_gb28181_api_codec_type_t codec;
  c_gb28181_api_codec_type_t audio_codec;
  c_gb28181_api_video_config_t video_config;
} c_gb28181_api_push_config_t;

typedef struct c_gb28181_api_talkback_config_t {
  bool auto_answer;
  const char *local_ip;
  int local_port;
  uint32_t payload_type;
  uint32_t clock_rate;
  c_gb28181_api_codec_type_t codec;
  uint32_t invite_timeout_ms;
} c_gb28181_api_talkback_config_t;

typedef struct c_gb28181_api_frame_t {
  c_gb28181_api_media_type_t media_type;
  const uint8_t *data;
  uint32_t size;
  uint32_t timestamp;
  bool key_frame;
} c_gb28181_api_frame_t;

typedef struct c_gb28181_api_config_t {
  const char *server_ip;
  int server_port;
  c_gb28181_api_runtime_options_t runtime_options;
  int connection_timeout_ms;
  c_gb28181_api_device_info_t device_info;
  const c_gb28181_api_channel_config_t *channels;
  int channel_count;
} c_gb28181_api_config_t;

typedef struct c_gb28181_api_callbacks_t {
  void *user_data;
  void (*on_state)(void *user_data,
                   c_gb28181_api_t *api,
                   c_gb28181_api_state_t old_state,
                   c_gb28181_api_state_t new_state,
                   const char *reason);
  void (*on_registered)(void *user_data,
                        c_gb28181_api_t *api,
                        bool ok,
                        int code,
                        const char *reason);
  void (*on_keepalive)(void *user_data,
                       c_gb28181_api_t *api,
                       bool ok);
  void (*on_message)(void *user_data,
                     c_gb28181_api_t *api,
                     const char *data,
                     size_t len);
  void (*on_error)(void *user_data,
                   c_gb28181_api_t *api,
                   int code,
                   const char *reason);
  void (*on_push_started)(void *user_data,
                          c_gb28181_api_t *api,
                          const c_gb28181_api_push_config_t *config);
  void (*on_channel_push_started)(void *user_data,
                                  c_gb28181_api_t *api,
                                  const char *channel_id,
                                  const c_gb28181_api_push_config_t *config);
  void (*on_push_stopped)(void *user_data,
                          c_gb28181_api_t *api,
                          const char *reason);
  void (*on_channel_push_stopped)(void *user_data,
                                  c_gb28181_api_t *api,
                                  const char *channel_id,
                                  const char *reason);
  void (*on_talkback_invite)(void *user_data,
                             c_gb28181_api_t *api,
                             uint32_t invite_id,
                             const char *from_id,
                             const char *sdp_body);
  void (*on_channel_talkback_invite)(void *user_data,
                                     c_gb28181_api_t *api,
                                     const char *channel_id,
                                     uint32_t invite_id,
                                     const char *from_id,
                                     const char *sdp_body);
  void (*on_talkback_canceled)(void *user_data,
                               c_gb28181_api_t *api,
                               uint32_t invite_id,
                               const char *reason);
  void (*on_channel_talkback_canceled)(void *user_data,
                                       c_gb28181_api_t *api,
                                       const char *channel_id,
                                       uint32_t invite_id,
                                       const char *reason);
  void (*on_talkback_started)(void *user_data,
                              c_gb28181_api_t *api,
                              const char *remote_ip,
                              int remote_port);
  void (*on_channel_talkback_started)(void *user_data,
                                      c_gb28181_api_t *api,
                                      const char *channel_id,
                                      const char *remote_ip,
                                      int remote_port);
  void (*on_talkback_stopped)(void *user_data,
                              c_gb28181_api_t *api,
                              const char *reason);
  void (*on_channel_talkback_stopped)(void *user_data,
                                      c_gb28181_api_t *api,
                                      const char *channel_id,
                                      const char *reason);
  void (*on_channel_broadcast_started)(void *user_data,
                                       c_gb28181_api_t *api,
                                       const char *channel_id,
                                       const char *remote_ip,
                                       int remote_port);
  void (*on_channel_broadcast_stopped)(void *user_data,
                                       c_gb28181_api_t *api,
                                       const char *channel_id,
                                       const char *reason);
  void (*on_channel_video_bitrate_update)(void *user_data,
                                          c_gb28181_api_t *api,
                                          const char *channel_id,
                                          uint32_t bitrate);
  void (*on_frame)(void *user_data,
                   c_gb28181_api_t *api,
                   const c_gb28181_api_frame_t *frame);
  void (*on_channel_frame)(void *user_data,
                           c_gb28181_api_t *api,
                           const char *channel_id,
                           const c_gb28181_api_frame_t *frame);
  void (*on_sip_message)(void *user_data,
                         c_gb28181_api_t *api,
                         const char *channel_id,
                         const char *cmd_type,
                         const char *body);
  void (*on_device_control)(void *user_data,
                            c_gb28181_api_t *api,
                            const char *channel_id,
                            const char *device_id,
                            c_gb28181_api_device_control_type_t control_type,
                            const char *raw_value);
  int (*on_record_info)(void *user_data,
                        c_gb28181_api_t *api,
                        const char *channel_id,
                        const c_gb28181_api_record_info_request_t *request,
                        uint32_t item_index,
                        c_gb28181_api_record_item_t *item_out);
} c_gb28181_api_callbacks_t;

#ifdef __cplusplus
}
#endif

#endif
