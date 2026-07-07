#include "idle_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "screens.h"
#include "led_state.h"
#include "app_config.h"
#include "services.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "idle_mgr";

enum {
    WAKE_TIMER_PANEL_W = 390,
    WAKE_TIMER_PANEL_H = 144,
    WAKE_TIMER_PANEL_EXPANDED_H = 198,
    WAKE_TIMER_ARC_SIZE = 108,
    WAKE_TIMER_CLOSE_SIZE = 34,
    WAKE_TIMER_SLIDER_ROW_H = 40,
    WAKE_TIMER_MARGIN = 12,
    WAKE_TIMER_SLIDER_HEADROOM_MINUTES = 15,
};

#define WAKE_TIMER_TIME_FONT_NORMAL THEME_FONT_XLARGE
#define WAKE_TIMER_TIME_FONT_LONG   THEME_FONT_TITLE

static lv_obj_t *s_idle_scr;
static lv_obj_t *s_main_scr;
static bool      s_idle_active;
static bool      s_idle_inhibited;
static uint32_t  s_idle_snooze_until_ms;
static lv_timer_t *s_idle_timer;
static lv_timer_t *s_wake_lights_timer;
static bool s_wake_lights_timer_on;
static uint32_t s_wake_lights_started_at_ms;
static uint32_t s_wake_lights_off_at_ms;
static uint32_t s_wake_lights_duration_ms;
static bool s_wake_timer_overlay_suppressed;
static lv_obj_t *s_wake_timer_panel;
static lv_obj_t *s_wake_timer_arc;
static lv_obj_t *s_wake_timer_time;
static lv_obj_t *s_wake_timer_detail;
static lv_obj_t *s_wake_timer_slider_row;
static lv_obj_t *s_wake_timer_slider;
static bool s_wake_timer_expanded;
static bool s_wake_timer_adjusting;
static int32_t s_wake_timer_x = -1;
static int32_t s_wake_timer_y = -1;

static void wake_lights_timer_stop(void);
static void wake_lights_timer_cb(lv_timer_t *t);
static void wake_timer_update_panel(void);

