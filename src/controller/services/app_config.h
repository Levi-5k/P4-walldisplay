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

#define APP_TIMER_AUDIO_PATH_MAX 128
#define APP_TIMER_QUICK_PRESET_COUNT 4
#define APP_TIMER_HISTORY_COUNT 4
#define APP_TIMER_MAX_SECONDS (10u * 60u * 60u)

typedef enum {
    APP_WEATHER_BOTTOM_PANEL_DATA = 0,
    APP_WEATHER_BOTTOM_PANEL_GRAPHS = 1,
    APP_WEATHER_BOTTOM_PANEL_CYCLE = 2,
} app_weather_bottom_panel_mode_t;

typedef struct {
    uint16_t weather_refresh_min;
    uint16_t weather_retry_s;
    uint16_t weather_wifi_wait_s;
    uint8_t weather_http_timeout_s;
    uint8_t weather_forecast_gap_s;
    uint16_t weather_tab_stale_min;
    uint8_t weather_tab_wake_s;
    uint8_t weather_page_update_s;
    uint16_t weather_graph_cycle_s;
    uint8_t weather_bottom_panel_mode;
    uint16_t weather_bottom_panel_cycle_s;
    uint16_t time_sync_interval_min;
    uint8_t time_sync_hour;
    uint8_t wled_poll_s;
    uint8_t wled_stale_s;
    uint8_t wled_hue_update_hz;
    bool power_preset_mode_enabled;
    uint16_t power_on_preset_id;
    uint16_t power_off_preset_id;
    uint16_t power_on_delay_ms;
    bool light_safety_auto_off_enabled;
    uint8_t light_safety_auto_off_hours;
    bool auto_brightness_enabled;
    uint8_t auto_brightness_min_pct;
    uint8_t auto_brightness_max_pct;
    uint16_t auto_brightness_eval_s;
    uint8_t auto_brightness_ramp_s;
    uint16_t auto_brightness_hold_min;
    uint8_t low_brightness_warn_pct;
    uint8_t idle_check_s;
    bool idle_dismiss_lights_on;
    bool idle_dismiss_lights_timer_on;
    uint16_t idle_dismiss_lights_timer_min;
    uint16_t idle_swipe_dismiss_min;
    bool idle_swipe_wake_lights_on;
    uint8_t status_bar_update_s;
    bool system_cpu_load_enabled;
    uint16_t toast_duration_ms;
    uint8_t timer_audio_volume_pct;
    bool timer_repeat_until_dismissed;
    uint8_t timer_repeat_gap_s;
    uint8_t timer_snooze_min;
    uint8_t timer_snooze_limit;
    uint16_t timer_default_seconds;
    uint16_t timer_quick_seconds[APP_TIMER_QUICK_PRESET_COUNT];
    uint16_t timer_history_seconds[APP_TIMER_HISTORY_COUNT];
    uint16_t timer_prealert_s;
    uint8_t timer_finish_light_action;
    bool timer_auto_show_on_finish;
    bool timer_show_finish_toast;
    char timer_audio_path[APP_TIMER_AUDIO_PATH_MAX];
} app_tuning_config_t;

#define APP_THEME_MAX_IMAGES 12

typedef struct {
    bool configured;
    bool background_enabled;
    bool background_idle_only;
    uint8_t surface_opacity_pct;
    uint8_t background_dim_pct;
    bool shadows_enabled;
    bool slideshow_enabled;
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