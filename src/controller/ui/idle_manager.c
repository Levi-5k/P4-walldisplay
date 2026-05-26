#include "idle_manager.h"

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "screens.h"
#include "led_state.h"
#include "app_config.h"

static const char *TAG = "idle_mgr";

static lv_obj_t *s_idle_scr;
static lv_obj_t *s_main_scr;
static bool      s_idle_active;
static lv_timer_t *s_idle_timer;

static uint32_t idle_tick_period_ms(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    return (uint32_t)tuning.idle_check_s * 1000u;
}

static void idle_tick_cb(lv_timer_t *t)
{
    (void)t;
    led_state_t ls; led_state_get(&ls);
    uint32_t timeout_ms = (uint32_t)ls.screen_timeout_s * 1000u;
    uint32_t inactive   = lv_display_get_inactive_time(NULL);
    if (!s_idle_active && inactive >= timeout_ms) {
        s_main_scr = lv_screen_active();
        if (s_idle_scr == NULL) s_idle_scr = screen_idle_create();
        lv_screen_load(s_idle_scr);
        s_idle_active = true;
        ESP_LOGI(TAG, "idle screen shown");
    } else if (s_idle_active && inactive < timeout_ms) {
        if (s_main_scr) {
            lv_screen_load(s_main_scr);
        }
        s_idle_active = false;
        ESP_LOGI(TAG, "idle dismissed");
    }
}

esp_err_t idle_manager_start(void)
{
    if (!bsp_display_lock(0)) return ESP_FAIL;
    s_idle_timer = lv_timer_create(idle_tick_cb, idle_tick_period_ms(), NULL);
    bsp_display_unlock();
    ESP_LOGI(TAG, "idle manager started");
    return ESP_OK;
}

esp_err_t idle_manager_apply_tuning(void)
{
    if (s_idle_timer) lv_timer_set_period(s_idle_timer, idle_tick_period_ms());
    return ESP_OK;
}