static uint32_t idle_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool idle_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static int32_t clamp_i32(int32_t value, int32_t min_value, int32_t max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static lv_color_t wake_timer_urgency_color(uint32_t urgency)
{
    if (urgency >= 900u) return lv_color_hex(0xFF4D4D);
    if (urgency >= 750u) return lv_color_hex(0xFF8A1F);
    if (urgency >= 500u) return lv_color_hex(0xFFB347);
    return THEME_PRIMARY_COLOR;
}

static uint32_t wake_timer_remaining_ms(uint32_t now)
{
    if (!s_wake_lights_timer_on || idle_time_reached(now, s_wake_lights_off_at_ms)) return 0;
    return s_wake_lights_off_at_ms - now;
}

static uint16_t wake_timer_minutes_from_ms(uint32_t remaining_ms)
{
    uint32_t minutes = (remaining_ms + 59999u) / 60000u;
    if (minutes < 1u) minutes = 1u;
    if (minutes > 240u) minutes = 240u;
    return (uint16_t)minutes;
}

static uint16_t wake_timer_slider_max_from_minutes(uint16_t minutes)
{
    uint32_t max_min = minutes ? minutes : 1u;
    max_min += WAKE_TIMER_SLIDER_HEADROOM_MINUTES;
    if (max_min > 240u) max_min = 240u;
    return (uint16_t)max_min;
}

static void wake_timer_format_time(uint32_t remaining_ms, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    uint32_t remaining_s = (remaining_ms + 999u) / 1000u;
    if (remaining_s >= 3600u) {
        uint32_t hours = remaining_s / 3600u;
        uint32_t minutes = (remaining_s % 3600u) / 60u;
        uint32_t seconds = remaining_s % 60u;
        snprintf(out, out_len, "%lu:%02lu:%02lu", (unsigned long)hours,
                 (unsigned long)minutes, (unsigned long)seconds);
    } else {
        uint32_t minutes = remaining_s / 60u;
        uint32_t seconds = remaining_s % 60u;
        snprintf(out, out_len, "%02lu:%02lu", (unsigned long)minutes,
                 (unsigned long)seconds);
    }
}

static const lv_font_t *wake_timer_time_font(uint32_t remaining_ms)
{
    uint32_t remaining_s = (remaining_ms + 999u) / 1000u;
    return remaining_s >= 3600u ? WAKE_TIMER_TIME_FONT_LONG : WAKE_TIMER_TIME_FONT_NORMAL;
}

static void wake_timer_time_set(uint32_t remaining_ms)
{
    if (!s_wake_timer_time) return;
    char time_text[12];
    wake_timer_format_time(remaining_ms, time_text, sizeof(time_text));
    lv_obj_set_style_text_font(s_wake_timer_time, wake_timer_time_font(remaining_ms), LV_PART_MAIN);
    lv_label_set_text(s_wake_timer_time, time_text);
    lv_obj_center(s_wake_timer_time);
}

static void wake_timer_format_minutes(uint16_t minutes, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (minutes >= 60u) {
        snprintf(out, out_len, "%uh %02um", (unsigned)(minutes / 60u), (unsigned)(minutes % 60u));
    } else {
        snprintf(out, out_len, "%u min", (unsigned)minutes);
    }
}

static void wake_timer_slider_label_set(uint16_t minutes)
{
    if (!s_wake_timer_detail || !s_wake_timer_expanded) return;
    char text[16];
    wake_timer_format_minutes(minutes, text, sizeof(text));
    char detail[32];
    snprintf(detail, sizeof(detail), "Remaining %s", text);
    lv_label_set_text(s_wake_timer_detail, detail);
}

static void wake_timer_set_top_left_default(void)
{
    s_wake_timer_x = WAKE_TIMER_MARGIN;
    s_wake_timer_y = WAKE_TIMER_MARGIN;
}

static void wake_timer_apply_pos(lv_obj_t *obj, int32_t x, int32_t y)
{
    if (!obj) return;

    lv_display_t *display = lv_display_get_default();
    int32_t screen_w = display ? lv_display_get_horizontal_resolution(display) : 720;
    int32_t screen_h = display ? lv_display_get_vertical_resolution(display) : 720;
    lv_obj_update_layout(obj);
    int32_t max_x = screen_w - lv_obj_get_width(obj) - WAKE_TIMER_MARGIN;
    int32_t max_y = screen_h - lv_obj_get_height(obj) - WAKE_TIMER_MARGIN;
    if (max_x < WAKE_TIMER_MARGIN) max_x = WAKE_TIMER_MARGIN;
    if (max_y < WAKE_TIMER_MARGIN) max_y = WAKE_TIMER_MARGIN;

    s_wake_timer_x = clamp_i32(x, WAKE_TIMER_MARGIN, max_x);
    s_wake_timer_y = clamp_i32(y, WAKE_TIMER_MARGIN, max_y);
    lv_obj_set_pos(obj, s_wake_timer_x, s_wake_timer_y);
}

static void wake_timer_request_power_on(const char *reason)
{
    led_state_t ls;
    led_state_get(&ls);
    if (ls.power) return;

    bool used_preset = false;
    esp_err_t err = services_request_light_power(true, &used_preset);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s power-on request failed: %s", reason ? reason : "wake timer", esp_err_to_name(err));
    }
    if (used_preset) ESP_LOGI(TAG, "%s used power-on preset", reason ? reason : "wake timer");
}

static void wake_timer_dismiss(void)
{
    led_state_hold_power_on_for(0);
    wake_timer_request_power_on("wake timer dismiss");
    wake_lights_timer_stop();
    ESP_LOGI(TAG, "idle wake lights timer dismissed; lights held on");
}

static void wake_timer_close_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wake_timer_dismiss();
}

static void wake_timer_set_expanded(bool expanded)
{
    s_wake_timer_expanded = expanded;
    if (!s_wake_timer_panel) return;

    lv_obj_set_height(s_wake_timer_panel, expanded ? WAKE_TIMER_PANEL_EXPANDED_H : WAKE_TIMER_PANEL_H);
    if (s_wake_timer_slider_row) {
        if (expanded) lv_obj_clear_flag(s_wake_timer_slider_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_wake_timer_slider_row, LV_OBJ_FLAG_HIDDEN);
    }
    if (expanded && s_wake_timer_slider) {
        wake_timer_slider_label_set((uint16_t)lv_slider_get_value(s_wake_timer_slider));
    } else if (s_wake_timer_detail) {
        lv_label_set_text(s_wake_timer_detail, "Lights held on");
    }
    wake_timer_apply_pos(s_wake_timer_panel, s_wake_timer_x, s_wake_timer_y);
}

