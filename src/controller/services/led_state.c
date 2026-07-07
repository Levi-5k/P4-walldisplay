#include "led_state.h"
#include "app_config.h"
#include "backlight_pwm.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG     = "led_state";
static const char *NVS_NS  = "leds";

#define MAX_SUBS 6
#define LIGHT_SAFETY_CHECK_PERIOD_US (60ULL * 1000ULL * 1000ULL)

static led_state_t s;
static struct { led_state_cb_t cb; void *user; } subs[MAX_SUBS];
static int sub_count;
static bool s_power_on_hold;
static uint32_t s_power_on_hold_until_ms;
static uint32_t s_power_on_since_ms;
static esp_timer_handle_t s_light_safety_timer;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void notify(void)
{
    for (int i = 0; i < sub_count; i++) subs[i].cb(&s, subs[i].user);
}

static uint32_t power_on_elapsed_ms(uint32_t now)
{
    if (!s.power) return 0;
    if (!s_power_on_since_ms) s_power_on_since_ms = now;
    return now - s_power_on_since_ms;
}

void led_state_safety_check_now(void)
{
    if (!s.power) return;

    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    if (!tuning.light_safety_auto_off_enabled) return;

    uint32_t max_on_ms = (uint32_t)tuning.light_safety_auto_off_hours * 60u * 60u * 1000u;
    uint32_t now = now_ms();
    if (max_on_ms && power_on_elapsed_ms(now) >= max_on_ms) {
        ESP_LOGW(TAG, "safety auto-off after %u h continuous on", (unsigned)tuning.light_safety_auto_off_hours);
        led_state_set_power(false);
    }
}

static void light_safety_timer_cb(void *arg)
{
    (void)arg;
    led_state_safety_check_now();
}

static void light_safety_timer_start(void)
{
    if (s_light_safety_timer) return;

    const esp_timer_create_args_t args = {
        .callback = light_safety_timer_cb,
        .name = "light_safety",
    };
    esp_err_t err = esp_timer_create(&args, &s_light_safety_timer);
    if (err == ESP_OK) err = esp_timer_start_periodic(s_light_safety_timer, LIGHT_SAFETY_CHECK_PERIOD_US);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light safety timer start failed: %s", esp_err_to_name(err));
        if (s_light_safety_timer) {
            esp_timer_delete(s_light_safety_timer);
            s_light_safety_timer = NULL;
        }
    }
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8 (h, "bri",    s.brightness_pct);
    nvs_set_u8 (h, "wbri",   s.white_brightness_pct);
    nvs_set_u16(h, "kelvin", s.kelvin);
    nvs_set_u8 (h, "hue0",   s.primary_hue);
    nvs_set_u8 (h, "hue1",   s.secondary_hue);
    nvs_set_u16(h, "k_min", s.kelvin_min);
    nvs_set_u16(h, "k_max", s.kelvin_max);
    nvs_set_u16(h, "to_s",  s.screen_timeout_s);
    nvs_set_u8 (h, "disp",  s.display_brightness_pct);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t led_state_init(void)
{
    s.power                  = false;
    s.brightness_pct         = 50;
    s.white_brightness_pct   = 100;
    s.kelvin                 = 3500;
    s.primary_hue            = 0;
    s.secondary_hue          = 0;
    s.kelvin_min             = 2200;
    s.kelvin_max             = 6500;
    s.screen_timeout_s       = 60;
    s.display_brightness_pct = 60;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8 (h, "bri",    &s.brightness_pct);
        nvs_get_u8 (h, "wbri",   &s.white_brightness_pct);
        nvs_get_u16(h, "kelvin", &s.kelvin);
        nvs_get_u8 (h, "hue0",   &s.primary_hue);
        nvs_get_u8 (h, "hue1",   &s.secondary_hue);
        nvs_get_u16(h, "k_min", &s.kelvin_min);
        nvs_get_u16(h, "k_max", &s.kelvin_max);
        nvs_get_u16(h, "to_s",  &s.screen_timeout_s);
        nvs_get_u8 (h, "disp",  &s.display_brightness_pct);
        nvs_close(h);
    }
    if (s.brightness_pct > 100) s.brightness_pct = 100;
    if (s.white_brightness_pct > 100) s.white_brightness_pct = 100;
    if (s.kelvin < s.kelvin_min) s.kelvin = s.kelvin_min;
    if (s.kelvin > s.kelvin_max) s.kelvin = s.kelvin_max;
    s_power_on_since_ms = s.power ? now_ms() : 0;
    light_safety_timer_start();

    /* Backlight is applied separately once display_bsp is up. */

    ESP_LOGI(TAG, "init power=%d bri=%u%% wbri=%u%% K=%u hue=(%u,%u) (%u..%u) timeout=%us disp=%u%%",
             s.power, s.brightness_pct, s.white_brightness_pct, s.kelvin,
             s.primary_hue, s.secondary_hue,
             s.kelvin_min, s.kelvin_max,
             s.screen_timeout_s, s.display_brightness_pct);
    return ESP_OK;
}

