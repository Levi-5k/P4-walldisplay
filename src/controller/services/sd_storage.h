#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool mounted;
	bool paused;
	bool network_busy;
	esp_err_t last_err;
	char last_error[96];
} sd_storage_status_t;

esp_err_t sd_storage_ensure_mounted(void);
esp_err_t sd_storage_ensure_dir(const char *path);
esp_err_t sd_storage_write_text_atomic(const char *path, const char *data, size_t len);
bool sd_storage_file_exists(const char *path);
const char *sd_storage_last_error(void);
void sd_storage_status_get(sd_storage_status_t *out);

/**
 * Prevent app-level SD access until sd_storage_resume() is called.
 * SDMMC Slot 0 (SD card) and Slot 1 (Wi-Fi SDIO) share host resources,
 * so long HTTPS transfers must not race with filesystem activity.
 *
 * The card is intentionally left mounted.  Physically unmounting Slot 0
 * can deinitialize shared SDMMC host state that ESP-Hosted Wi-Fi still
 * needs on Slot 1.
 *
 * Typical usage in download tasks:
 *   sd_storage_pause();              // block app SD access
 *   ... HTTPS downloads to PSRAM ... // only Slot 1 active
 *   esp_http_client_cleanup(client);
 *   vTaskDelay(2500);                // let TCP FIN flush
 *   sd_storage_resume();             // allow SD access
 *   sd_storage_ensure_dir(...);
 *   ... write PSRAM to SD ...
 */
void sd_storage_pause(void);
void sd_storage_resume(void);

/**
 * Software-level SD access guard for sustained HTTPS transfers.
 *
 * When busy=true, sd_storage_ensure_mounted() and all write functions
 * return ESP_ERR_INVALID_STATE immediately without touching the SDMMC
 * peripheral.  This prevents other tasks (weather history, LVGL
 * slideshow, file explorer) from issuing SD I/O commands while HTTPS
 * data is flowing on the SDIO link (Slot 1).
 *
 * Unlike sd_storage_pause(), this does NOT unmount the SD card or
 * deinitialise the host controller — it simply blocks new SD operations
 * at the software level so no actual SDMMC bus commands are issued
 * during active network transfers.
 *
 * Usage in download tasks:
 *   sd_storage_set_network_busy(true);   // block all SD access
 *   ... HTTPS download to PSRAM ...
 *   esp_http_client_close(client);
 *   vTaskDelay(2000);                    // let TCP FIN flush
 *   sd_storage_set_network_busy(false);  // allow SD access
 *   sd_storage_ensure_dir(...);          // safe to write now
 *   ... write PSRAM to SD ...
 */
void sd_storage_set_network_busy(bool busy);
bool sd_storage_is_network_busy(void);

#ifdef __cplusplus
}
#endif
