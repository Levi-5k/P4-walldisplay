#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t jpeg_hw_init(void);

/**
 * Decode a baseline JPEG buffer to a raw RGB565 file on SD.
 *
 * The output file has an 8-byte header:
 *   uint16 LE width, height, padded_width, padded_height
 * followed by padded_width*padded_height*2 bytes of dithered RGB565 pixel data.
 *
 * The hardware JPEG decoder handles IDCT, color conversion, and subsampling
 * expansion to RGB888; software dithers to RGB565 to avoid gradient banding.
 */
esp_err_t jpeg_hw_decode_to_file(const uint8_t *jpeg_data, size_t jpeg_len,
                                  const char *out_path);

#ifdef __cplusplus
}
#endif
