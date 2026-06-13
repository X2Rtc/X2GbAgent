#include "capture_internal.h"
#include "gb_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GBMEDIA_WITH_DV500
#define GBMEDIA_WITH_DV500 0
#endif

#if GBMEDIA_WITH_DV500
#include <fcntl.h>
#include <unistd.h>

#include "ot_common.h"
#include "ot_common_sys.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_common_vpss.h"
#include "ot_mipi_rx.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vpss.h"

td_s32 sample_comm_vi_set_vi_vpss_mode(ot_vi_vpss_mode_type mode_type,
                                       ot_vi_aiisp_mode aiisp_mode);
#endif

#define DV500_CAMERA_ID "dv500://camera0"
#define DV500_VPSS_GRP 0
#define DV500_VPSS_CHN 0

typedef struct {
  gbmc_capture_t base;
  gbmc_capture_config_t config;
  gbmc_capture_callbacks_t callbacks;
  gb_thread_t thread;
  int started;
  int stop_requested;
#if GBMEDIA_WITH_DV500
  sample_vi_cfg vi_cfg;
  sample_comm_cfg sys_cfg;
  sample_vpss_cfg vpss_cfg;
  sample_vpss_chn_attr vpss_chn_attr;
  ot_vi_pipe vi_pipe;
  ot_vi_chn vi_chn;
  ot_vpss_grp vpss_grp;
  int sys_owned;
  int vb_owned;
  int vi_started;
  int vpss_started;
  int vi_vpss_bound;
  int width;
  int height;
  const char *format_name;
#endif
} gbmc_dv500_capture_t;

static int dv500_stop(gbmc_capture_t *capture);

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

#if GBMEDIA_WITH_DV500
static int dv500_ret(td_s32 ret) {
  return ret == TD_SUCCESS ? GBMC_OK : GBMC_ERR_BACKEND;
}

static int dv500_device_exists(void) {
  int fd = open("/dev/ot_mipi_rx", O_RDONLY);
  if (fd < 0) return 0;
  close(fd);
  return 1;
}

static int dv500_parse_env_int(const char *name, int fallback) {
  const char *value = getenv(name);
  char *end = NULL;
  long parsed;
  if (!value || !value[0]) return fallback;
  parsed = strtol(value, &end, 0);
  if (end == value || *end != '\0') return fallback;
  return (int) parsed;
}

static sample_sns_type dv500_sensor_type(void) {
  const char *name = getenv("GB28181_DV500_SENSOR");
  if (name && (strcmp(name, "os08a20") == 0 || strcmp(name, "OS08A20") == 0)) {
    return OV_OS08A20_MIPI_8M_30FPS_12BIT;
  }
  if (name && (strcmp(name, "imx347") == 0 || strcmp(name, "IMX347") == 0)) {
    return SONY_IMX347_SLAVE_MIPI_4M_30FPS_12BIT;
  }
  if (name && (strcmp(name, "imx515") == 0 || strcmp(name, "IMX515") == 0)) {
    return SONY_IMX515_MIPI_8M_30FPS_12BIT;
  }
  return OV_OS04A10_MIPI_4M_30FPS_12BIT;
}

static ot_pixel_format dv500_pixel_format(const char **format_name) {
  const char *value = getenv("GB28181_DV500_PIXEL_FORMAT");
  if (value && (strcmp(value, "yuv") == 0 || strcmp(value, "YUV") == 0 ||
                strcmp(value, "nv12") == 0 || strcmp(value, "NV12") == 0)) {
    if (format_name) *format_name = "nv12";
    return OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
  }
  if (format_name) *format_name = "nv21";
  return OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
}

static int dv500_frame_size(int width, int height) {
  if (width <= 0 || height <= 0) return 0;
  return width * height * 3 / 2;
}

