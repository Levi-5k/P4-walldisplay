/*
 * led_state.h — single source of truth for the controlled LED fixture
 * plus user settings persisted to NVS.
 *
 * UI reads via led_state_get(); UI writes via led_state_set_*().
 * Subscribers (e.g. the lights page) get notified after every change.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     power;                  /* current on/off */
    uint8_t  brightness_pct;         /* 0..100 */
    uint16_t kelvin;                 /* current color temp */
    uint8_t  primary_hue;            /* WLED color slot 0 hue */
    uint8_t  secondary_hue;          /* WLED color slot 1 hue */

    /* Settings (persisted to NVS) */
    uint16_t kelvin_min;             /* warm end */
    uint16_t kelvin_max;             /* cool end */
    uint16_t screen_timeout_s;       /* idle timeout */
    uint8_t  display_brightness_pct; /* panel backlight */
} led_state_t;

typedef void (*led_state_cb_t)(const led_state_t *s, void *user);

esp_err_t led_state_init(void);
void      led_state_get(led_state_t *out);

void led_state_set_power(bool on);
void led_state_set_brightness(uint8_t pct);
void led_state_set_kelvin(uint16_t k);
void led_state_set_hues(uint8_t primary_hue, uint8_t secondary_hue);
void led_state_persist_current(void);

void led_state_hold_power_on_for(uint32_t duration_ms);
void led_state_clear_power_on_hold(void);
bool led_state_power_on_hold_active(void);
void led_state_safety_check_now(void);

void led_state_set_kelvin_min(uint16_t k_min);
void led_state_set_kelvin_max(uint16_t k_max);
void led_state_set_screen_timeout(uint16_t sec);
void led_state_set_display_brightness(uint8_t pct);

/* Push the current display_brightness setting to the panel backlight.
 * Call once after display_bsp_init(). */
void led_state_apply_display_brightness(void);

void led_state_subscribe(led_state_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
