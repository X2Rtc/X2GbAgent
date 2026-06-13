#ifndef GB_PREVIEW_JPEG_H
#define GB_PREVIEW_JPEG_H

#include <stddef.h>
#include <stdint.h>

int gb_preview_jpeg_make(const char *source,
                         int is_device,
                         int width,
                         int height,
                         uint8_t **jpeg_out,
                         size_t *jpeg_size_out);

int gb_preview_jpeg_make_at_ms(const char *source,
                               int64_t position_ms,
                               int width,
                               int height,
                               uint8_t **jpeg_out,
                               size_t *jpeg_size_out);

int gb_preview_jpeg_no_signal(int width,
                              int height,
                              uint8_t **jpeg_out,
                              size_t *jpeg_size_out);

int gb_preview_jpeg_no_source(int width,
                              int height,
                              uint8_t **jpeg_out,
                              size_t *jpeg_size_out);

int gb_preview_jpeg_from_raw(const uint8_t *data,
                             size_t size,
                             int src_width,
                             int src_height,
                             const char *format,
                             int out_width,
                             int out_height,
                             uint8_t **jpeg_out,
                             size_t *jpeg_size_out);

void gb_preview_jpeg_free(uint8_t **jpeg);

#endif