static int dv500_copy_sp420(const ot_video_frame *frame, uint8_t **out, size_t *out_size) {
  int width;
  int height;
  int y_size;
  int uv_size;
  uint8_t *buf;
  uint8_t *dst;
  const uint8_t *y;
  const uint8_t *uv;
  if (!frame || !out || !out_size) return GBMC_ERR_INVALID;
  width = (int) frame->width;
  height = (int) frame->height;
  y_size = width * height;
  uv_size = y_size / 2;
  if (dv500_frame_size(width, height) <= 0 || !frame->virt_addr[0] || !frame->virt_addr[1]) {
    return GBMC_ERR_INVALID;
  }
  buf = (uint8_t *) malloc((size_t) y_size + (size_t) uv_size);
  if (!buf) return GBMC_ERR_NO_MEMORY;
  dst = buf;
  y = (const uint8_t *)(uintptr_t) frame->virt_addr[0];
  uv = (const uint8_t *)(uintptr_t) frame->virt_addr[1];
  for (int row = 0; row < height; row++) {
    memcpy(dst, y + (size_t) row * frame->stride[0], (size_t) width);
    dst += width;
  }
  for (int row = 0; row < height / 2; row++) {
    memcpy(dst, uv + (size_t) row * frame->stride[1], (size_t) width);
    dst += width;
  }
  *out = buf;
  *out_size = (size_t) y_size + (size_t) uv_size;
  return GBMC_OK;
}

static int dv500_start_sys(gbmc_dv500_capture_t *capture,
                           sample_sns_type sensor_type,
                           ot_pixel_format pixel_format) {
  sample_vb_param vb_param;
  td_s32 ret;
  int sensor_bus = dv500_parse_env_int("GB28181_DV500_SENSOR_BUS", 3);
  sample_comm_vi_get_default_vi_cfg(sensor_type, &capture->vi_cfg);
  sample_comm_vi_get_size_by_sns_type(sensor_type, &capture->sys_cfg.in_size);
  sample_comm_sys_get_default_cfg(1, &capture->sys_cfg);
  capture->vi_cfg.sns_info.bus_id = (td_s8) sensor_bus;
  capture->vi_cfg.pipe_info[0].chn_info[0].chn_attr.mirror_en = TD_TRUE;
  capture->vi_cfg.pipe_info[0].chn_info[0].chn_attr.flip_en = TD_TRUE;
  capture->sys_cfg.vi_cfg = capture->vi_cfg;
  capture->vpss_grp = DV500_VPSS_GRP;
  capture->vi_pipe = capture->sys_cfg.vi_pipe;
  capture->vi_chn = capture->sys_cfg.vi_chn;
  capture->sys_cfg.vpss_grp[0] = capture->vpss_grp;
  capture->sys_cfg.chn_attr[DV500_VPSS_CHN].width = (td_u32) capture->width;
  capture->sys_cfg.chn_attr[DV500_VPSS_CHN].height = (td_u32) capture->height;
  capture->sys_cfg.chn_attr[DV500_VPSS_CHN].pixel_format = pixel_format;
  capture->sys_cfg.chn_attr[DV500_VPSS_CHN].compress_mode = OT_COMPRESS_MODE_NONE;
  memset(&vb_param, 0, sizeof(vb_param));
  vb_param.vb_size = capture->sys_cfg.in_size;
  vb_param.pixel_format[0] = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
  vb_param.pixel_format[1] = pixel_format;
  vb_param.compress_mode[0] = OT_COMPRESS_MODE_NONE;
  vb_param.compress_mode[1] = OT_COMPRESS_MODE_NONE;
  vb_param.video_format[0] = OT_VIDEO_FORMAT_LINEAR;
  vb_param.video_format[1] = OT_VIDEO_FORMAT_LINEAR;
  sample_comm_sys_get_default_vb_cfg(&vb_param, &capture->sys_cfg.vb_cfg);
  ss_mpi_sys_exit();
  ss_mpi_vb_exit();
  ret = ss_mpi_vb_set_cfg(&capture->sys_cfg.vb_cfg);
  if (ret == TD_SUCCESS) capture->vb_owned = 1;
  ret = ss_mpi_vb_init();
  if (ret == TD_SUCCESS) capture->vb_owned = 1;
  ret = ss_mpi_sys_init();
  if (ret == TD_SUCCESS) capture->sys_owned = 1;
  ret = sample_comm_vi_set_vi_vpss_mode(capture->sys_cfg.mode_type, capture->sys_cfg.aiisp_mode);
  if (ret != TD_SUCCESS) return dv500_ret(ret);
  ret = sample_comm_vi_start_vi(&capture->sys_cfg.vi_cfg);
  if (ret != TD_SUCCESS) return dv500_ret(ret);
  capture->vi_started = 1;
  capture->vpss_cfg.vpss_grp = capture->vpss_grp;
  capture->vpss_cfg.grp_attr = capture->sys_cfg.grp_attr;
  memcpy(capture->vpss_cfg.chn_en, capture->sys_cfg.chn_en, sizeof(capture->vpss_cfg.chn_en));
  memcpy(capture->vpss_cfg.chn_attr, capture->sys_cfg.chn_attr, sizeof(capture->vpss_cfg.chn_attr));
  memcpy(capture->vpss_chn_attr.chn_enable,
         capture->sys_cfg.chn_en,
         sizeof(capture->vpss_chn_attr.chn_enable));
  memcpy(capture->vpss_chn_attr.chn_attr,
         capture->sys_cfg.chn_attr,
         sizeof(capture->vpss_chn_attr.chn_attr));
  capture->vpss_chn_attr.chn_array_size = OT_VPSS_MAX_PHYS_CHN_NUM;
  capture->vpss_chn_attr.chn0_wrap = TD_FALSE;
  ret = sample_common_vpss_start(capture->vpss_grp,
                                 &capture->vpss_cfg.grp_attr,
                                 &capture->vpss_chn_attr);
  if (ret != TD_SUCCESS) return dv500_ret(ret);
  capture->vpss_started = 1;
  ret = sample_comm_vi_bind_vpss(capture->vi_pipe, capture->vi_chn, capture->vpss_grp, DV500_VPSS_CHN);
  if (ret != TD_SUCCESS) return dv500_ret(ret);
  capture->vi_vpss_bound = 1;
  return GBMC_OK;
}

