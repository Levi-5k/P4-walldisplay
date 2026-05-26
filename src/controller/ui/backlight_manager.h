#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BL_PRESET_DEFAULT = 0,
    BL_PRESET_AGGRESSIVE,
    BL_PRESET_SUBTLE,
    BL_PRESET_CUSTOM,
} backlight_preset_t;

typedef struct {
    bool enabled;
    uint8_t min_pct;
    uint8_t max_pct;
    uint16_t hold_s;
    backlight_preset_t preset;
} backlight_manager_config_t;

esp_err_t backlight_manager_init(void);
esp_err_t backlight_manager_apply_tuning(void);
esp_err_t backlight_manager_set_enabled(bool enabled);
bool      backlight_manager_is_enabled(void);
esp_err_t backlight_manager_set_manual_hold_minutes(uint16_t hold_min);

/* Called when the user manually adjusts brightness; engages hold timer. */
void      backlight_manager_manual_override(uint8_t pct);

/* Force re-evaluation (e.g. after weather update with new sunrise/sunset). */
void      backlight_manager_recalculate(void);

#ifdef __cplusplus
}
#endif
