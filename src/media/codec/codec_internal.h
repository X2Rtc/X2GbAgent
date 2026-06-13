#ifndef GB_MEDIA_CODEC_INTERNAL_H
#define GB_MEDIA_CODEC_INTERNAL_H

#include "gb_media_codec.h"

struct gbmcd_codec {
  const gbmcd_codec_backend_t *backend;
};

const gbmcd_codec_backend_t *gbmcd_null_backend(void);
#if defined(GBMEDIA_ENABLE_FFMPEG_BACKEND) && GBMEDIA_ENABLE_FFMPEG_BACKEND
const gbmcd_codec_backend_t *gbmcd_ffmpeg_backend(void);
#endif
#if defined(GBMEDIA_ENABLE_DV500_BACKEND) && GBMEDIA_ENABLE_DV500_BACKEND
const gbmcd_codec_backend_t *gbmcd_dv500_backend(void);
#endif

#endif
