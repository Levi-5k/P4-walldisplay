/*
 * ui_init.h
 *
 * Bootstraps LVGL 9 on top of esp_lvgl_port, creates the main pages,
 * and starts the idle manager.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

#ifdef __cplusplus
}
#endif
