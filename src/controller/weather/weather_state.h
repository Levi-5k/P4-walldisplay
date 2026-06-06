/* Cached weather snapshot shared by weather_api and LVGL screens. */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_FORECAST_DAYS 5
#define WEATHER_FORECAST_HOURS 24

typedef struct {
    uint32_t dt_utc;          /* representative dt for the day (UTC seconds) */
    int16_t  hi_f;
    int16_t  lo_f;
    char     condition[16];   /* short, e.g. "Rain" */
    char     icon[8];         /* OWM icon code, e.g. "10d" */
    uint8_t  pop_pct;         /* max probability of precipitation, percent */
} weather_day_t;

typedef struct {
    uint32_t dt_utc;          /* hourly forecast timestamp (UTC seconds) */
    int16_t  temp_f;
    uint8_t  humidity_pct;
    uint16_t pressure_hpa;
    uint8_t  pop_pct;
    uint16_t precip_in_x100;
    uint16_t rain_in_x100;
    uint16_t snow_in_x100;
} weather_hour_t;

typedef struct {
    bool     valid;
    /* Current observation */
    int32_t  temp_f;          /* current temperature, deg F */
    int32_t  feels_f;
    int32_t  temp_min_f;
    int32_t  temp_max_f;
    uint8_t  humidity_pct;
    uint16_t pressure_hpa;
    uint16_t sea_level_hpa;
    uint16_t grnd_level_hpa;
    uint16_t wind_mph_x10;    /* mph * 10 */
    uint16_t wind_gust_mph_x10;
    bool     wind_gust_valid;
    uint16_t wind_deg;        /* 0..359 */
    uint8_t  clouds_pct;
    uint16_t visibility_m;    /* meters, OWM caps at 10000 */
    uint16_t rain_1h_mm_x10;
    uint16_t snow_1h_mm_x10;
    uint8_t  uv_index;        /* rounded current UV index */
    bool     uv_index_valid;
    uint16_t weather_id;      /* OWM weather condition id */
    char     condition[32];   /* short text e.g. "Clear" */
    char     description[48]; /* long text e.g. "broken clouds" */
    char     icon[8];         /* OWM icon code, e.g. "01d" */
    char     city[40];
    char     country[4];
    uint32_t observed_utc;    /* dt from current obs */
    uint32_t sunrise_utc;
    uint32_t sunset_utc;
    int32_t  tz_offset_s;     /* timezone offset of the location, seconds */

    /* Forecast (next ~5 local days) */
    weather_day_t days[WEATHER_FORECAST_DAYS];
    uint8_t  day_count;

    /* Compact hourly forecast for graphing (next ~24 hours) */
    weather_hour_t hours[WEATHER_FORECAST_HOURS];
    uint8_t  hour_count;

    uint32_t fetched_at_ms;   /* esp_timer_get_time() / 1000 at fetch */
} weather_state_t;

esp_err_t weather_state_init(void);
esp_err_t weather_state_get(weather_state_t *out);
esp_err_t weather_state_set(const weather_state_t *in);
esp_err_t weather_state_clear(void);

#ifdef __cplusplus
}
#endif
