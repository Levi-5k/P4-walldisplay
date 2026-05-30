#pragma once

#include "app_config.h"
#include "esp_err.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- background-image preset table ---- */
typedef struct {
    const char *label;
    const char *urls[APP_THEME_MAX_IMAGES];
} bg_preset_t;

uint8_t         bg_preset_count(void);
const bg_preset_t *bg_preset_get(uint8_t index);
uint8_t         bg_preset_url_count(uint8_t index);
const char     *bg_preset_dropdown_options(void);

/* ---- lifecycle ---- */
/**
 * Call from app_main() BEFORE ui_init().  Checks whether the auto-download
 * task will need HTTPS and, if so, pauses the SD card so the SDMMC host
 * controller never initialises Slot 0 (which permanently poisons the SDIO
 * Wi-Fi link on Slot 1 for that boot).
 */
void ui_background_pre_init(void);
void ui_background_attach(lv_obj_t *screen);
void ui_background_attach_idle_weather(lv_obj_t *screen);
void ui_background_refresh(void);

/* ---- download API ---- */
typedef struct {
    bool busy;
    bool has_progress;
    uint8_t progress_pct;
    uint8_t image_index;
    uint8_t image_total;
    char status[120];
} ui_background_download_state_t;

esp_err_t ui_background_download_start(const char *url, uint8_t preset_index);
esp_err_t ui_background_download_collection_start(const char *const *urls,
                                                   uint8_t url_count,
                                                   uint8_t preset_index);
esp_err_t ui_background_clear_images(void);
esp_err_t ui_background_delete_folder(void);
bool ui_background_is_busy(void);
const char *ui_background_status(void);
void ui_background_download_state_get(ui_background_download_state_t *out);
bool ui_background_preset_images_present(uint8_t preset_index,
                                         uint8_t *present_count,
                                         uint8_t *total_count);

/**
 * Legacy auto-download entry point. Do not call during boot or from weather /
 * service callbacks on this board; SD slot 0 and hosted Wi-Fi share SDMMC.
 */
void ui_background_auto_download(void);

#ifdef __cplusplus
}
#endif
