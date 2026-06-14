#pragma once
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	bool active;
	uint32_t remaining_ms;
	uint32_t duration_ms;
	uint16_t remaining_minutes;
	uint16_t max_minutes;
} idle_light_timer_state_t;

/* Polls lv_display_get_inactive_time() and fades to the idle screen after
 * the NVS-backed timeout configured in led_state. */
esp_err_t idle_manager_start(void);
esp_err_t idle_manager_apply_tuning(void);
void      idle_manager_set_inhibited(bool inhibited);
void      idle_manager_dismiss_for_minutes(uint16_t minutes);
void      idle_manager_light_timer_start_minutes(uint16_t minutes);
void      idle_manager_light_timer_adjust_minutes(uint16_t minutes);
void      idle_manager_light_timer_stop(bool keep_lights_on);
void      idle_manager_light_timer_get(idle_light_timer_state_t *out);
void      idle_manager_set_light_timer_overlay_suppressed(bool suppressed);

#ifdef __cplusplus
}
#endif
