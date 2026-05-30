#pragma once

#include "audio_codec_data_if.h"
#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_BUS_SAMPLE_RATE     16000
#define AUDIO_BUS_CHANNELS        1
#define AUDIO_BUS_BITS_PER_SAMPLE 16

esp_err_t audio_bus_init(void);
bool audio_bus_is_ready(void);
const audio_codec_data_if_t *audio_bus_data_if(void);
const char *audio_bus_last_error(void);

#ifdef __cplusplus
}
#endif