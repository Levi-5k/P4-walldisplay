#include "backlight_manager.h"
#include "app_config.h"
#include "backlight_pwm.h"
#include "weather/weather_state.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <time.h>
#include <math.h>

static const char *TAG = "bl_mgr";

#define EVAL_INTERVAL_MS    60000
#define RAMP_STEP_MS        50
#define FALLBACK_SUNRISE_MIN (6 * 60 + 30)
#define FALLBACK_SUNSET_MIN  (19 * 60 + 30)

static bool s_enabled;
static bool s_initialized;
static uint8_t s_min_pct = 5;
static uint8_t s_max_pct = 100;
static uint16_t s_hold_s = 600;
static uint16_t s_eval_s = EVAL_INTERVAL_MS / 1000;
static uint8_t s_ramp_s = 5;
static backlight_preset_t s_preset = BL_PRESET_DEFAULT;

static uint8_t s_target_pct;
static uint8_t s_current_pct;
static bool s_holding;
static uint64_t s_hold_until_us;
static lv_timer_t *s_eval_timer;
static lv_timer_t *s_ramp_timer;

typedef struct {
    int offset_min;
    uint8_t pct;
} curve_anchor_t;

static curve_anchor_t s_curve_default[] = {
    { .offset_min = -60,  .pct = 5  },   /* sunset+1h to sunrise-1h: night */
    { .offset_min = -60,  .pct = 5  },   /* sunrise-1h: begin dawn */
    { .offset_min = +30,  .pct = 60 },   /* sunrise+30m: morning */
    { .offset_min = -120, .pct = 100 },  /* noon-2h: midday start */
    { .offset_min = +120, .pct = 100 },  /* noon+2h: midday end */
    { .offset_min = -30,  .pct = 60 },   /* sunset-30m: afternoon end */
    { .offset_min = +60,  .pct = 5  },   /* sunset+1h: dusk end */
};
#define CURVE_ANCHORS 7

static uint8_t clamp_pct(uint8_t pct)
{
    if (pct < s_min_pct) return s_min_pct;
    if (pct > s_max_pct) return s_max_pct;
    return pct;
}

static uint32_t ramp_period_ms(void)
{
    uint32_t period = ((uint32_t)s_ramp_s * 1000u) / 100u;
    return period < 10u ? 10u : period;
}

static void load_tuning_values(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    s_min_pct = tuning.auto_brightness_min_pct;
    s_max_pct = tuning.auto_brightness_max_pct;
    s_hold_s = tuning.auto_brightness_hold_min * 60u;
    s_eval_s = tuning.auto_brightness_eval_s;
    s_ramp_s = tuning.auto_brightness_ramp_s;
}

static uint8_t lerp_pct(uint8_t a, uint8_t b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return (uint8_t)((float)a + ((float)b - (float)a) * t);
}

static void get_sun_times(int *sunrise_min, int *sunset_min, int *noon_min)
{
    weather_state_t w;
    if (weather_state_get(&w) == ESP_OK && w.valid && w.sunrise_utc && w.sunset_utc) {
        time_t sr = (time_t)w.sunrise_utc + w.tz_offset_s;
        time_t ss = (time_t)w.sunset_utc + w.tz_offset_s;
        struct tm sr_tm, ss_tm;
        gmtime_r(&sr, &sr_tm);
        gmtime_r(&ss, &ss_tm);
        *sunrise_min = sr_tm.tm_hour * 60 + sr_tm.tm_min;
        *sunset_min = ss_tm.tm_hour * 60 + ss_tm.tm_min;
    } else {
        *sunrise_min = FALLBACK_SUNRISE_MIN;
        *sunset_min = FALLBACK_SUNSET_MIN;
    }
    *noon_min = (*sunrise_min + *sunset_min) / 2;
}

static uint8_t evaluate_curve(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int now_min = lt.tm_hour * 60 + lt.tm_min;

    int sunrise_min, sunset_min, noon_min;
    get_sun_times(&sunrise_min, &sunset_min, &noon_min);

    /* Build absolute anchor times for the default curve */
    int anchors_min[CURVE_ANCHORS];
    uint8_t anchors_pct[CURVE_ANCHORS];

    anchors_min[0] = sunset_min + 60;     /* night starts */
    anchors_pct[0] = s_curve_default[0].pct;

    anchors_min[1] = sunrise_min - 60;    /* dawn starts */
    anchors_pct[1] = s_curve_default[1].pct;

    anchors_min[2] = sunrise_min + 30;    /* morning */
    anchors_pct[2] = s_curve_default[2].pct;

    anchors_min[3] = noon_min - 120;      /* midday start */
    anchors_pct[3] = s_curve_default[3].pct;

    anchors_min[4] = noon_min + 120;      /* midday end */
    anchors_pct[4] = s_curve_default[4].pct;

    anchors_min[5] = sunset_min - 30;     /* afternoon end */
    anchors_pct[5] = s_curve_default[5].pct;

    anchors_min[6] = sunset_min + 60;     /* dusk end = night */
    anchors_pct[6] = s_curve_default[6].pct;

    /* Find which segment we're in */
    if (now_min <= anchors_min[1]) {
        return clamp_pct(anchors_pct[0]);
    }
    for (int i = 1; i < CURVE_ANCHORS; i++) {
        if (now_min <= anchors_min[i]) {
            int span = anchors_min[i] - anchors_min[i - 1];
            if (span <= 0) return clamp_pct(anchors_pct[i]);
            float t = (float)(now_min - anchors_min[i - 1]) / (float)span;
            return clamp_pct(lerp_pct(anchors_pct[i - 1], anchors_pct[i], t));
        }
    }
    return clamp_pct(anchors_pct[CURVE_ANCHORS - 1]);
}