static void dv500_stop_sys(gbmc_dv500_capture_t *capture) {
  if (!capture) return;
  if (capture->vi_vpss_bound) {
    sample_comm_vi_un_bind_vpss(capture->vi_pipe, capture->vi_chn, capture->vpss_grp, DV500_VPSS_CHN);
    capture->vi_vpss_bound = 0;
  }
  if (capture->vpss_started) {
    sample_common_vpss_stop(capture->vpss_grp, capture->vpss_cfg.chn_en, OT_VPSS_MAX_PHYS_CHN_NUM);
    capture->vpss_started = 0;
  }
  if (capture->vi_started) {
    sample_comm_vi_stop_vi(&capture->sys_cfg.vi_cfg);
    capture->vi_started = 0;
  }
  if (capture->sys_owned) {
    ss_mpi_sys_exit();
    capture->sys_owned = 0;
  }
  if (capture->vb_owned) {
    ss_mpi_vb_exit();
    capture->vb_owned = 0;
  }
}

static void *dv500_capture_thread(void *arg) {
  gbmc_dv500_capture_t *capture = (gbmc_dv500_capture_t *) arg;
  while (capture && !capture->stop_requested) {
    ot_video_frame_info frame;
    td_s32 ret;
    memset(&frame, 0, sizeof(frame));
    ret = ss_mpi_vpss_get_chn_frame(capture->vpss_grp, DV500_VPSS_CHN, &frame, 1000);
    if (ret == TD_SUCCESS) {
      uint8_t *data = NULL;
      size_t size = 0;
      if (dv500_copy_sp420(&frame.video_frame, &data, &size) == GBMC_OK && capture->callbacks.on_sample) {
        gbmc_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        sample.type = GBMC_SAMPLE_VIDEO_RAW;
        sample.pts_us = (int64_t) frame.video_frame.pts;
        sample.duration_us = capture->config.fps_num > 0
                               ? (int64_t) capture->config.fps_den * 1000000LL / capture->config.fps_num
                               : 40000;
        sample.data = data;
        sample.size = size;
        sample.width = (int) frame.video_frame.width;
        sample.height = (int) frame.video_frame.height;
        sample.format = capture->format_name;
        sample.flags = GBMC_SAMPLE_FLAG_TRANSIENT_DATA;
        (void) capture->callbacks.on_sample(capture->callbacks.user_data, &sample);
      }
      free(data);
      (void) ss_mpi_vpss_release_chn_frame(capture->vpss_grp, DV500_VPSS_CHN, &frame);
    } else {
      gb_sleep_ms(5);
    }
  }
  return NULL;
}
#endif

static int dv500_probe(void) {
#if GBMEDIA_WITH_DV500
  return dv500_device_exists() ? GBMC_OK : GBMC_ERR_NOT_FOUND;
#else
  return GBMC_ERR_UNSUPPORTED;
#endif
}

