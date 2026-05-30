/*
 * ui_init.c — single-screen layout:
 *   status bar (top) | page container (flex grow) | nav bar (bottom card).
 */

#include "ui_init.h"

#include "display_bsp.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "theme.h"
#include "app_config.h"
#include "screens.h"
#include "idle_manager.h"
#include "status_bar.h"
#include "nav_bar.h"
#include "ui_background.h"

static const char *TAG = "ui_init";
static lv_obj_t *s_weather_page;
static lv_obj_t *s_timer_page;

static void main_nav_page_shown(lv_obj_t *page, void *user_data)
{
    (void)user_data;
    if (page == s_weather_page) screen_weather_activate();
    idle_manager_set_inhibited(page == s_timer_page);
}

static void build_main_ui(void)
{
    lv_obj_t *scr = lv_screen_active();

    app_theme_config_t theme_cfg;
    app_config_theme_load(&theme_cfg);
    theme_apply_config(&theme_cfg);
    theme_apply_screen(scr);
    ui_background_attach(scr);

    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    status_bar_create(scr);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_hor(content, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(content, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    nav_pages_t pages = {0};
    pages.lights_page   = screen_lights_create(content);
    pages.weather_page  = screen_weather_create(content);
    pages.timer_page    = screen_timer_create(content);
    pages.settings_page = screen_settings_create(content);
    s_weather_page = pages.weather_page;
    s_timer_page = pages.timer_page;
    pages.page_shown_cb = main_nav_page_shown;

    lv_obj_t *navwrap = lv_obj_create(scr);
    lv_obj_remove_style_all(navwrap);
    lv_obj_set_size(navwrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(navwrap, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(navwrap, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_top(navwrap, 4, LV_PART_MAIN);
    lv_obj_clear_flag(navwrap, LV_OBJ_FLAG_SCROLLABLE);
    nav_bar_create(navwrap, &pages);
}

esp_err_t ui_init(void)
{
    esp_err_t err = display_bsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display_bsp_init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (bsp_display_lock(0)) {
        build_main_ui();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "bsp_display_lock failed");
        return ESP_FAIL;
    }
    idle_manager_start();
    ESP_LOGI(TAG, "main UI built (status | pages | nav)");
    return ESP_OK;
}
