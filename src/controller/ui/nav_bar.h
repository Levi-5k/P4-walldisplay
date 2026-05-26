#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nav_page_shown_cb_t)(lv_obj_t *page, void *user_data);

typedef struct {
    lv_obj_t *lights_page;
    lv_obj_t *weather_page;
    lv_obj_t *settings_page;
    nav_page_shown_cb_t page_shown_cb;
    void *page_shown_user_data;
} nav_pages_t;

lv_obj_t *nav_bar_create(lv_obj_t *parent, const nav_pages_t *pages);
void      nav_bar_show_page(lv_obj_t *page);

#ifdef __cplusplus
}
#endif
