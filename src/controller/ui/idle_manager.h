#pragma once
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Polls lv_display_get_inactive_time() and fades to the idle screen after
 * the NVS-backed timeout configured in led_state. */
esp_err_t idle_manager_start(void);
esp_err_t idle_manager_apply_tuning(void);
void      idle_manager_set_inhibited(bool inhibited);
void      idle_manager_dismiss_for_minutes(uint16_t minutes);

#ifdef __cplusplus
}
#endif