static void wake_timer_expand_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DOUBLE_CLICKED) return;
    wake_timer_set_expanded(!s_wake_timer_expanded);
}

static void wake_timer_apply_slider_minutes(uint16_t minutes)
{
    if (minutes < 1u) minutes = 1u;
    if (minutes > 240u) minutes = 240u;

    uint32_t remaining_ms = (uint32_t)minutes * 60u * 1000u;
    uint32_t now = idle_now_ms();
    s_wake_lights_started_at_ms = now;
    s_wake_lights_off_at_ms = now + remaining_ms;
    s_wake_lights_duration_ms = remaining_ms;
    led_state_hold_power_on_for(remaining_ms);
    wake_timer_request_power_on("wake timer adjust");
    wake_timer_slider_label_set(minutes);
}

static void wake_timer_commit_slider(lv_obj_t *slider)
{
    if (!slider || !s_wake_timer_adjusting) return;
    s_wake_timer_adjusting = false;
    wake_timer_apply_slider_minutes((uint16_t)lv_slider_get_value(slider));
}

static void wake_timer_slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target(e);
    uint16_t minutes = (uint16_t)lv_slider_get_value(slider);

    if (code == LV_EVENT_PRESSED) {
        s_wake_timer_adjusting = true;
        lv_obj_move_foreground(s_wake_timer_panel);
        wake_timer_slider_label_set(minutes);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (s_wake_timer_adjusting) wake_timer_slider_label_set(minutes);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_CANCEL || code == LV_EVENT_DEFOCUSED) {
        wake_timer_commit_slider(slider);
        wake_timer_update_panel();
    }
}

static void wake_timer_drag_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_obj_move_foreground(obj);
        return;
    }
    if (code != LV_EVENT_PRESSING) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    wake_timer_apply_pos(obj, lv_obj_get_x_aligned(obj) + vect.x,
                         lv_obj_get_y_aligned(obj) + vect.y);
}

static void wake_timer_delete_panel(void)
{
    if (!s_wake_timer_panel) return;
    lv_obj_delete(s_wake_timer_panel);
    s_wake_timer_panel = NULL;
    s_wake_timer_arc = NULL;
    s_wake_timer_time = NULL;
    s_wake_timer_detail = NULL;
    s_wake_timer_slider_row = NULL;
    s_wake_timer_slider = NULL;
    s_wake_timer_expanded = false;
    s_wake_timer_adjusting = false;
}

