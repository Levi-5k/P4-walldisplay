#include "app_config.h"

#include "sd_storage.h"

#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define WIFI_NS       "wifi_creds"
#define WEATHER_NS    "weather"
#define TUNING_NS     "tuning"
#define THEME_NS      "theme"
#define WIFI_SD_PATH  "/sdcard/walldisplay_wifi.cfg"

#define THEME_DEFAULT_BG_HEX       0x05070D
#define THEME_DEFAULT_SURFACE_HEX  0x111625
#define THEME_DEFAULT_CARD_HEX     0x171D31
#define THEME_DEFAULT_BORDER_HEX   0x303A59
#define THEME_DEFAULT_PRIMARY_HEX  0x1E86FF
#define THEME_DEFAULT_ACCENT_HEX   0xFF4B00

static void trim_line(char *text)
{
    if (!text) return;
    size_t len = strlen(text);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
}

static bool has_line_break(const char *text)
{
    return text && (strchr(text, '\n') || strchr(text, '\r'));
}

static bool weather_location_valid(int32_t lat_x1e6, int32_t lon_x1e6)
{
    return (lat_x1e6 || lon_x1e6) &&
           lat_x1e6 >= -90000000 && lat_x1e6 <= 90000000 &&
           lon_x1e6 >= -180000000 && lon_x1e6 <= 180000000;
}

static esp_err_t wifi_load_nvs(app_wifi_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(out->ssid);
    err = nvs_get_str(h, "ssid", out->ssid, &len);
    if (err == ESP_OK && out->ssid[0]) {
        out->configured = true;
        len = sizeof(out->psk);
        if (nvs_get_str(h, "psk", out->psk, &len) != ESP_OK) out->psk[0] = '\0';
    }

    nvs_close(h);
    return out->configured ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t wifi_save_nvs(const char *ssid, const char *psk)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "psk", psk ? psk : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t wifi_load_sd(app_wifi_config_t *out)
{
    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

    FILE *file = fopen(WIFI_SD_PATH, "r");
    if (!file) return ESP_ERR_NOT_FOUND;

    char ssid[sizeof(out->ssid)] = {0};
    char psk[sizeof(out->psk)] = {0};
    bool have_ssid = fgets(ssid, sizeof(ssid), file) != NULL;
    bool have_psk = fgets(psk, sizeof(psk), file) != NULL;
    fclose(file);

    if (!have_ssid) return ESP_ERR_NOT_FOUND;
    trim_line(ssid);
    trim_line(psk);
    if (!ssid[0]) return ESP_ERR_NOT_FOUND;

    snprintf(out->ssid, sizeof(out->ssid), "%s", ssid);
    snprintf(out->psk, sizeof(out->psk), "%s", have_psk ? psk : "");
    out->configured = true;
    return ESP_OK;
}

static esp_err_t wifi_save_sd(const char *ssid, const char *psk)
{
    char data[sizeof(((app_wifi_config_t *)0)->ssid) + sizeof(((app_wifi_config_t *)0)->psk) + 4];
    int written = snprintf(data, sizeof(data), "%s\n%s\n", ssid, psk ? psk : "");
    if (written < 0 || (size_t)written >= sizeof(data)) return ESP_ERR_INVALID_SIZE;
    return sd_storage_write_text_atomic(WIFI_SD_PATH, data, (size_t)written);
}

static void wifi_remove_sd(void)
{
    if (sd_storage_ensure_mounted() == ESP_OK) {
        (void)remove(WIFI_SD_PATH);
    }
}

esp_err_t app_config_wifi_load(app_wifi_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t err = wifi_load_nvs(out);
    if (err == ESP_OK) return ESP_OK;

    err = wifi_load_sd(out);
    if (err == ESP_OK) (void)wifi_save_nvs(out->ssid, out->psk);
    return err;
}

esp_err_t app_config_wifi_save(const char *ssid, const char *psk)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= sizeof(((app_wifi_config_t *)0)->ssid)) return ESP_ERR_INVALID_SIZE;
    if (psk && strlen(psk) >= sizeof(((app_wifi_config_t *)0)->psk)) return ESP_ERR_INVALID_SIZE;
    if (has_line_break(ssid) || has_line_break(psk)) return ESP_ERR_INVALID_ARG;

    esp_err_t nvs_err = wifi_save_nvs(ssid, psk ? psk : "");
    esp_err_t sd_err = wifi_save_sd(ssid, psk ? psk : "");
    return (nvs_err == ESP_OK || sd_err == ESP_OK) ? ESP_OK : nvs_err;
}

