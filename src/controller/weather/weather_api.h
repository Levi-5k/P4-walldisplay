/* OpenWeatherMap fetcher backed by NVS configuration.
 *
 * NVS namespace: "weather"
 *   api_key       string
 *
 * Location is supplied by the boot IP-location check in services.c.
 * Optional build flags may also define WALLDISPLAY_WEATHER_API_KEY.
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spawns a periodic fetcher task. It waits for Wi-Fi and never fakes data. */
esp_err_t weather_api_init(void);

/* Wake the fetcher after settings change. Starts it if needed. */
esp_err_t weather_api_request_refresh(void);

#ifdef __cplusplus
}
#endif