static void wake_timer_ensure_panel(void)
{
    if (s_wake_timer_panel || !s_wake_lights_timer_on) return;

    lv_obj_t *layer = lv_layer_top();
    if (!layer) return;

    s_wake_timer_panel = lv_obj_create(layer);
    lv_obj_remove_style_all(s_wake_timer_panel);
    lv_obj_set_size(s_wake_timer_panel, WAKE_TIMER_PANEL_W, WAKE_TIMER_PANEL_H);
    lv_obj_set_style_bg_color(s_wake_timer_panel, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wake_timer_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wake_timer_panel, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_wake_timer_panel, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wake_timer_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wake_timer_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wake_timer_panel, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_wake_timer_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_wake_timer_panel, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_wake_timer_panel, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_wake_timer_panel, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(s_wake_timer_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s_wake_timer_panel, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_wake_timer_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wake_timer_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_wake_timer_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_wake_timer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_wake_timer_panel, wake_timer_drag_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wake_timer_panel, wake_timer_drag_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_wake_timer_panel, wake_timer_expand_event, LV_EVENT_DOUBLE_CLICKED, NULL);

    lv_obj_t *top_row = lv_obj_create(s_wake_timer_panel);
    lv_obj_remove_style_all(top_row);
    lv_obj_set_size(top_row, LV_PCT(100), WAKE_TIMER_ARC_SIZE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_row, 18, LV_PART_MAIN);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_wake_timer_arc = lv_arc_create(top_row);
    lv_obj_set_size(s_wake_timer_arc, WAKE_TIMER_ARC_SIZE, WAKE_TIMER_ARC_SIZE);
    lv_arc_set_rotation(s_wake_timer_arc, 270);
    lv_arc_set_bg_angles(s_wake_timer_arc, 0, 360);
    lv_arc_set_range(s_wake_timer_arc, 0, 1000);
    lv_arc_set_mode(s_wake_timer_arc, LV_ARC_MODE_REVERSE);
    lv_arc_set_value(s_wake_timer_arc, 1000);
    lv_obj_set_style_arc_width(s_wake_timer_arc, 11, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_wake_timer_arc, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_wake_timer_arc, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_wake_timer_arc, 11, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_wake_timer_arc, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_wake_timer_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_wake_timer_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_wake_timer_arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(s_wake_timer_arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_wake_timer_time = lv_label_create(s_wake_timer_arc);
    lv_label_set_text(s_wake_timer_time, "--:--");
    lv_obj_set_style_text_color(s_wake_timer_time, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wake_timer_time, WAKE_TIMER_TIME_FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(s_wake_timer_time);
    lv_obj_add_event_cb(s_wake_timer_arc, wake_timer_expand_event, LV_EVENT_DOUBLE_CLICKED, NULL);

    lv_obj_t *col = lv_obj_create(top_row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(col, wake_timer_expand_event, LV_EVENT_DOUBLE_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "Light Timer");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_add_event_cb(title, wake_timer_expand_event, LV_EVENT_DOUBLE_CLICKED, NULL);

    s_wake_timer_detail = lv_label_create(col);
    lv_label_set_text(s_wake_timer_detail, "Lights held on");
    lv_obj_set_style_text_color(s_wake_timer_detail, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wake_timer_detail, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_add_event_cb(s_wake_timer_detail, wake_timer_expand_event, LV_EVENT_DOUBLE_CLICKED, NULL);

    lv_obj_t *grip = lv_label_create(top_row);
    lv_label_set_text(grip, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(grip, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(grip, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(grip, wake_timer_expand_event, LV_EVENT_DOUBLE_CLICKED, NULL);

    s_wake_timer_slider_row = lv_obj_create(s_wake_timer_panel);
    lv_obj_remove_style_all(s_wake_timer_slider_row);
    lv_obj_set_size(s_wake_timer_slider_row, LV_PCT(100), WAKE_TIMER_SLIDER_ROW_H);
    lv_obj_set_flex_flow(s_wake_timer_slider_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wake_timer_slider_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_wake_timer_slider_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wake_timer_slider_row, LV_OBJ_FLAG_HIDDEN);

    s_wake_timer_slider = lv_slider_create(s_wake_timer_slider_row);
    lv_obj_set_width(s_wake_timer_slider, LV_PCT(100));
    lv_obj_set_height(s_wake_timer_slider, 18);
    uint16_t remaining_min = wake_timer_minutes_from_ms(wake_timer_remaining_ms(idle_now_ms()));
    lv_slider_set_range(s_wake_timer_slider, 1, wake_timer_slider_max_from_minutes(remaining_min));
    lv_slider_set_value(s_wake_timer_slider, remaining_min, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_wake_timer_slider, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wake_timer_slider, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_wake_timer_slider, 9, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wake_timer_slider, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_wake_timer_slider, THEME_TEXT_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_wake_timer_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s_wake_timer_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_wake_timer_slider, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(s_wake_timer_slider, wake_timer_slider_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wake_timer_slider, wake_timer_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_wake_timer_slider, wake_timer_slider_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_wake_timer_slider, wake_timer_slider_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_wake_timer_slider, wake_timer_slider_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(s_wake_timer_slider, wake_timer_slider_event, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *close_btn = lv_button_create(s_wake_timer_panel);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(close_btn, WAKE_TIMER_CLOSE_SIZE, WAKE_TIMER_CLOSE_SIZE);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 14, -14);
    lv_obj_set_style_radius(close_btn, WAKE_TIMER_CLOSE_SIZE / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2A1720), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(close_btn, THEME_ERROR_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_opa(close_btn, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(close_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, wake_timer_close_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(close_label, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(close_label);

    if (s_wake_timer_x < 0 || s_wake_timer_y < 0) {
        wake_timer_set_top_left_default();
    }
    wake_timer_set_expanded(s_wake_timer_expanded);
    wake_timer_apply_pos(s_wake_timer_panel, s_wake_timer_x, s_wake_timer_y);
    lv_obj_move_foreground(s_wake_timer_panel);
}

static void wake_timer_apply_glow(uint32_t remaining_progress)
{
    if (remaining_progress > 1000u) remaining_progress = 1000u;
    uint32_t urgency = 1000u - remaining_progress;
    lv_color_t glow_color = wake_timer_urgency_color(urgency);
    int32_t glow_width = 14 + (int32_t)((urgency * 30u) / 1000u);
    lv_opa_t glow_opa = (lv_opa_t)(LV_OPA_30 + (int32_t)((urgency * 55u) / 1000u));
    int32_t glow_y = 4 + (int32_t)((urgency * 4u) / 1000u);

    if (s_wake_timer_panel) {
        if (!lv_color_eq(lv_obj_get_style_border_color(s_wake_timer_panel, LV_PART_MAIN), glow_color)) {
            lv_obj_set_style_border_color(s_wake_timer_panel, glow_color, LV_PART_MAIN);
        }
        if (!lv_color_eq(lv_obj_get_style_shadow_color(s_wake_timer_panel, LV_PART_MAIN), glow_color)) {
            lv_obj_set_style_shadow_color(s_wake_timer_panel, glow_color, LV_PART_MAIN);
        }
        if (lv_obj_get_style_shadow_width(s_wake_timer_panel, LV_PART_MAIN) != glow_width) {
            lv_obj_set_style_shadow_width(s_wake_timer_panel, glow_width, LV_PART_MAIN);
        }
        if (lv_obj_get_style_shadow_opa(s_wake_timer_panel, LV_PART_MAIN) != glow_opa) {
            lv_obj_set_style_shadow_opa(s_wake_timer_panel, glow_opa, LV_PART_MAIN);
        }
        if (lv_obj_get_style_shadow_offset_y(s_wake_timer_panel, LV_PART_MAIN) != glow_y) {
            lv_obj_set_style_shadow_offset_y(s_wake_timer_panel, glow_y, LV_PART_MAIN);
        }
    }
    if (s_wake_timer_arc && !lv_color_eq(lv_obj_get_style_arc_color(s_wake_timer_arc, LV_PART_INDICATOR), glow_color)) {
        lv_obj_set_style_arc_color(s_wake_timer_arc, glow_color, LV_PART_INDICATOR);
    }
}

static void wake_timer_update_panel(void)
{
    if (!s_wake_lights_timer_on) {
        wake_timer_delete_panel();
        return;
    }

    if (s_wake_timer_overlay_suppressed) {
        wake_timer_delete_panel();
        return;
    }

    wake_timer_ensure_panel();
    if (!s_wake_timer_panel) return;

    if (s_wake_timer_adjusting && s_wake_timer_slider &&
        !lv_obj_has_state(s_wake_timer_slider, LV_STATE_PRESSED)) {
        wake_timer_commit_slider(s_wake_timer_slider);
    }

    uint32_t now = idle_now_ms();
    uint32_t remaining_ms = wake_timer_remaining_ms(now);
    wake_timer_time_set(remaining_ms);

    if (s_wake_timer_slider) {
        uint16_t remaining_min = wake_timer_minutes_from_ms(remaining_ms);
        uint16_t max_min = wake_timer_slider_max_from_minutes(remaining_min);
        lv_slider_set_range(s_wake_timer_slider, 1, max_min);
        if (!s_wake_timer_adjusting) lv_slider_set_value(s_wake_timer_slider, remaining_min, LV_ANIM_OFF);
        if (s_wake_timer_expanded) {
            wake_timer_slider_label_set(s_wake_timer_adjusting ? (uint16_t)lv_slider_get_value(s_wake_timer_slider) : remaining_min);
        } else if (s_wake_timer_detail) {
            lv_label_set_text(s_wake_timer_detail, "Lights held on");
        }
    } else if (s_wake_timer_detail) {
        lv_label_set_text(s_wake_timer_detail, "Lights held on");
    }

    uint32_t remaining_progress = 0u;
    if (s_wake_lights_duration_ms) {
        remaining_progress = (remaining_ms >= s_wake_lights_duration_ms)
            ? 1000u
            : (uint32_t)(((uint64_t)remaining_ms * 1000ULL + (uint64_t)s_wake_lights_duration_ms / 2ULL) /
                         (uint64_t)s_wake_lights_duration_ms);
        if (remaining_progress > 1000u) remaining_progress = 1000u;
    }
    if (s_wake_timer_arc) {
        lv_arc_set_value(s_wake_timer_arc, (int32_t)remaining_progress);
    }
    wake_timer_apply_glow(remaining_progress);
    lv_obj_move_foreground(s_wake_timer_panel);
}

static void wake_lights_timer_stop(void)
{
    s_wake_lights_timer_on = false;
    s_wake_lights_started_at_ms = 0;
    s_wake_lights_off_at_ms = 0;
    s_wake_lights_duration_ms = 0;
    if (s_wake_lights_timer) lv_timer_pause(s_wake_lights_timer);
    wake_timer_delete_panel();
}

void idle_manager_set_light_timer_overlay_suppressed(bool suppressed)
{
    if (s_wake_timer_overlay_suppressed == suppressed) return;
    s_wake_timer_overlay_suppressed = suppressed;
    if (suppressed) wake_timer_delete_panel();
    else if (s_wake_lights_timer_on) wake_timer_update_panel();
}

void idle_manager_light_timer_start_minutes(uint16_t minutes)
{
    if (minutes < 1u) minutes = 1u;
    if (minutes > 240u) minutes = 240u;

    uint32_t duration_ms = (uint32_t)minutes * 60u * 1000u;
    uint32_t now = idle_now_ms();
    led_state_hold_power_on_for(duration_ms);
    wake_timer_request_power_on("wake timer start");

    s_wake_lights_timer_on = true;
    s_wake_lights_started_at_ms = now;
    s_wake_lights_off_at_ms = now + duration_ms;
    s_wake_lights_duration_ms = duration_ms;
    s_wake_timer_x = -1;
    s_wake_timer_y = -1;

    if (!s_wake_lights_timer) s_wake_lights_timer = lv_timer_create(wake_lights_timer_cb, 1000, NULL);
    else lv_timer_resume(s_wake_lights_timer);
    wake_timer_update_panel();
}

void idle_manager_light_timer_adjust_minutes(uint16_t minutes)
{
    if (!s_wake_lights_timer_on) {
        idle_manager_light_timer_start_minutes(minutes);
        return;
    }
    wake_timer_apply_slider_minutes(minutes);
    wake_timer_update_panel();
}

void idle_manager_light_timer_stop(bool keep_lights_on)
{
    if (keep_lights_on) {
        led_state_hold_power_on_for(0);
        wake_timer_request_power_on("wake timer stop");
    } else {
        led_state_clear_power_on_hold();
        led_state_set_power(false);
    }
    wake_lights_timer_stop();
}

void idle_manager_light_timer_get(idle_light_timer_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_wake_lights_timer_on;
    if (!s_wake_lights_timer_on) {
        out->max_minutes = 240;
        return;
    }

    uint32_t remaining_ms = wake_timer_remaining_ms(idle_now_ms());
    uint16_t remaining_minutes = wake_timer_minutes_from_ms(remaining_ms);
    out->remaining_ms = remaining_ms;
    out->duration_ms = s_wake_lights_duration_ms;
    out->remaining_minutes = remaining_minutes;
    out->max_minutes = wake_timer_slider_max_from_minutes(remaining_minutes);
}

static void wake_lights_timer_cb(lv_timer_t *t)
{
    (void)t;
    led_state_t ls;
    led_state_get(&ls);

    if (s_wake_lights_timer_on && idle_time_reached(idle_now_ms(), s_wake_lights_off_at_ms)) {
        led_state_clear_power_on_hold();
        led_state_set_power(false);
        wake_lights_timer_stop();
        ESP_LOGI(TAG, "idle wake lights timer expired");
        return;
    }

    if (!led_state_power_on_hold_active()) {
        wake_lights_timer_stop();
        return;
    }
    wake_timer_update_panel();
}

static uint32_t idle_tick_period_ms(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    return (uint32_t)tuning.idle_check_s * 1000u;
}

static void idle_start_wake_lights_hold(const app_tuning_config_t *tuning)
{
    if (!tuning) return;

    uint32_t duration_ms = tuning->idle_dismiss_lights_timer_on
        ? (uint32_t)tuning->idle_dismiss_lights_timer_min * 60u * 1000u
        : 0;
    led_state_hold_power_on_for(duration_ms);

    s_wake_lights_timer_on = tuning->idle_dismiss_lights_timer_on;
    s_wake_lights_started_at_ms = s_wake_lights_timer_on ? idle_now_ms() : 0;
    s_wake_lights_off_at_ms = s_wake_lights_timer_on ? s_wake_lights_started_at_ms + duration_ms : 0;
    s_wake_lights_duration_ms = s_wake_lights_timer_on ? duration_ms : 0;
    if (s_wake_lights_timer_on) {
        s_wake_timer_x = -1;
        s_wake_timer_y = -1;
        if (!s_wake_lights_timer) s_wake_lights_timer = lv_timer_create(wake_lights_timer_cb, 1000, NULL);
        else lv_timer_resume(s_wake_lights_timer);
        wake_timer_update_panel();
    } else {
        wake_lights_timer_stop();
    }
}

static void idle_handle_dismissed(bool wake_lights_allowed)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    if (!wake_lights_allowed) return;
    if (!tuning.idle_dismiss_lights_on) return;

    led_state_t ls;
    led_state_get(&ls);
    if (ls.power) {
        if (s_wake_lights_timer_on) wake_timer_update_panel();
        else wake_timer_delete_panel();
        ESP_LOGI(TAG, "idle dismiss lights already on; light timer skipped");
        return;
    }

    idle_start_wake_lights_hold(&tuning);
    bool used_preset = false;
    esp_err_t err = services_request_light_power(true, &used_preset);
    ESP_LOGI(TAG, "idle dismiss lights on%s", tuning.idle_dismiss_lights_timer_on ? " with timer" : "");
    if (err != ESP_OK) ESP_LOGW(TAG, "idle dismiss power preset send failed: %s", esp_err_to_name(err));
    if (used_preset) ESP_LOGI(TAG, "idle dismiss used power-on preset");
}

static bool idle_snooze_active(uint32_t now)
{
    return s_idle_snooze_until_ms && !idle_time_reached(now, s_idle_snooze_until_ms);
}

void idle_manager_dismiss_for_minutes(uint16_t minutes)
{
    if (minutes == 0) return;

    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);

    uint32_t duration_ms = (uint32_t)minutes * 60u * 1000u;
    s_idle_snooze_until_ms = idle_now_ms() + duration_ms;

    if (s_idle_active && s_main_scr) {
        lv_screen_load(s_main_scr);
        idle_handle_dismissed(tuning.idle_swipe_wake_lights_on);
        s_idle_active = false;
    }

    ESP_LOGI(TAG, "idle screen snoozed for %u min", (unsigned)minutes);
}

static void idle_tick_cb(lv_timer_t *t)
{
    (void)t;
    led_state_t ls; led_state_get(&ls);
    uint32_t timeout_ms = (uint32_t)ls.screen_timeout_s * 1000u;
    uint32_t inactive   = lv_display_get_inactive_time(NULL);
    uint32_t now = idle_now_ms();

    if (s_idle_snooze_until_ms && idle_time_reached(now, s_idle_snooze_until_ms)) {
        s_idle_snooze_until_ms = 0;
    }

    if (s_idle_inhibited && !s_idle_active) return;
    if (!s_idle_active && idle_snooze_active(now)) return;
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
        idle_handle_dismissed(true);
        s_idle_active = false;
        ESP_LOGI(TAG, "idle dismissed");
    }
}

void idle_manager_set_inhibited(bool inhibited)
{
    if (s_idle_inhibited == inhibited) return;
    s_idle_inhibited = inhibited;
    if (inhibited && s_idle_active && s_main_scr) {
        lv_screen_load(s_main_scr);
        s_idle_active = false;
        ESP_LOGI(TAG, "idle dismissed by inhibit");
    }
    ESP_LOGI(TAG, "idle %s", inhibited ? "inhibited" : "enabled");
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