esp_err_t app_config_wifi_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        wifi_remove_sd();
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    wifi_remove_sd();
    return err;
}

esp_err_t app_config_weather_load(app_weather_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(WEATHER_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(out->api_key);
    bool have_key = nvs_get_str(h, "api_key", out->api_key, &len) == ESP_OK && out->api_key[0];
    (void)nvs_get_i32(h, "lat_x1e6", &out->lat_x1e6);
    (void)nvs_get_i32(h, "lon_x1e6", &out->lon_x1e6);
    nvs_close(h);

    out->configured = have_key;
    return out->configured ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t app_config_weather_save(const char *api_key)
{
    if (!api_key || !api_key[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(api_key) >= sizeof(((app_weather_config_t *)0)->api_key)) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(WEATHER_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "api_key", api_key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_weather_save_location(int32_t lat_x1e6, int32_t lon_x1e6)
{
    if (!weather_location_valid(lat_x1e6, lon_x1e6)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(WEATHER_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_i32(h, "lat_x1e6", lat_x1e6);
    if (err == ESP_OK) err = nvs_set_i32(h, "lon_x1e6", lon_x1e6);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_weather_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WEATHER_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void app_config_theme_defaults(app_theme_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->surface_opacity_pct = 100;
    out->background_dim_pct = 58;
    out->slideshow_seconds = 25;
    out->bg_color_hex = THEME_DEFAULT_BG_HEX;
    out->surface_color_hex = THEME_DEFAULT_SURFACE_HEX;
    out->card_color_hex = THEME_DEFAULT_CARD_HEX;
    out->border_color_hex = THEME_DEFAULT_BORDER_HEX;
    out->primary_color_hex = THEME_DEFAULT_PRIMARY_HEX;
    out->accent_color_hex = THEME_DEFAULT_ACCENT_HEX;
}

static uint8_t clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static uint16_t clamp_u16(uint16_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

void app_config_tuning_defaults(app_tuning_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->weather_refresh_min = 15;
    out->weather_retry_s = 60;
    out->weather_wifi_wait_s = 30;
    out->weather_http_timeout_s = 10;
    out->weather_forecast_gap_s = 5;
    out->weather_tab_stale_min = 10;
    out->weather_tab_wake_s = 15;
    out->weather_page_update_s = 1;
    out->wled_poll_s = 5;
    out->wled_stale_s = 30;
    out->wled_hue_update_hz = 5;
    out->auto_brightness_enabled = false;
    out->auto_brightness_min_pct = 5;
    out->auto_brightness_max_pct = 100;
    out->auto_brightness_eval_s = 60;
    out->auto_brightness_ramp_s = 5;
    out->auto_brightness_hold_min = 10;
    out->low_brightness_warn_pct = 20;
    out->idle_check_s = 1;
    out->idle_dismiss_lights_on = false;
    out->idle_dismiss_lights_timer_on = false;
    out->idle_dismiss_lights_timer_min = 30;
    out->idle_swipe_dismiss_min = 30;
    out->idle_swipe_wake_lights_on = true;
    out->status_bar_update_s = 1;
    out->toast_duration_ms = 1600;
    out->timer_audio_volume_pct = 70;
    out->timer_repeat_until_dismissed = true;
    out->timer_repeat_gap_s = 2;
    out->timer_snooze_min = 5;
    out->timer_snooze_limit = 3;
    out->timer_default_seconds = 60;
    out->timer_quick_seconds[0] = 30;
    out->timer_quick_seconds[1] = 60;
    out->timer_quick_seconds[2] = 300;
    out->timer_quick_seconds[3] = 600;
    for (size_t i = 0; i < APP_TIMER_HISTORY_COUNT; i++) {
        out->timer_history_seconds[i] = 0;
    }
    out->timer_prealert_s = 10;
    out->timer_finish_light_action = 0;
    out->timer_auto_show_on_finish = true;
    out->timer_show_finish_toast = false;
    out->timer_audio_path[0] = '\0';
}

static void app_config_tuning_clamp(app_tuning_config_t *cfg)
{
    if (!cfg) return;
    cfg->weather_refresh_min = clamp_u16(cfg->weather_refresh_min, 5, 120);
    cfg->weather_retry_s = clamp_u16(cfg->weather_retry_s, 15, 300);
    cfg->weather_wifi_wait_s = clamp_u16(cfg->weather_wifi_wait_s, 10, 120);
    cfg->weather_http_timeout_s = clamp_u8(cfg->weather_http_timeout_s, 5, 30);
    cfg->weather_forecast_gap_s = clamp_u8(cfg->weather_forecast_gap_s, 1, 30);
    cfg->weather_tab_stale_min = clamp_u16(cfg->weather_tab_stale_min, 1, 60);
    cfg->weather_tab_wake_s = clamp_u8(cfg->weather_tab_wake_s, 5, 120);
    cfg->weather_page_update_s = clamp_u8(cfg->weather_page_update_s, 1, 10);
    cfg->wled_poll_s = clamp_u8(cfg->wled_poll_s, 2, 30);
    cfg->wled_stale_s = clamp_u8(cfg->wled_stale_s, 10, 120);
    if (cfg->wled_stale_s < cfg->wled_poll_s * 2) cfg->wled_stale_s = cfg->wled_poll_s * 2;
    cfg->wled_hue_update_hz = clamp_u8(cfg->wled_hue_update_hz, 1, 5);
    cfg->auto_brightness_enabled = cfg->auto_brightness_enabled ? true : false;
    cfg->auto_brightness_min_pct = clamp_u8(cfg->auto_brightness_min_pct, 1, 60);
    cfg->auto_brightness_max_pct = clamp_u8(cfg->auto_brightness_max_pct, 20, 100);
    if (cfg->auto_brightness_max_pct < cfg->auto_brightness_min_pct + 5) {
        cfg->auto_brightness_max_pct = cfg->auto_brightness_min_pct + 5;
    }
    cfg->auto_brightness_eval_s = clamp_u16(cfg->auto_brightness_eval_s, 5, 300);
    cfg->auto_brightness_ramp_s = clamp_u8(cfg->auto_brightness_ramp_s, 1, 30);
    cfg->auto_brightness_hold_min = clamp_u16(cfg->auto_brightness_hold_min, 1, 60);
    cfg->low_brightness_warn_pct = clamp_u8(cfg->low_brightness_warn_pct, 1, 50);
    cfg->idle_check_s = clamp_u8(cfg->idle_check_s, 1, 10);
    cfg->idle_dismiss_lights_on = cfg->idle_dismiss_lights_on ? true : false;
    cfg->idle_dismiss_lights_timer_on = cfg->idle_dismiss_lights_timer_on ? true : false;
    cfg->idle_dismiss_lights_timer_min = clamp_u16(cfg->idle_dismiss_lights_timer_min, 1, 240);
    cfg->idle_swipe_dismiss_min = clamp_u16(cfg->idle_swipe_dismiss_min, 1, 240);
    cfg->idle_swipe_wake_lights_on = cfg->idle_swipe_wake_lights_on ? true : false;
    cfg->status_bar_update_s = clamp_u8(cfg->status_bar_update_s, 1, 10);
    cfg->toast_duration_ms = clamp_u16(cfg->toast_duration_ms, 800, 5000);
    cfg->timer_audio_volume_pct = clamp_u8(cfg->timer_audio_volume_pct, 0, 100);
    cfg->timer_repeat_until_dismissed = cfg->timer_repeat_until_dismissed ? true : false;
    cfg->timer_repeat_gap_s = clamp_u8(cfg->timer_repeat_gap_s, 0, 30);
    cfg->timer_snooze_min = clamp_u8(cfg->timer_snooze_min, 1, 30);
    cfg->timer_snooze_limit = clamp_u8(cfg->timer_snooze_limit, 0, 10);
    cfg->timer_default_seconds = clamp_u16(cfg->timer_default_seconds, 30, APP_TIMER_MAX_SECONDS);
    for (size_t i = 0; i < APP_TIMER_QUICK_PRESET_COUNT; i++) {
        cfg->timer_quick_seconds[i] = clamp_u16(cfg->timer_quick_seconds[i], 10, APP_TIMER_MAX_SECONDS);
    }
    for (size_t i = 0; i < APP_TIMER_HISTORY_COUNT; i++) {
        if (cfg->timer_history_seconds[i] > 0) {
            cfg->timer_history_seconds[i] = clamp_u16(cfg->timer_history_seconds[i], 1, APP_TIMER_MAX_SECONDS);
        }
    }
    cfg->timer_prealert_s = clamp_u16(cfg->timer_prealert_s, 0, 300);
    cfg->timer_finish_light_action = clamp_u8(cfg->timer_finish_light_action, 0, 3);
    cfg->timer_auto_show_on_finish = cfg->timer_auto_show_on_finish ? true : false;
    cfg->timer_show_finish_toast = cfg->timer_show_finish_toast ? true : false;
    cfg->timer_audio_path[sizeof(cfg->timer_audio_path) - 1] = '\0';
    if (has_line_break(cfg->timer_audio_path) ||
        (cfg->timer_audio_path[0] && strncmp(cfg->timer_audio_path, "/sdcard/", 8) != 0)) {
        cfg->timer_audio_path[0] = '\0';
    }
}

esp_err_t app_config_tuning_load(app_tuning_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    app_config_tuning_defaults(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(TUNING_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t u8 = 0;
    uint16_t u16 = 0;
    if (nvs_get_u16(h, "wx_ref_m", &u16) == ESP_OK) out->weather_refresh_min = u16;
    if (nvs_get_u16(h, "wx_retry", &u16) == ESP_OK) out->weather_retry_s = u16;
    if (nvs_get_u16(h, "wx_wifi", &u16) == ESP_OK) out->weather_wifi_wait_s = u16;
    if (nvs_get_u8(h, "wx_http", &u8) == ESP_OK) out->weather_http_timeout_s = u8;
    if (nvs_get_u8(h, "wx_fc_gap", &u8) == ESP_OK) out->weather_forecast_gap_s = u8;
    if (nvs_get_u16(h, "wx_stale", &u16) == ESP_OK) out->weather_tab_stale_min = u16;
    if (nvs_get_u8(h, "wx_wake", &u8) == ESP_OK) out->weather_tab_wake_s = u8;
    if (nvs_get_u8(h, "wx_ui", &u8) == ESP_OK) out->weather_page_update_s = u8;
    if (nvs_get_u8(h, "wl_poll", &u8) == ESP_OK) out->wled_poll_s = u8;
    if (nvs_get_u8(h, "wl_stale", &u8) == ESP_OK) out->wled_stale_s = u8;
    if (nvs_get_u8(h, "wl_hue_hz", &u8) == ESP_OK) out->wled_hue_update_hz = u8;
    if (nvs_get_u8(h, "bl_auto", &u8) == ESP_OK) out->auto_brightness_enabled = u8 != 0;
    if (nvs_get_u8(h, "bl_min", &u8) == ESP_OK) out->auto_brightness_min_pct = u8;
    if (nvs_get_u8(h, "bl_max", &u8) == ESP_OK) out->auto_brightness_max_pct = u8;
    if (nvs_get_u16(h, "bl_eval", &u16) == ESP_OK) out->auto_brightness_eval_s = u16;
    if (nvs_get_u8(h, "bl_ramp", &u8) == ESP_OK) out->auto_brightness_ramp_s = u8;
    if (nvs_get_u16(h, "bl_hold", &u16) == ESP_OK) out->auto_brightness_hold_min = u16;
    if (nvs_get_u8(h, "bl_warn", &u8) == ESP_OK) out->low_brightness_warn_pct = u8;
    if (nvs_get_u8(h, "idle_chk", &u8) == ESP_OK) out->idle_check_s = u8;
    if (nvs_get_u8(h, "idle_wlon", &u8) == ESP_OK) out->idle_dismiss_lights_on = u8 != 0;
    if (nvs_get_u8(h, "idle_wlt_on", &u8) == ESP_OK) out->idle_dismiss_lights_timer_on = u8 != 0;
    if (nvs_get_u16(h, "idle_wlt_min", &u16) == ESP_OK) out->idle_dismiss_lights_timer_min = u16;
    if (nvs_get_u16(h, "idle_swp_min", &u16) == ESP_OK) out->idle_swipe_dismiss_min = u16;
    if (nvs_get_u8(h, "idle_swp_wl", &u8) == ESP_OK) out->idle_swipe_wake_lights_on = u8 != 0;
    if (nvs_get_u8(h, "stat_upd", &u8) == ESP_OK) out->status_bar_update_s = u8;
    if (nvs_get_u16(h, "toast_ms", &u16) == ESP_OK) out->toast_duration_ms = u16;
    if (nvs_get_u8(h, "tmr_vol", &u8) == ESP_OK) out->timer_audio_volume_pct = u8;
    if (nvs_get_u8(h, "tmr_repeat", &u8) == ESP_OK) out->timer_repeat_until_dismissed = u8 != 0;
    if (nvs_get_u8(h, "tmr_gap", &u8) == ESP_OK) out->timer_repeat_gap_s = u8;
    if (nvs_get_u8(h, "tmr_snooze", &u8) == ESP_OK) out->timer_snooze_min = u8;
    if (nvs_get_u8(h, "tmr_snz_lim", &u8) == ESP_OK) out->timer_snooze_limit = u8;
    if (nvs_get_u16(h, "tmr_def_s", &u16) == ESP_OK) out->timer_default_seconds = u16;
    if (nvs_get_u16(h, "tmr_q1", &u16) == ESP_OK) out->timer_quick_seconds[0] = u16;
    if (nvs_get_u16(h, "tmr_q2", &u16) == ESP_OK) out->timer_quick_seconds[1] = u16;
    if (nvs_get_u16(h, "tmr_q3", &u16) == ESP_OK) out->timer_quick_seconds[2] = u16;
    if (nvs_get_u16(h, "tmr_q4", &u16) == ESP_OK) out->timer_quick_seconds[3] = u16;
    if (nvs_get_u16(h, "tmr_h1", &u16) == ESP_OK) out->timer_history_seconds[0] = u16;
    if (nvs_get_u16(h, "tmr_h2", &u16) == ESP_OK) out->timer_history_seconds[1] = u16;
    if (nvs_get_u16(h, "tmr_h3", &u16) == ESP_OK) out->timer_history_seconds[2] = u16;
    if (nvs_get_u16(h, "tmr_h4", &u16) == ESP_OK) out->timer_history_seconds[3] = u16;
    if (nvs_get_u16(h, "tmr_pre", &u16) == ESP_OK) out->timer_prealert_s = u16;
    if (nvs_get_u8(h, "tmr_light", &u8) == ESP_OK) out->timer_finish_light_action = u8;
    if (nvs_get_u8(h, "tmr_auto_pg", &u8) == ESP_OK) out->timer_auto_show_on_finish = u8 != 0;
    if (nvs_get_u8(h, "tmr_fn_toast", &u8) == ESP_OK) out->timer_show_finish_toast = u8 != 0;
    size_t str_len = sizeof(out->timer_audio_path);
    (void)nvs_get_str(h, "tmr_audio", out->timer_audio_path, &str_len);
    nvs_close(h);

    app_config_tuning_clamp(out);
    return ESP_OK;
}

esp_err_t app_config_tuning_save(const app_tuning_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    app_tuning_config_t clean = *cfg;
    app_config_tuning_clamp(&clean);

    nvs_handle_t h;
    esp_err_t err = nvs_open(TUNING_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, "wx_ref_m", clean.weather_refresh_min);
    if (err == ESP_OK) err = nvs_set_u16(h, "wx_retry", clean.weather_retry_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "wx_wifi", clean.weather_wifi_wait_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "wx_http", clean.weather_http_timeout_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "wx_fc_gap", clean.weather_forecast_gap_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "wx_stale", clean.weather_tab_stale_min);
    if (err == ESP_OK) err = nvs_set_u8(h, "wx_wake", clean.weather_tab_wake_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "wx_ui", clean.weather_page_update_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "wl_poll", clean.wled_poll_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "wl_stale", clean.wled_stale_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "wl_hue_hz", clean.wled_hue_update_hz);
    if (err == ESP_OK) err = nvs_set_u8(h, "bl_auto", clean.auto_brightness_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "bl_min", clean.auto_brightness_min_pct);
    if (err == ESP_OK) err = nvs_set_u8(h, "bl_max", clean.auto_brightness_max_pct);
    if (err == ESP_OK) err = nvs_set_u16(h, "bl_eval", clean.auto_brightness_eval_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "bl_ramp", clean.auto_brightness_ramp_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "bl_hold", clean.auto_brightness_hold_min);
    if (err == ESP_OK) err = nvs_set_u8(h, "bl_warn", clean.low_brightness_warn_pct);
    if (err == ESP_OK) err = nvs_set_u8(h, "idle_chk", clean.idle_check_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "idle_wlon", clean.idle_dismiss_lights_on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "idle_wlt_on", clean.idle_dismiss_lights_timer_on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(h, "idle_wlt_min", clean.idle_dismiss_lights_timer_min);
    if (err == ESP_OK) err = nvs_set_u16(h, "idle_swp_min", clean.idle_swipe_dismiss_min);
    if (err == ESP_OK) err = nvs_set_u8(h, "idle_swp_wl", clean.idle_swipe_wake_lights_on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "stat_upd", clean.status_bar_update_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "toast_ms", clean.toast_duration_ms);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_vol", clean.timer_audio_volume_pct);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_repeat", clean.timer_repeat_until_dismissed ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_gap", clean.timer_repeat_gap_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_snooze", clean.timer_snooze_min);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_snz_lim", clean.timer_snooze_limit);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_def_s", clean.timer_default_seconds);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_q1", clean.timer_quick_seconds[0]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_q2", clean.timer_quick_seconds[1]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_q3", clean.timer_quick_seconds[2]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_q4", clean.timer_quick_seconds[3]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_h1", clean.timer_history_seconds[0]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_h2", clean.timer_history_seconds[1]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_h3", clean.timer_history_seconds[2]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_h4", clean.timer_history_seconds[3]);
    if (err == ESP_OK) err = nvs_set_u16(h, "tmr_pre", clean.timer_prealert_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_light", clean.timer_finish_light_action);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_auto_pg", clean.timer_auto_show_on_finish ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "tmr_fn_toast", clean.timer_show_finish_toast ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "tmr_audio", clean.timer_audio_path);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_tuning_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(TUNING_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_theme_load(app_theme_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    app_config_theme_defaults(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(THEME_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    uint8_t u8 = 0;
    uint16_t u16 = 0;
    if (nvs_get_u8(h, "bg_en", &u8) == ESP_OK) out->background_enabled = u8 != 0;
    if (nvs_get_u8(h, "bg_idle", &u8) == ESP_OK) out->background_idle_only = u8 != 0;
    if (nvs_get_u8(h, "surface", &u8) == ESP_OK) out->surface_opacity_pct = clamp_u8(u8, 35, 100);
    if (nvs_get_u8(h, "dim", &u8) == ESP_OK) out->background_dim_pct = clamp_u8(u8, 0, 95);
    if (nvs_get_u16(h, "slide", &u16) == ESP_OK) {
        if (u16 < 5) u16 = 5;
        if (u16 > 600) u16 = 600;
        out->slideshow_seconds = u16;
    }
    if (nvs_get_u8(h, "count", &u8) == ESP_OK) out->image_count = clamp_u8(u8, 0, APP_THEME_MAX_IMAGES);
    if (nvs_get_u8(h, "next", &u8) == ESP_OK) out->next_slot = u8 % APP_THEME_MAX_IMAGES;
    if (nvs_get_u8(h, "bg_preset", &u8) == ESP_OK) out->background_preset = u8;

    uint32_t u32 = 0;
    if (nvs_get_u32(h, "bg_hex", &u32) == ESP_OK) out->bg_color_hex = u32 & 0xFFFFFFu;
    if (nvs_get_u32(h, "surface_hex", &u32) == ESP_OK) out->surface_color_hex = u32 & 0xFFFFFFu;
    if (nvs_get_u32(h, "card_hex", &u32) == ESP_OK) out->card_color_hex = u32 & 0xFFFFFFu;
    if (nvs_get_u32(h, "border_hex", &u32) == ESP_OK) out->border_color_hex = u32 & 0xFFFFFFu;
    if (nvs_get_u32(h, "primary_hex", &u32) == ESP_OK) out->primary_color_hex = u32 & 0xFFFFFFu;
    if (nvs_get_u32(h, "accent_hex", &u32) == ESP_OK) out->accent_color_hex = u32 & 0xFFFFFFu;

    if (out->surface_opacity_pct >= 80) out->surface_opacity_pct = 100;

    size_t len = sizeof(out->background_url);
    if (nvs_get_str(h, "url", out->background_url, &len) != ESP_OK) out->background_url[0] = '\0';

    nvs_close(h);
    out->configured = true;
    return ESP_OK;
}

esp_err_t app_config_theme_save(const app_theme_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->background_url[0] && strlen(cfg->background_url) >= sizeof(cfg->background_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (has_line_break(cfg->background_url)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(THEME_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t surface = clamp_u8(cfg->surface_opacity_pct, 35, 100);
    uint8_t dim = clamp_u8(cfg->background_dim_pct, 0, 95);
    uint16_t slide = cfg->slideshow_seconds;
    if (slide < 5) slide = 5;
    if (slide > 600) slide = 600;
    uint8_t count = clamp_u8(cfg->image_count, 0, APP_THEME_MAX_IMAGES);
    uint8_t next = cfg->next_slot % APP_THEME_MAX_IMAGES;

    err = nvs_set_u8(h, "bg_en", cfg->background_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "bg_idle", cfg->background_idle_only ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "surface", surface);
    if (err == ESP_OK) err = nvs_set_u8(h, "dim", dim);
    if (err == ESP_OK) err = nvs_set_u16(h, "slide", slide);
    if (err == ESP_OK) err = nvs_set_u8(h, "count", count);
    if (err == ESP_OK) err = nvs_set_u8(h, "next", next);
    if (err == ESP_OK) err = nvs_set_u8(h, "bg_preset", cfg->background_preset);
    if (err == ESP_OK) err = nvs_set_u32(h, "bg_hex", cfg->bg_color_hex & 0xFFFFFFu);
    if (err == ESP_OK) err = nvs_set_u32(h, "surface_hex", cfg->surface_color_hex & 0xFFFFFFu);
    if (err == ESP_OK) err = nvs_set_u32(h, "card_hex", cfg->card_color_hex & 0xFFFFFFu);
    if (err == ESP_OK) err = nvs_set_u32(h, "border_hex", cfg->border_color_hex & 0xFFFFFFu);
    if (err == ESP_OK) err = nvs_set_u32(h, "primary_hex", cfg->primary_color_hex & 0xFFFFFFu);
    if (err == ESP_OK) err = nvs_set_u32(h, "accent_hex", cfg->accent_color_hex & 0xFFFFFFu);
    if (err == ESP_OK) err = nvs_set_str(h, "url", cfg->background_url);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_theme_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(THEME_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}