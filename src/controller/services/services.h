/* Cross-cutting services used by the controller UI and background tasks. */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool wifi_supported;
	bool wifi_configured;
	bool wifi_connected;
	char wifi_ssid[33];
	char ip_addr[16];
	char wifi_detail[64];
	int wifi_disconnect_reason;

	bool location_ready;
	bool location_has_coords;
	char location_area[72];
	char timezone[48];
	int timezone_offset_sec;
	int32_t location_lat_x1e6;
	int32_t location_lon_x1e6;

	bool rs485_ready;
	bool wled_online;
	uint32_t wled_last_rx_ms;

	bool weather_configured;
	bool weather_online;
	char weather_detail[56];
	uint32_t weather_status_ms;

	bool time_sync_started;
	bool time_synced;
	char time_detail[48];
	uint32_t time_sync_status_ms;

	bool audio_ready;
	bool fft_ready;
	bool sound_sync_ready;
} services_status_t;

typedef struct {
	char ssid[33];
	int8_t rssi;
	uint8_t channel;
	bool secure;
} wifi_scan_result_t;

esp_err_t services_status_get(services_status_t *out);
void services_status_note_weather(bool configured, bool online, const char *detail);
void services_note_weather_timezone(int32_t lat_x1e6, int32_t lon_x1e6,
									int offset_sec, const char *city, const char *country);

/* Wi-Fi station via esp_wifi_remote/esp_hosted when that stack is enabled. */
esp_err_t wifi_radio_preinit(void);
esp_err_t wifi_sta_init(void);
esp_err_t wifi_sta_connect(const char *ssid, const char *psk);

/* Tear down the ESP-Hosted SDIO transport, reset the C6 coprocessor via
 * GPIO, rebuild the link from scratch, and reconnect Wi-Fi.  Call this
 * after sustained HTTPS traffic to clear degraded SDIO driver state
 * (espressif/esp-hosted-mcu #167, #184).                                */
esp_err_t services_reset_wifi_link(void);

/* Exclusive lock for inbound HTTPS traffic through SDIO.
 * The ESP-Hosted SDIO driver cannot handle concurrent inbound HTTPS
 * sessions — the overlapping TLS traffic causes "Unrecoverable host
 * sdio state" crashes.  Every HTTPS consumer must hold this lock for
 * the duration of its request(s).                                       */
void services_https_lock(void);
void services_https_unlock(void);

/* Retry IP-based location lookup when weather needs coordinates after boot. */
esp_err_t services_location_request_refresh(void);

/* Blocking Wi-Fi scan. Returns up to max_results visible SSIDs sorted by the driver. */
esp_err_t wifi_scan_networks(wifi_scan_result_t *results, size_t max_results, size_t *result_count);

/* RS-485 UART1 (TX=47, RX=48), newline-delimited WLED JSON. */
esp_err_t cmd_tx_init(void);
esp_err_t cmd_tx_send_json(const char *json);
bool      cmd_tx_is_ready(void);

/* I2S mic capture (audio analysis pipeline source). */
esp_err_t audio_in_init(void);

/* FFT/loudness post-processing on captured audio. */
esp_err_t audio_fft_init(void);

/* UDP broadcast of normalized audio levels to WLED nodes. */
esp_err_t sound_sync_tx_init(void);

/* Push NVS-stored Wi-Fi credentials/base config to WLED over RS-485.
 * Safe to call again after credentials change. */
esp_err_t provision_init(void);

/* Periodic WLED heartbeat over RS-485. */
esp_err_t link_health_init(void);

#ifdef __cplusplus
}
#endif