static void ramp_cb(lv_timer_t *t)
{
    (void)t;
    if (s_current_pct == s_target_pct) return;

    if (s_current_pct < s_target_pct) {
        s_current_pct++;
    } else {
        s_current_pct--;
    }
    backlight_set(s_current_pct);
}

static void eval_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_enabled) return;

    if (s_holding) {
        uint64_t now_us = esp_timer_get_time();
        if (now_us < s_hold_until_us) return;
        s_holding = false;
        ESP_LOGI(TAG, "Manual hold expired; auto-brightness resuming");
    }

    uint8_t target = evaluate_curve();
    if (target != s_target_pct) {
        s_target_pct = target;
        ESP_LOGD(TAG, "Auto-brightness target: %u%%", target);
    }
}

esp_err_t backlight_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    load_tuning_values();

    s_current_pct = backlight_get();
    s_target_pct = s_current_pct;

    s_eval_timer = lv_timer_create(eval_cb, (uint32_t)s_eval_s * 1000u, NULL);
    if (!s_eval_timer) return ESP_ERR_NO_MEM;

    s_ramp_timer = lv_timer_create(ramp_cb, ramp_period_ms(), NULL);
    if (!s_ramp_timer) {
        lv_timer_delete(s_eval_timer);
        return ESP_ERR_NO_MEM;
    }

    if (!s_enabled) {
        lv_timer_pause(s_eval_timer);
        lv_timer_pause(s_ramp_timer);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Backlight manager initialized (enabled=%d, min=%u%%, max=%u%%)",
             s_enabled, s_min_pct, s_max_pct);
    return ESP_OK;
}

esp_err_t backlight_manager_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!s_initialized) return ESP_OK;

    if (enabled) {
        lv_timer_resume(s_eval_timer);
        lv_timer_resume(s_ramp_timer);
        eval_cb(NULL);
    } else {
        lv_timer_pause(s_eval_timer);
        lv_timer_pause(s_ramp_timer);
    }
    ESP_LOGI(TAG, "Auto-brightness %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

bool backlight_manager_is_enabled(void)
{
    return s_enabled;
}

esp_err_t backlight_manager_apply_tuning(void)
{
    load_tuning_values();
    if (s_initialized) {
        if (s_eval_timer) lv_timer_set_period(s_eval_timer, (uint32_t)s_eval_s * 1000u);
        if (s_ramp_timer) lv_timer_set_period(s_ramp_timer, ramp_period_ms());
        if (s_holding) s_hold_until_us = esp_timer_get_time() + (uint64_t)s_hold_s * 1000000ULL;
        if (s_enabled && !s_holding) eval_cb(NULL);
    }
    ESP_LOGI(TAG, "Backlight tuning: min=%u%% max=%u%% eval=%us ramp=%us hold=%us",
             s_min_pct, s_max_pct, s_eval_s, s_ramp_s, s_hold_s);
    return ESP_OK;
}

esp_err_t backlight_manager_set_manual_hold_minutes(uint16_t hold_min)
{
    if (hold_min < 1) hold_min = 1;
    if (hold_min > 60) hold_min = 60;
    s_hold_s = hold_min * 60u;
    if (s_holding) s_hold_until_us = esp_timer_get_time() + (uint64_t)s_hold_s * 1000000ULL;
    ESP_LOGI(TAG, "Auto-brightness manual hold: %u min", hold_min);
    return ESP_OK;
}

void backlight_manager_manual_override(uint8_t pct)
{
    if (!s_enabled || !s_initialized) return;
    s_holding = true;
    s_hold_until_us = esp_timer_get_time() + (uint64_t)s_hold_s * 1000000ULL;
    s_current_pct = pct;
    s_target_pct = pct;
    ESP_LOGI(TAG, "Manual override: %u%%, holding for %u s", pct, s_hold_s);
}

void backlight_manager_recalculate(void)
{
    if (!s_enabled || !s_initialized || s_holding) return;
    eval_cb(NULL);
}
