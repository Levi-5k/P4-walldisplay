#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FFT_SIZE        512
#define FFT_BINS        16

typedef struct {
    uint8_t  bin[FFT_BINS];
    float    sample_raw;
    float    sample_smooth;
    bool     sample_peak;
    float    fft_magnitude;
    float    fft_major_peak;
    uint32_t timestamp_ms;
} audio_fft_result_t;

typedef void (*audio_fft_cb_t)(const audio_fft_result_t *result, void *user);

esp_err_t audio_fft_start(void);
esp_err_t audio_fft_subscribe(audio_fft_cb_t cb, void *user);
bool      audio_fft_is_ready(void);

#ifdef __cplusplus
}
#endif
