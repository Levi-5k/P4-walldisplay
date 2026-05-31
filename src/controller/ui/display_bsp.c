/*
 * display_bsp.c
 *
 * Bring up the panel + touch + LVGL port via the Waveshare BSP component.
 */

#include "display_bsp.h"

#include "board_pins.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "display_bsp";

#define DEFAULT_BRIGHTNESS_PERCENT  60
#define FAST_DRAW_BUFFER_PIXELS     (BSP_LCD_H_RES * 80)
#define LVGL_TASK_STACK_BYTES       12288

esp_err_t display_bsp_init(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = FAST_DRAW_BUFFER_PIXELS,
        /* Double draw buffer: LVGL renders the next region into one buffer while
         * the previous is flushed to the DSI frame buffer, overlapping draw and
         * copy for smoother updates. Two FAST_DRAW_BUFFER_PIXELS RGB565 buffers
         * (~112 KB each) live in PSRAM. */
        .double_buffer = true,
        .flags = {
            .buff_dma    = true,
            .buff_spiram = true,
            .sw_rotate   = false,
        },
    };

    cfg.lvgl_port_cfg.task_max_sleep_ms = 20;
    cfg.lvgl_port_cfg.task_stack = LVGL_TASK_STACK_BYTES;

    if (bsp_display_start_with_config(&cfg) == NULL) {
        ESP_LOGE(TAG, "bsp_display_start_with_config failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_ERROR_CHECK(bsp_display_brightness_set(DEFAULT_BRIGHTNESS_PERCENT));

    ESP_LOGI(TAG, "display %dx%d up, backlight %d%%",
             BSP_LCD_H_RES, BSP_LCD_V_RES, DEFAULT_BRIGHTNESS_PERCENT);
    return ESP_OK;
}

esp_err_t display_bsp_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    return bsp_display_brightness_set(percent);
}
