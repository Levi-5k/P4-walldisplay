/* Top status bar — live icons for Wi-Fi, WLED link, RS-485, weather,
 * audio sync, and display brightness. */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the status bar under parent. Height ~32 px. Returns root. */
lv_obj_t *status_bar_create(lv_obj_t *parent);
void status_bar_apply_tuning(void);

#ifdef __cplusplus
}
#endif
