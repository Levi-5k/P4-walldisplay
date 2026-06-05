/* Rolling weather history persisted on the SD card. */
#pragma once

#include "esp_err.h"
#include "weather_state.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_HISTORY_MAX_POINTS 96

typedef struct {
    uint32_t observed_utc;
    int16_t temp_f;
    int16_t feels_f;
    uint8_t humidity_pct;
    uint16_t pressure_hpa;
    uint16_t sea_level_hpa;
    uint16_t grnd_level_hpa;
    uint16_t wind_mph_x10;
    uint16_t wind_gust_mph_x10;
    uint16_t wind_deg;
    uint8_t clouds_pct;
    uint16_t rain_1h_mm_x10;
    uint16_t snow_1h_mm_x10;
} weather_history_sample_t;

typedef struct {
    bool loaded;
    uint8_t count;
    esp_err_t storage_err;
    char storage_detail[72];
    weather_history_sample_t samples[WEATHER_HISTORY_MAX_POINTS];
} weather_history_t;

esp_err_t weather_history_init(void);
esp_err_t weather_history_record(const weather_state_t *state);
esp_err_t weather_history_get(weather_history_t *out);
const char *weather_history_file_path(void);

#ifdef __cplusplus
}
#endif