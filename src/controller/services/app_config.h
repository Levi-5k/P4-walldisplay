#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool configured;
    char ssid[33];
    char psk[65];
} app_wifi_config_t;

typedef struct {
    bool configured;
    char api_key[96];
    /* Coordinates are now supplied by the boot IP-location check. */
    int32_t lat_x1e6;
    int32_t lon_x1e6;
} app_weather_config_t;

typedef struct {
    uint16_t weather_refresh_min;
    uint16_t weather_retry_s;
    uint16_t weather_wifi_wait_s;
    uint8_t weather_http_timeout_s;
    uint8_t weather_forecast_gap_s;
    uint16_t weather_tab_stale_min;
    uint8_t weather_tab_wake_s;
    uint8_t weather_page_update_s;
    uint8_t wled_poll_s;
    uint8_t wled_stale_s;
    uint8_t auto_brightness_min_pct;
    uint8_t auto_brightness_max_pct;
    uint16_t auto_brightness_eval_s;
    uint8_t auto_brightness_ramp_s;
    uint16_t auto_brightness_hold_min;
    uint8_t low_brightness_warn_pct;
    uint8_t idle_check_s;
    uint8_t status_bar_update_s;
    uint16_t toast_duration_ms;
} app_tuning_config_t;

#define APP_THEME_MAX_IMAGES 8

typedef struct {
    bool configured;
    bool background_enabled;
    uint8_t surface_opacity_pct;
    uint8_t background_dim_pct;
    uint16_t slideshow_seconds;
    uint8_t image_count;
    uint8_t next_slot;
    uint8_t background_preset;
    uint32_t bg_color_hex;
    uint32_t surface_color_hex;
    uint32_t card_color_hex;
    uint32_t border_color_hex;
    uint32_t primary_color_hex;
    uint32_t accent_color_hex;
    char background_url[256];
} app_theme_config_t;

esp_err_t app_config_wifi_load(app_wifi_config_t *out);
esp_err_t app_config_wifi_save(const char *ssid, const char *psk);
esp_err_t app_config_wifi_clear(void);

esp_err_t app_config_weather_load(app_weather_config_t *out);
esp_err_t app_config_weather_save(const char *api_key);
esp_err_t app_config_weather_save_location(int32_t lat_x1e6, int32_t lon_x1e6);
esp_err_t app_config_weather_clear(void);

void app_config_tuning_defaults(app_tuning_config_t *out);
esp_err_t app_config_tuning_load(app_tuning_config_t *out);
esp_err_t app_config_tuning_save(const app_tuning_config_t *cfg);
esp_err_t app_config_tuning_clear(void);

void app_config_theme_defaults(app_theme_config_t *out);
esp_err_t app_config_theme_load(app_theme_config_t *out);
esp_err_t app_config_theme_save(const app_theme_config_t *cfg);
esp_err_t app_config_theme_clear(void);

#ifdef __cplusplus
}
#endif