#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_IN_SAMPLE_RATE    16000
#define AUDIO_IN_CHANNELS       1
#define AUDIO_IN_FRAME_SAMPLES  512

typedef struct {
    int16_t samples[AUDIO_IN_FRAME_SAMPLES];
    uint32_t timestamp_ms;
} audio_frame_t;

typedef void (*audio_frame_cb_t)(const audio_frame_t *frame, void *user);

esp_err_t audio_in_start(void);
esp_err_t audio_in_subscribe(audio_frame_cb_t cb, void *user);
bool      audio_in_is_ready(void);

#ifdef __cplusplus
}
#endif
