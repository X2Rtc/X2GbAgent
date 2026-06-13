#ifndef CGB28181_API_H
#define CGB28181_API_H

#include "c_gb28181_types.h"

#ifdef __cplusplus
extern "C" {
#endif

C_GB28181_API int C_GB28181_CALL c_gb28181_api_create(const c_gb28181_api_config_t *config,
                                                       const c_gb28181_api_callbacks_t *callbacks,
                                                       c_gb28181_api_t **api_out);
C_GB28181_API void C_GB28181_CALL c_gb28181_api_destroy(c_gb28181_api_t **api);

C_GB28181_API int C_GB28181_CALL c_gb28181_api_start(c_gb28181_api_t *api);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_stop(c_gb28181_api_t *api);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_send_message(c_gb28181_api_t *api, const char *xml_body);

C_GB28181_API int C_GB28181_CALL c_gb28181_api_start_push(c_gb28181_api_t *api,
                                                           const c_gb28181_api_push_config_t *config);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_stop_push(c_gb28181_api_t *api);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_set_talkback_config(c_gb28181_api_t *api,
                                                                    const c_gb28181_api_talkback_config_t *config);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_accept_talkback(c_gb28181_api_t *api, uint32_t invite_id);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_reject_talkback(c_gb28181_api_t *api,
                                                                uint32_t invite_id,
                                                                int sip_code);
C_GB28181_API int C_GB28181_CALL c_gb28181_api_send_channel_frame(c_gb28181_api_t *api,
                                                                   const char *channel_id,
                                                                   const c_gb28181_api_frame_t *frame);

C_GB28181_API c_gb28181_api_state_t C_GB28181_CALL c_gb28181_api_get_state(const c_gb28181_api_t *api);
C_GB28181_API const c_gb28181_api_config_t *C_GB28181_CALL c_gb28181_api_get_config(const c_gb28181_api_t *api);
C_GB28181_API const c_gb28181_api_device_info_t *C_GB28181_CALL
c_gb28181_api_get_device_info(const c_gb28181_api_t *api);

#ifdef __cplusplus
}
#endif

#endif
