#ifndef GB_MEDIA_CAPTURE_INTERNAL_H
#define GB_MEDIA_CAPTURE_INTERNAL_H

#include "gb_media_capture.h"

struct gbmc_capture {
  const gbmc_capture_backend_t *backend;
};

const gbmc_capture_backend_t *gbmc_null_backend(void);
#if defined(GBMEDIA_ENABLE_FFMPEG_BACKEND) && GBMEDIA_ENABLE_FFMPEG_BACKEND
const gbmc_capture_backend_t *gbmc_ffmpeg_backend(void);
#endif
#if defined(GBMEDIA_ENABLE_DV500_BACKEND) && GBMEDIA_ENABLE_DV500_BACKEND
const gbmc_capture_backend_t *gbmc_dv500_backend(void);
#endif

#endif
