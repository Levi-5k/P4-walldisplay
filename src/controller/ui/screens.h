#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *screen_lights_create(lv_obj_t *parent);
lv_obj_t *screen_weather_create(lv_obj_t *parent);
void      screen_weather_activate(void);
lv_obj_t *screen_settings_create(lv_obj_t *parent);
lv_obj_t *screen_timer_create(lv_obj_t *parent);
void      screen_timer_settings_create(lv_obj_t *parent);

lv_obj_t *screen_idle_create(void);

#ifdef __cplusplus
}
#endif
