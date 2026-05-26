#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a progressive JPEG file to baseline JPEG in-place.
 * If the file is already baseline, it is left unchanged.
 * Returns ESP_OK on success, ESP_ERR_NOT_SUPPORTED if already baseline.
 */
esp_err_t jpeg_progressive_to_baseline(const char *filepath);

#ifdef __cplusplus
}
#endif
