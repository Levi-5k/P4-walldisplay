#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;
    const char *filename;
    const char *url;
} audio_library_asset_t;

#define AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE 10

typedef struct {
    bool busy;
    bool has_progress;
    uint8_t progress_pct;
    uint8_t file_index;
    uint8_t file_total;
    char status[120];
} audio_library_state_t;

size_t audio_library_asset_count(void);
const audio_library_asset_t *audio_library_asset_get(size_t index);
bool audio_library_assets_present(uint8_t *present_count, uint8_t *total_count);
esp_err_t audio_library_download_defaults_start(void);
bool audio_library_is_busy(void);
const char *audio_library_status(void);
void audio_library_state_get(audio_library_state_t *out);

#ifdef __cplusplus
}
#endif