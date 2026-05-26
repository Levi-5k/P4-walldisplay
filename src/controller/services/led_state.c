#include "led_state.h"
#include "backlight_pwm.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG     = "led_state";
static const char *NVS_NS  = "leds";

#define MAX_SUBS 6

static led_state_t s;
static struct { led_state_cb_t cb; void *user; } subs[MAX_SUBS];
static int sub_count;

static void notify(void)
{
    for (int i = 0; i < sub_count; i++) subs[i].cb(&s, subs[i].user);
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
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
    s.kelvin                 = 3500;
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
        nvs_get_u16(h, "k_min", &s.kelvin_min);
        nvs_get_u16(h, "k_max", &s.kelvin_max);
        nvs_get_u16(h, "to_s",  &s.screen_timeout_s);
        nvs_get_u8 (h, "disp",  &s.display_brightness_pct);
        nvs_close(h);
    }
    if (s.kelvin < s.kelvin_min) s.kelvin = s.kelvin_min;
    if (s.kelvin > s.kelvin_max) s.kelvin = s.kelvin_max;

    /* Backlight is applied separately once display_bsp is up. */

    ESP_LOGI(TAG, "init power=%d bri=%u%% K=%u (%u..%u) timeout=%us disp=%u%%",
             s.power, s.brightness_pct, s.kelvin,
             s.kelvin_min, s.kelvin_max,
             s.screen_timeout_s, s.display_brightness_pct);
    return ESP_OK;
}

void led_state_get(led_state_t *out) { *out = s; }

void led_state_set_power(bool on)
{
    if (s.power == on) return;
    s.power = on;
    notify();
}

void led_state_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (s.brightness_pct == pct) return;
    s.brightness_pct = pct;
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
