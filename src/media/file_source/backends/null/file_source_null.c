#include "gb_media_file_source.h"

struct gbmf_file_source {
  int unused;
};

const char *gbmf_result_name(int result) {
  switch (result) {
    case GBMF_OK: return "GBMF_OK";
    case GBMF_ERR_INVALID: return "GBMF_ERR_INVALID";
    case GBMF_ERR_NOT_FOUND: return "GBMF_ERR_NOT_FOUND";
    case GBMF_ERR_NO_MEMORY: return "GBMF_ERR_NO_MEMORY";
    case GBMF_ERR_BACKEND: return "GBMF_ERR_BACKEND";
    case GBMF_ERR_UNSUPPORTED: return "GBMF_ERR_UNSUPPORTED";
    case GBMF_ERR_EOF: return "GBMF_ERR_EOF";
    default: return "GBMF_ERR_UNKNOWN";
  }
}

int gbmf_open(const gbmf_file_source_config_t *config,
              const gbmf_file_source_callbacks_t *callbacks,
              gbmf_file_source_t **source_out) {
  (void) config;
  (void) callbacks;
  if (source_out) *source_out = NULL;
  return GBMF_ERR_UNSUPPORTED;
}

void gbmf_close(gbmf_file_source_t **source) {
  if (source) *source = NULL;
}

int gbmf_read(gbmf_file_source_t *source) {
  (void) source;
  return GBMF_ERR_UNSUPPORTED;
}
