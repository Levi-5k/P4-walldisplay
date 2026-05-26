/*
 * display_bsp.h
 *
 * Thin wrapper around the official Waveshare BSP component
 * (waveshare/esp32_p4_wifi6_touch_lcd_4b) that handles MIPI-DSI panel,
 * GT911 touch, CH422G IO expander, I2C bus, and LVGL port wiring.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the panel + touch + LVGL port via the Waveshare BSP.
 *        Backlight is turned on at a default brightness. After this call,
 *        callers may use bsp_display_lock()/bsp_display_unlock() from
 *        <bsp/esp-bsp.h> to manipulate the LVGL tree.
 */
esp_err_t display_bsp_init(void);

/**
 * @brief Set the panel backlight duty cycle (0..100). Wraps
 *        bsp_display_brightness_set().
 */
esp_err_t display_bsp_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
