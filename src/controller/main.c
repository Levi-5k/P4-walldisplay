/*
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (Smart 86 Box) — P4 controller main
 *
 * Boot order (will grow as phases land):
 *   - log chip / PSRAM / clock info
 *   - state stores (LED/settings, weather cache)
 *   - microphone capture + FFT before large DMA consumers
 *   - ui_init()       — display + LVGL
 *   - cmd_tx_init()   — RS-485 WLED command transport
 *   - wifi_sta_init() — esp_wifi_remote/esp_hosted when enabled
 *   - weather_task    — OpenWeatherMap when Wi-Fi + config are present
 *   - sound sync      — UDP WLED-MM audio packets once Wi-Fi is online
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "board_pins.h"
#include "ui/ui_init.h"
#include "services/services.h"
#include "services/audio_out.h"
#include "services/led_state.h"
#include "services/sd_storage.h"
#include "services/slave_ota.h"
#include "services/wled_state.h"
#include "ui/backlight_manager.h"
#include "ui/ui_background.h"
#include "weather/weather_state.h"
#include "weather/weather_api.h"

static const char *TAG = "p4_main";

static void log_boot_banner(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Waveshare ESP32-P4 Smart 86 Box starting...");
    ESP_LOGI(TAG, "Chip: %s, cores=%d, rev=%d.%d",
             CONFIG_IDF_TARGET, chip_info.cores,
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "CPU frequency: %d MHz", esp_clk_cpu_freq() / 1000000);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Flash: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));

#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
#endif
}

void app_main(void)
{
    log_boot_banner();

    /* State stores must come up before the UI so pages read sane defaults. */
    led_state_init();
    weather_state_init();

    /* SD is mounted on-demand (not at boot) because SDMMC slot 0 (SD) and
     * slot 1 (esp_hosted Wi-Fi) share the peripheral and conflict under load. */

    esp_err_t wifi_preinit_err = wifi_radio_preinit();
    if (wifi_preinit_err != ESP_OK && wifi_preinit_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Wi-Fi radio preinit failed: %s", esp_err_to_name(wifi_preinit_err));
    }

    /* Reserve the weather task's internal stack before LVGL/background work can
     * fragment the heap. The worker sleeps until Wi-Fi, location, and config exist. */
    weather_api_init();

    /* Check whether background images need downloading.  If so, pause SD
     * BEFORE anything can mount it — the SDMMC host controller cannot have
     * Slot 0 (SD) and Slot 1 (SDIO Wi-Fi) active in the same boot.       */
    ui_background_pre_init();

    /* Audio needs DMA-capable internal memory. Bring it up before LVGL's
     * draw buffers and Wi-Fi tasks consume or fragment that heap. */
    audio_in_init();
    audio_fft_init();
    audio_out_init();

    /* Phase 1.5 — display + LVGL bring-up + main UI. */
    ui_init();

    /* Now that BSP is up, push the persisted display brightness. */
    led_state_apply_display_brightness();
    backlight_manager_init();

    /* Background services. Each init is non-fatal: missing Wi-Fi/weather
     * config should degrade the UI status, not crash the panel. */
    wled_state_init();
    cmd_tx_init();
    wifi_sta_init();
    slave_ota_init();
    provision_init();
    link_health_init();
    esp_err_t serial_debug_err = serial_debug_init();
    if (serial_debug_err != ESP_OK) {
        ESP_LOGW(TAG, "Serial diagnostics init failed: %s", esp_err_to_name(serial_debug_err));
    }
    sound_sync_tx_init();

    /* Heartbeat until the rest of the system comes online. */
    while (1) {
        ESP_LOGI(TAG, "tick");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
