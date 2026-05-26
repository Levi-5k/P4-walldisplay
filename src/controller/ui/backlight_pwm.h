#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thin wrapper over the BSP backlight. The manual Display Brightness
 * setting writes through here, and future auto-brightness can share it. */

esp_err_t backlight_set(uint8_t percent);
uint8_t   backlight_get(void);

#ifdef __cplusplus
}
#endif