void led_state_get(led_state_t *out) { *out = s; }

void led_state_set_power(bool on)
{
    if (!on) led_state_clear_power_on_hold();
    if (s.power == on) {
        if (on && !s_power_on_since_ms) s_power_on_since_ms = now_ms();
        if (!on) s_power_on_since_ms = 0;
        return;
    }
    s.power = on;
    s_power_on_since_ms = on ? now_ms() : 0;
    notify();
}

void led_state_hold_power_on_for(uint32_t duration_ms)
{
    s_power_on_hold = true;
    s_power_on_hold_until_ms = duration_ms ? now_ms() + duration_ms : 0;
}

void led_state_clear_power_on_hold(void)
{
    s_power_on_hold = false;
    s_power_on_hold_until_ms = 0;
}

bool led_state_power_on_hold_active(void)
{
    if (!s_power_on_hold) return false;
    if (!s_power_on_hold_until_ms) return true;
    if (time_reached(now_ms(), s_power_on_hold_until_ms)) {
        led_state_clear_power_on_hold();
        return false;
    }
    return true;
}

void led_state_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (s.brightness_pct == pct) return;
    s.brightness_pct = pct;
    notify();
}

void led_state_set_white_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (s.white_brightness_pct == pct) return;
    s.white_brightness_pct = pct;
    notify();
}

void led_state_set_kelvin(uint16_t k)
{
    if (k < s.kelvin_min) k = s.kelvin_min;
    if (k > s.kelvin_max) k = s.kelvin_max;
    if (s.kelvin == k) return;
    s.kelvin = k;
    notify();
}

void led_state_set_hues(uint8_t primary_hue, uint8_t secondary_hue)
{
    if (s.primary_hue == primary_hue && s.secondary_hue == secondary_hue) return;
    s.primary_hue = primary_hue;
    s.secondary_hue = secondary_hue;
    notify();
}

void led_state_persist_current(void)
{
    persist();
}

void led_state_set_kelvin_min(uint16_t k_min)
{
    if (k_min < 1000) k_min = 1000;
    if (k_min >= s.kelvin_max) k_min = s.kelvin_max - 100;
    s.kelvin_min = k_min;
    if (s.kelvin < k_min) s.kelvin = k_min;
    persist();
    notify();
}

void led_state_set_kelvin_max(uint16_t k_max)
{
    if (k_max > 10000) k_max = 10000;
    if (k_max <= s.kelvin_min) k_max = s.kelvin_min + 100;
    s.kelvin_max = k_max;
    if (s.kelvin > k_max) s.kelvin = k_max;
    persist();
    notify();
}

void led_state_set_screen_timeout(uint16_t sec)
{
    if (sec < 10)   sec = 10;
    if (sec > 3600) sec = 3600;
    s.screen_timeout_s = sec;
    persist();
    notify();
}

void led_state_set_display_brightness(uint8_t pct)
{
    if (pct < 5)   pct = 5;
    if (pct > 100) pct = 100;
    s.display_brightness_pct = pct;
    (void)backlight_set(pct);
    persist();
    notify();
}

void led_state_apply_display_brightness(void)
{
    (void)backlight_set(s.display_brightness_pct);
}

void led_state_subscribe(led_state_cb_t cb, void *user)
{
    if (sub_count < MAX_SUBS) {
        subs[sub_count].cb   = cb;
        subs[sub_count].user = user;
        sub_count++;
    }
}