static int dv500_list_devices(gbmc_media_type_t media_type,
                              gbmc_device_info_t *devices,
                              size_t max_devices,
                              size_t *count_out) {
  if (!count_out) return GBMC_ERR_INVALID;
  *count_out = 0;
  if (media_type != GBMC_MEDIA_VIDEO) return GBMC_ERR_UNSUPPORTED;
#if !GBMEDIA_WITH_DV500
  (void) devices;
  (void) max_devices;
  return GBMC_ERR_UNSUPPORTED;
#else
  if (dv500_probe() != GBMC_OK) return GBMC_ERR_NOT_FOUND;
  *count_out = 1;
  if (devices && max_devices > 0) {
    memset(&devices[0], 0, sizeof(devices[0]));
    copy_text(devices[0].backend, sizeof(devices[0].backend), "dv500");
    copy_text(devices[0].id, sizeof(devices[0].id), DV500_CAMERA_ID);
    copy_text(devices[0].name, sizeof(devices[0].name), "DV500 camera");
    devices[0].media_type = GBMC_MEDIA_VIDEO;
  }
  return GBMC_OK;
#endif
}

static int dv500_open(const gbmc_capture_config_t *config,
                      const gbmc_capture_callbacks_t *callbacks,
                      gbmc_capture_t **capture_out) {
  gbmc_dv500_capture_t *capture;
  if (!config || !capture_out) return GBMC_ERR_INVALID;
  *capture_out = NULL;
#if !GBMEDIA_WITH_DV500
  (void) callbacks;
  return GBMC_ERR_UNSUPPORTED;
#else
  if (config->video_device_id && config->video_device_id[0] &&
      strcmp(config->video_device_id, DV500_CAMERA_ID) != 0) {
    return GBMC_ERR_NOT_FOUND;
  }
  if (dv500_probe() != GBMC_OK) return GBMC_ERR_NOT_FOUND;
  capture = (gbmc_dv500_capture_t *) calloc(1, sizeof(*capture));
  if (!capture) return GBMC_ERR_NO_MEMORY;
  capture->base.backend = gbmc_dv500_backend();
  capture->config = *config;
  capture->width = config->width > 0 ? config->width : 1280;
  capture->height = config->height > 0 ? config->height : 720;
  if (callbacks) capture->callbacks = *callbacks;
  *capture_out = &capture->base;
  return GBMC_OK;
#endif
}

static void dv500_close(gbmc_capture_t *capture) {
  if (!capture) return;
  (void) dv500_stop(capture);
  free(capture);
}

static int dv500_start(gbmc_capture_t *capture) {
  if (!capture) return GBMC_ERR_INVALID;
#if !GBMEDIA_WITH_DV500
  return GBMC_ERR_UNSUPPORTED;
#else
  gbmc_dv500_capture_t *dv = (gbmc_dv500_capture_t *) capture;
  sample_sns_type sensor_type;
  ot_pixel_format pixel_format;
  int rc;
  if (dv->started) return GBMC_OK;
  sensor_type = dv500_sensor_type();
  pixel_format = dv500_pixel_format(&dv->format_name);
  rc = dv500_start_sys(dv, sensor_type, pixel_format);
  if (rc != GBMC_OK) {
    dv500_stop_sys(dv);
    return rc;
  }
  dv->stop_requested = 0;
  if (gb_thread_create(&dv->thread, dv500_capture_thread, dv) != 0) {
    dv500_stop_sys(dv);
    return GBMC_ERR_BACKEND;
  }
  dv->started = 1;
  return GBMC_OK;
#endif
}

static int dv500_stop(gbmc_capture_t *capture) {
  if (!capture) return GBMC_ERR_INVALID;
#if GBMEDIA_WITH_DV500
  gbmc_dv500_capture_t *dv = (gbmc_dv500_capture_t *) capture;
  if (dv->started) {
    dv->stop_requested = 1;
    if (!gb_thread_is_current(dv->thread)) gb_thread_join(dv->thread);
    memset(&dv->thread, 0, sizeof(dv->thread));
    dv->started = 0;
  }
  dv500_stop_sys(dv);
#endif
  return GBMC_OK;
}

const gbmc_capture_backend_t *gbmc_dv500_backend(void) {
  static const gbmc_capture_backend_t backend = {
      "dv500",
      "DV500 VI/VPSS camera capture backend",
      300,
      dv500_probe,
      dv500_list_devices,
      dv500_open,
      dv500_close,
      dv500_start,
      dv500_stop,
  };
  return &backend;
}
