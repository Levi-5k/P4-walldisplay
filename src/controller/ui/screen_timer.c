#include "screens.h"
#include "theme.h"
#include "toast.h"
#include "app_config.h"
#include "audio_library.h"
#include "audio_out.h"
#include "nav_bar.h"
#include "services.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "screen_timer";

/* Timer states */
typedef enum {
    TIMER_STATE_READY,
    TIMER_STATE_RUNNING,
    TIMER_STATE_PAUSED,
    TIMER_STATE_FINISHED
} timer_state_t;

/* State variables */
static uint32_t s_total_seconds = 0;
static uint32_t s_remaining_seconds = 0;
static timer_state_t s_state = TIMER_STATE_READY;
static lv_timer_t *s_tick_timer = NULL;
static lv_timer_t *s_anim_timer = NULL;
static bool s_blink_state = false;
static bool s_prealert_fired = false;
static bool s_alarm_repeat_armed = false;
static uint32_t s_alarm_next_play_ms = 0;
static uint32_t s_timer_deadline_ms = 0;
static uint8_t s_snooze_count = 0;
static app_tuning_config_t s_timer_cfg;

/* UI elements */
static lv_obj_t *s_timer_root = NULL;
static lv_obj_t *s_top_row = NULL;
static lv_obj_t *s_time_panel = NULL;
static lv_obj_t *s_outline_top = NULL;
static lv_obj_t *s_outline_right = NULL;
static lv_obj_t *s_outline_bottom = NULL;
static lv_obj_t *s_outline_left = NULL;
static lv_obj_t *s_outline_corner_tl = NULL;
static lv_obj_t *s_outline_corner_tr = NULL;
static lv_obj_t *s_outline_corner_br = NULL;
static lv_obj_t *s_outline_corner_bl = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_time_inner_panel = NULL;
static lv_obj_t *s_picker_panel = NULL;
static lv_obj_t *s_hour_roller = NULL;
static lv_obj_t *s_minute_roller = NULL;
static lv_obj_t *s_second_roller = NULL;
static lv_obj_t *s_history_panel = NULL;
static lv_obj_t *s_history_btns[APP_TIMER_HISTORY_COUNT];
static lv_obj_t *s_history_labels[APP_TIMER_HISTORY_COUNT];
static lv_obj_t *s_preset_row = NULL;
static lv_obj_t *s_start_btn = NULL;
static lv_obj_t *s_start_btn_label = NULL;
static lv_obj_t *s_reset_btn = NULL;
static lv_obj_t *s_reset_btn_label = NULL;
static lv_obj_t *s_repeat_btn = NULL;
static lv_obj_t *s_repeat_btn_label = NULL;
static lv_obj_t *s_snooze_btn = NULL;
static lv_obj_t *s_snooze_btn_label = NULL;
static lv_obj_t *s_quick_labels[APP_TIMER_QUICK_PRESET_COUNT];
static lv_obj_t *s_audio_library_card = NULL;
static lv_obj_t *s_audio_dropdown = NULL;
static lv_obj_t *s_audio_download_btn = NULL;
static lv_obj_t *s_audio_download_label = NULL;
static lv_obj_t *s_audio_library_status_label = NULL;
static lv_obj_t *s_audio_status_label = NULL;
static lv_obj_t *s_repeat_switch = NULL;
static lv_obj_t *s_snooze_limit_dropdown = NULL;
static lv_obj_t *s_finish_light_dropdown = NULL;
static lv_timer_t *s_audio_download_timer = NULL;
static audio_out_file_t *s_audio_files = NULL;
static size_t s_audio_file_count = 0;
static char *s_audio_options = NULL;
static char s_selected_audio_path[AUDIO_OUT_PATH_MAX];
static uint8_t s_audio_volume_pct = AUDIO_OUT_DEFAULT_VOLUME;
static bool s_audio_download_was_busy = false;
static bool s_audio_refresh_deferred = false;
static uint32_t s_picker_seconds = 0;
static bool s_picker_syncing = false;

#define TIMER_AUDIO_OPTIONS_BYTES 2200
#define TIMER_PAGE_PAD_X 0
#define TIMER_PAGE_PAD_Y 12
#define TIMER_LAYOUT_GAP 12
#define TIMER_CONTROL_GAP 16
#define TIMER_TIME_PANEL_W 500
#define TIMER_TIME_PANEL_H 224
#define TIMER_TOP_ROW_H 248
#define TIMER_IDLE_TOP_ROW_H 304
#define TIMER_OUTLINE_W 20
#define TIMER_TIME_PANEL_RADIUS 28
#define TIMER_TIME_INNER_PAD TIMER_OUTLINE_W
#define TIMER_TIME_INNER_RADIUS 14
#define TIMER_OUTLINE_CORNER_SIZE (TIMER_TIME_PANEL_RADIUS * 2)
#define TIMER_CORNER_GAUGE_MAX 1000
#define TIMER_PICKER_PANEL_W 460
#define TIMER_PICKER_PANEL_H 292
#define TIMER_HISTORY_PANEL_W 208
#define TIMER_HISTORY_BTN_H 64
#define TIMER_HISTORY_GAP 12
#define TIMER_PICKER_ROLLER_W 140
#define TIMER_PICKER_VISIBLE_ROWS 7
#define TIMER_PICKER_MAX_SECONDS APP_TIMER_MAX_SECONDS
#define TIMER_PICKER_MAX_HOURS (TIMER_PICKER_MAX_SECONDS / 3600u)
#define TIMER_PRESET_BTN_W 156
#define TIMER_PRESET_BTN_H 68
#define TIMER_ACTION_ROW_H 112
#define TIMER_ACTION_BTN_W 324
#define TIMER_ACTION_BTN_H 104
#define TIMER_FINISHED_BTN_W 236
#define TIMER_SNOOZE_BTN_W 156

static void timer_audio_refresh_files(bool show_toast);
static void timer_refresh_quick_labels(void);
static void timer_refresh_history_labels(void);
static uint32_t timer_picker_current_seconds(void);
static void timer_picker_set_seconds(uint32_t seconds, bool animate);
static void timer_history_record(uint32_t seconds);
static void update_time_display(void);
static void update_buttons(void);
static void timer_update_layout(void);

typedef struct {
    int step;
    int vmin;
    int vmax;
    char suffix[8];
    void (*apply)(int value);
    int (*read)(void);
    lv_obj_t *value_label;
} timer_stepper_ctx_t;

#define TIMER_SETTINGS_STEPPER_MAX 10
static timer_stepper_ctx_t s_timer_steppers[TIMER_SETTINGS_STEPPER_MAX];
static int s_timer_stepper_count;

static uint32_t timer_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool timer_time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void timer_shadow_set_if_changed(lv_obj_t *obj, int32_t width, lv_color_t color, lv_opa_t opa)
{
    if (!obj) return;
    if (lv_obj_get_style_shadow_width(obj, LV_PART_MAIN) != width) {
        lv_obj_set_style_shadow_width(obj, width, LV_PART_MAIN);
    }
    if (!lv_color_eq(lv_obj_get_style_shadow_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_shadow_color(obj, color, LV_PART_MAIN);
    }
    if (lv_obj_get_style_shadow_opa(obj, LV_PART_MAIN) != opa) {
        lv_obj_set_style_shadow_opa(obj, opa, LV_PART_MAIN);
    }
}

static void timer_scale_set_if_changed(lv_obj_t *obj, int32_t scale)
{
    if (!obj) return;
    if (lv_obj_get_style_transform_scale_x(obj, LV_PART_MAIN) != scale ||
        lv_obj_get_style_transform_scale_y(obj, LV_PART_MAIN) != scale) {
        lv_obj_set_style_transform_scale(obj, scale, LV_PART_MAIN);
    }
}

static void timer_outline_segment_set(lv_obj_t *obj, lv_coord_t x, lv_coord_t y,
                                      lv_coord_t w, lv_coord_t h)
{
    if (!obj) return;
    if (w <= 0 || h <= 0) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
}

static void timer_outline_corner_value_set(lv_obj_t *obj, uint32_t value)
{
    if (!obj) return;
    if (value > TIMER_CORNER_GAUGE_MAX) value = TIMER_CORNER_GAUGE_MAX;
    lv_arc_set_value(obj, (int32_t)value);
}

static void timer_outline_corner_width_set(int32_t width)
{
    lv_obj_t *corners[] = {s_outline_corner_tl, s_outline_corner_tr, s_outline_corner_br, s_outline_corner_bl};
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        if (!corners[i]) continue;
        if (lv_obj_get_style_arc_width(corners[i], LV_PART_INDICATOR) != width) {
            lv_obj_set_style_arc_width(corners[i], width, LV_PART_INDICATOR);
        }
    }
}

static void timer_outline_set_color(lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *segments[] = {s_outline_top, s_outline_right, s_outline_bottom, s_outline_left};
    for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
        if (!segments[i]) continue;
        lv_obj_set_style_bg_color(segments[i], color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(segments[i], opa, LV_PART_MAIN);
    }

    lv_obj_t *corners[] = {s_outline_corner_tl, s_outline_corner_tr, s_outline_corner_br, s_outline_corner_bl};
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        if (!corners[i]) continue;
        lv_obj_set_style_arc_color(corners[i], color, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(corners[i], opa, LV_PART_INDICATOR);
    }
}

static uint32_t timer_outline_piece(uint32_t elapsed, uint32_t start, uint32_t length,
                                    uint32_t *offset)
{
    if (offset) *offset = 0;
    if (length == 0 || elapsed >= start + length) return 0;
    if (elapsed <= start) return length;
    uint32_t used = elapsed - start;
    if (offset) *offset = used;
    return length - used;
}

static uint32_t timer_outline_corner_gauge_value(uint32_t elapsed, uint32_t start, uint32_t length)
{
    if (length == 0 || elapsed >= start + length) return 0;
    if (elapsed <= start) return TIMER_CORNER_GAUGE_MAX;
    uint32_t remaining = length - (elapsed - start);
    return (uint32_t)(((uint64_t)TIMER_CORNER_GAUGE_MAX * remaining + length - 1u) / length);
}

static uint32_t timer_remaining_ms(void)
{
    if (s_state == TIMER_STATE_RUNNING && s_timer_deadline_ms) {
        uint32_t now = timer_now_ms();
        return timer_time_reached(now, s_timer_deadline_ms) ? 0 : s_timer_deadline_ms - now;
    }
    return s_remaining_seconds * 1000u;
}

static uint32_t timer_seconds_ceil(uint32_t ms)
{
    return (ms + 999u) / 1000u;
}

static void timer_outline_set_progress_ms(uint32_t remaining_ms, uint32_t total_ms)
{
    if (!s_time_panel || !s_outline_top) return;

    const uint32_t panel_w = TIMER_TIME_PANEL_W;
    const uint32_t panel_h = TIMER_TIME_PANEL_H;
    const uint32_t outline_w = TIMER_OUTLINE_W;
    const uint32_t corner_radius = TIMER_TIME_PANEL_RADIUS;
    const uint32_t corner_len = corner_radius;
    const uint32_t top_len = panel_w - (corner_radius * 2u);
    const uint32_t right_len = panel_h - (corner_radius * 2u);
    const uint32_t bottom_len = top_len;
    const uint32_t left_len = right_len;

    const uint32_t tl_start = 0;
    const uint32_t top_start = tl_start + corner_len;
    const uint32_t tr_start = top_start + top_len;
    const uint32_t right_start = tr_start + corner_len;
    const uint32_t br_start = right_start + right_len;
    const uint32_t bottom_start = br_start + corner_len;
    const uint32_t bl_start = bottom_start + bottom_len;
    const uint32_t left_start = bl_start + corner_len;
    const uint32_t tl_end_start = left_start + left_len;
    const uint32_t perimeter = tl_end_start + corner_len;

    uint32_t lit = 0;
    if (total_ms > 0 && remaining_ms > 0) {
        if (remaining_ms > total_ms) remaining_ms = total_ms;
        lit = (uint32_t)(((uint64_t)perimeter * remaining_ms + total_ms - 1u) / total_ms);
    }
    uint32_t elapsed = perimeter > lit ? perimeter - lit : 0;

    uint32_t top_offset = 0;
    uint32_t right_offset = 0;
    uint32_t top = timer_outline_piece(elapsed, top_start, top_len, &top_offset);
    uint32_t right = timer_outline_piece(elapsed, right_start, right_len, &right_offset);
    uint32_t bottom = timer_outline_piece(elapsed, bottom_start, bottom_len, NULL);
    uint32_t left = timer_outline_piece(elapsed, left_start, left_len, NULL);
    uint32_t tl = 0;
    if (elapsed < top_start) {
        tl = timer_outline_corner_gauge_value(elapsed, tl_start, corner_len);
    } else if (elapsed >= left_start) {
        tl = timer_outline_corner_gauge_value(elapsed, tl_end_start, corner_len);
    }
    uint32_t tr = timer_outline_corner_gauge_value(elapsed, tr_start, corner_len);
    uint32_t br = timer_outline_corner_gauge_value(elapsed, br_start, corner_len);
    uint32_t bl = timer_outline_corner_gauge_value(elapsed, bl_start, corner_len);

    timer_outline_segment_set(s_outline_top, (lv_coord_t)(corner_radius + top_offset), 0,
                              (lv_coord_t)top, (lv_coord_t)outline_w);
    timer_outline_segment_set(s_outline_right, (lv_coord_t)(panel_w - outline_w),
                              (lv_coord_t)(corner_radius + right_offset),
                              (lv_coord_t)outline_w, (lv_coord_t)right);
    timer_outline_segment_set(s_outline_bottom, (lv_coord_t)corner_radius,
                              (lv_coord_t)(panel_h - outline_w),
                              (lv_coord_t)bottom, (lv_coord_t)outline_w);
    timer_outline_segment_set(s_outline_left, 0, (lv_coord_t)corner_radius,
                              (lv_coord_t)outline_w, (lv_coord_t)left);

    timer_outline_corner_value_set(s_outline_corner_tl, lit > 0 ? tl : 0);
    timer_outline_corner_value_set(s_outline_corner_tr, lit > 0 ? tr : 0);
    timer_outline_corner_value_set(s_outline_corner_br, lit > 0 ? br : 0);
    timer_outline_corner_value_set(s_outline_corner_bl, lit > 0 ? bl : 0);
}

static void timer_outline_refresh(void)
{
    if (s_state == TIMER_STATE_FINISHED) {
        timer_outline_set_progress_ms(1, 1);
        return;
    }
    timer_outline_set_progress_ms(timer_remaining_ms(), s_total_seconds * 1000u);
}

static void timer_action_btn_size(lv_obj_t *btn)
{
    if (btn) lv_obj_set_size(btn, TIMER_ACTION_BTN_W, TIMER_ACTION_BTN_H);
}

static uint32_t timer_clamp_picker_seconds(uint32_t seconds)
{
    return seconds > TIMER_PICKER_MAX_SECONDS ? TIMER_PICKER_MAX_SECONDS : seconds;
}

static bool timer_idle_picker_visible(void)
{
    return s_state == TIMER_STATE_READY && s_total_seconds == 0 && s_remaining_seconds == 0;
}

static void timer_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void timer_update_layout(void)
{
    bool idle = timer_idle_picker_visible();

    timer_set_hidden(s_time_panel, idle);
    timer_set_hidden(s_picker_panel, !idle);
    timer_set_hidden(s_history_panel, !idle);
    timer_set_hidden(s_preset_row, s_state == TIMER_STATE_FINISHED);

    if (s_top_row) {
        lv_obj_set_height(s_top_row, idle ? TIMER_IDLE_TOP_ROW_H : TIMER_TOP_ROW_H);
        lv_obj_set_flex_align(s_top_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_top_row, idle ? TIMER_CONTROL_GAP : 0, LV_PART_MAIN);
    }
}

static lv_obj_t *timer_outline_segment_create(lv_obj_t *parent)
{
    lv_obj_t *segment = lv_obj_create(parent);
    lv_obj_remove_style_all(segment);
    lv_obj_set_style_bg_color(segment, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(segment, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(segment, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(segment, true, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(segment, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(segment, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(segment, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(segment, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return segment;
}

static lv_obj_t *timer_outline_corner_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                             uint16_t start_angle, uint16_t end_angle)
{
    lv_obj_t *corner = lv_arc_create(parent);
    lv_obj_set_pos(corner, x, y);
    lv_obj_set_size(corner, TIMER_OUTLINE_CORNER_SIZE, TIMER_OUTLINE_CORNER_SIZE);
    lv_arc_set_bg_angles(corner, start_angle, end_angle);
    lv_arc_set_range(corner, 0, 1000);
    lv_arc_set_mode(corner, LV_ARC_MODE_REVERSE);
    lv_arc_set_value(corner, 0);
    lv_obj_set_style_bg_opa(corner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(corner, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(corner, TIMER_OUTLINE_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(corner, lv_color_hex(0x26314D), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(corner, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(corner, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(corner, TIMER_OUTLINE_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(corner, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(corner, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(corner, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(corner, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(corner, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(corner, 0, LV_PART_KNOB);
    lv_obj_clear_flag(corner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return corner;
}

static void timer_audio_update_status(const char *text)
{
    if (s_audio_status_label) lv_label_set_text(s_audio_status_label, text ? text : "");
}

static void timer_audio_update_library_status(const char *text)
{
    if (s_audio_library_status_label) lv_label_set_text(s_audio_library_status_label, text ? text : "");
}

static void timer_audio_set_library_visible(bool visible)
{
    if (!s_audio_library_card) return;
    bool hidden = lv_obj_has_flag(s_audio_library_card, LV_OBJ_FLAG_HIDDEN);
    if (visible && hidden) {
        lv_obj_remove_flag(s_audio_library_card, LV_OBJ_FLAG_HIDDEN);
    } else if (!visible && !hidden) {
        lv_obj_add_flag(s_audio_library_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool timer_audio_storage_ready(void)
{
    if (!s_audio_files) {
        s_audio_files = heap_caps_calloc(AUDIO_OUT_MAX_LISTED, sizeof(*s_audio_files),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_audio_files) s_audio_files = calloc(AUDIO_OUT_MAX_LISTED, sizeof(*s_audio_files));
    }
    if (!s_audio_options) {
        s_audio_options = heap_caps_calloc(1, TIMER_AUDIO_OPTIONS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_audio_options) s_audio_options = calloc(1, TIMER_AUDIO_OPTIONS_BYTES);
    }
    return s_audio_files && s_audio_options;
}

static bool timer_audio_sd_access_allowed(void)
{
    services_status_t status;
    if (services_status_get(&status) != ESP_OK) return false;
    if (services_network_bulk_active()) return false;
    return !status.wifi_configured || status.wifi_connected;
}

static void timer_audio_update_download_button(uint8_t present, uint8_t total, bool busy)
{
    if (!s_audio_download_btn) return;

    if (busy) {
        lv_obj_add_state(s_audio_download_btn, LV_STATE_DISABLED);
        if (s_audio_download_label) lv_label_set_text(s_audio_download_label, LV_SYMBOL_DOWNLOAD " Working");
        return;
    }

    if (total > 0 && present >= total) {
        lv_obj_add_state(s_audio_download_btn, LV_STATE_DISABLED);
        if (s_audio_download_label) lv_label_set_text(s_audio_download_label, LV_SYMBOL_OK " Done");
        return;
    }

    lv_obj_remove_state(s_audio_download_btn, LV_STATE_DISABLED);
    if (s_audio_download_label) {
        uint8_t remaining = total > present ? (uint8_t)(total - present) : AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE;
        uint8_t batch = remaining < AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE ? remaining : AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE;
        char label[40];
        snprintf(label, sizeof(label), LV_SYMBOL_DOWNLOAD " Download %u", (unsigned)batch);
        lv_label_set_text(s_audio_download_label, label);
    }
}

static void timer_audio_update_prompt(void)
{
    audio_library_state_t state;
    audio_library_state_get(&state);
    char text[72];
    text[0] = '\0';

    if (state.busy) {
        timer_audio_set_library_visible(true);
        timer_audio_update_download_button(0, 0, true);
        if (state.has_progress) {
            snprintf(text, sizeof(text), "Downloading sounds %u%%", (unsigned)state.progress_pct);
        } else {
            snprintf(text, sizeof(text), "%s", state.status[0] ? state.status : "Downloading sounds");
        }
        timer_audio_update_library_status(text);
    } else {
        if (!timer_audio_sd_access_allowed()) {
            s_audio_refresh_deferred = true;
            timer_audio_set_library_visible(true);
            timer_audio_update_library_status("Audio library pending");
            timer_audio_update_download_button(0, AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE, false);
            return;
        }
        uint8_t present = 0;
        uint8_t total = 0;
        (void)audio_library_assets_present(&present, &total);
        bool all_present = total > 0 && present >= total;
        if (total > 0) {
            char library_status[48];
            snprintf(library_status, sizeof(library_status), "%u/%u sounds ready", (unsigned)present, (unsigned)total);
            timer_audio_update_library_status(library_status);
        } else {
            timer_audio_update_library_status("Audio library ready");
        }
        timer_audio_set_library_visible(!all_present);
        timer_audio_update_download_button(present, total, false);
    }
}

static const char *timer_audio_basename(const char *path)
{
    if (!path || !path[0]) return "Built-in chime";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void timer_audio_load_config(void)
{
    if (app_config_tuning_load(&s_timer_cfg) != ESP_OK) app_config_tuning_defaults(&s_timer_cfg);
    snprintf(s_selected_audio_path, sizeof(s_selected_audio_path), "%s", s_timer_cfg.timer_audio_path);
    s_audio_volume_pct = s_timer_cfg.timer_audio_volume_pct;
}

static void timer_audio_save_config(void)
{
    if (app_config_tuning_load(&s_timer_cfg) != ESP_OK) app_config_tuning_defaults(&s_timer_cfg);
    snprintf(s_timer_cfg.timer_audio_path, sizeof(s_timer_cfg.timer_audio_path), "%s", s_selected_audio_path);
    s_timer_cfg.timer_audio_volume_pct = s_audio_volume_pct;
    esp_err_t err = app_config_tuning_save(&s_timer_cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "timer audio save failed: %s", esp_err_to_name(err));
}

static void timer_settings_save_config(void)
{
    esp_err_t err = app_config_tuning_save(&s_timer_cfg);
    if (err != ESP_OK) {
        toast_show(esp_err_to_name(err));
    }
    if (app_config_tuning_load(&s_timer_cfg) != ESP_OK) app_config_tuning_defaults(&s_timer_cfg);
    snprintf(s_selected_audio_path, sizeof(s_selected_audio_path), "%s", s_timer_cfg.timer_audio_path);
    s_audio_volume_pct = s_timer_cfg.timer_audio_volume_pct;
    timer_refresh_quick_labels();
    timer_refresh_history_labels();
}

static void timer_audio_select_dropdown(uint16_t selected, bool save)
{
    if (selected == 0 || selected > s_audio_file_count) {
        s_selected_audio_path[0] = '\0';
    } else {
        snprintf(s_selected_audio_path, sizeof(s_selected_audio_path), "%s", s_audio_files[selected - 1].path);
    }
    timer_audio_update_status(timer_audio_basename(s_selected_audio_path));
    if (save) timer_audio_save_config();
}

static void timer_audio_build_options(void)
{
    if (!timer_audio_storage_ready()) return;
    size_t used = snprintf(s_audio_options, TIMER_AUDIO_OPTIONS_BYTES, "Built-in chime");
    for (size_t i = 0; i < s_audio_file_count && used < TIMER_AUDIO_OPTIONS_BYTES - 1; i++) {
        int written = snprintf(s_audio_options + used, TIMER_AUDIO_OPTIONS_BYTES - used,
                               "\n%s", s_audio_files[i].name);
        if (written <= 0) break;
        if ((size_t)written >= TIMER_AUDIO_OPTIONS_BYTES - used) {
            s_audio_options[TIMER_AUDIO_OPTIONS_BYTES - 1] = '\0';
            break;
        }
        used += (size_t)written;
    }
}

static void timer_audio_refresh_files(bool show_toast)
{
    if (!timer_audio_storage_ready()) {
        s_audio_file_count = 0;
        timer_audio_update_status("Audio list unavailable");
        timer_audio_update_prompt();
        if (show_toast) toast_show("Audio list unavailable");
        return;
    }

    if (!timer_audio_sd_access_allowed()) {
        s_audio_refresh_deferred = true;
        s_audio_file_count = 0;
        timer_audio_build_options();
        if (s_audio_dropdown) lv_dropdown_set_options(s_audio_dropdown, s_audio_options);
        timer_audio_update_status("Audio list pending");
        timer_audio_update_prompt();
        if (show_toast) toast_show("Audio list pending");
        return;
    }

    s_audio_refresh_deferred = false;

    esp_err_t err = audio_out_list_wav(s_audio_files, AUDIO_OUT_MAX_LISTED, &s_audio_file_count);
    if (err != ESP_OK) {
        s_audio_file_count = 0;
        timer_audio_build_options();
        if (s_audio_dropdown) lv_dropdown_set_options(s_audio_dropdown, s_audio_options);
        timer_audio_update_status("SD audio unavailable");
        timer_audio_update_prompt();
        if (show_toast) toast_show(audio_out_last_error());
        return;
    }

    timer_audio_build_options();
    if (s_audio_dropdown) lv_dropdown_set_options(s_audio_dropdown, s_audio_options);

    uint16_t selected = 0;
    for (size_t i = 0; i < s_audio_file_count; i++) {
        if (strcmp(s_selected_audio_path, s_audio_files[i].path) == 0) {
            selected = (uint16_t)(i + 1);
            break;
        }
    }
    if (s_audio_dropdown) lv_dropdown_set_selected(s_audio_dropdown, selected);

    if (s_audio_file_count == 0) {
        timer_audio_update_status("Built-in chime");
    } else {
        timer_audio_select_dropdown(selected, false);
    }
    timer_audio_update_prompt();
    if (show_toast) toast_show("Audio files refreshed");
}

static void timer_audio_dropdown_cb(lv_event_t *e)
{
    uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(e));
    timer_audio_select_dropdown(selected, true);
}

static void timer_audio_refresh_cb(lv_event_t *e)
{
    (void)e;
    timer_audio_refresh_files(true);
}

static void timer_audio_preview_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = s_selected_audio_path[0]
        ? audio_out_play_wav(s_selected_audio_path, s_audio_volume_pct)
        : audio_out_play_chime(s_audio_volume_pct);
    if (err != ESP_OK) toast_show(esp_err_to_name(err));
}

static void timer_audio_download_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = audio_library_download_defaults_start();
    if (err == ESP_OK) {
        s_audio_download_was_busy = true;
        timer_audio_update_status("Downloading sounds...");
        timer_audio_update_library_status("Downloading sounds...");
        timer_audio_update_prompt();
        if (s_audio_download_btn) lv_obj_add_state(s_audio_download_btn, LV_STATE_DISABLED);
    } else if (err == ESP_ERR_INVALID_STATE) {
        toast_show(audio_library_status());
    } else if (err == ESP_ERR_NOT_FOUND) {
        timer_audio_update_prompt();
        toast_show(audio_library_status());
    } else {
        toast_show(esp_err_to_name(err));
    }
}

static void timer_audio_download_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    audio_library_state_t state;
    audio_library_state_get(&state);

    if (state.busy) {
        s_audio_download_was_busy = true;
        if (s_audio_download_btn) lv_obj_add_state(s_audio_download_btn, LV_STATE_DISABLED);
        if (state.has_progress) {
            char label[64];
            snprintf(label, sizeof(label), "Downloading sounds %u%%", (unsigned)state.progress_pct);
            timer_audio_update_status(label);
        } else {
            timer_audio_update_status(state.status);
        }
        timer_audio_update_prompt();
        return;
    }

    if (s_audio_refresh_deferred && timer_audio_sd_access_allowed()) {
        timer_audio_refresh_files(false);
    }

    if (s_audio_download_btn) lv_obj_remove_state(s_audio_download_btn, LV_STATE_DISABLED);
    if (!s_audio_download_was_busy) return;

    s_audio_download_was_busy = false;
    timer_audio_refresh_files(false);
    if (!s_selected_audio_path[0] && s_audio_file_count > 0) {
        if (s_audio_dropdown) lv_dropdown_set_selected(s_audio_dropdown, 1);
        timer_audio_select_dropdown(1, true);
    }
    timer_audio_update_library_status(state.status);
    timer_audio_update_prompt();
    toast_show(state.status);
}

static void timer_audio_play_finished(void)
{
    esp_err_t err = s_selected_audio_path[0]
        ? audio_out_play_wav(s_selected_audio_path, s_audio_volume_pct)
        : audio_out_play_chime(s_audio_volume_pct);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer audio failed: %s", esp_err_to_name(err));
        (void)audio_out_play_chime(s_audio_volume_pct);
    }
}

static void timer_stop_alarm(void)
{
    s_alarm_repeat_armed = false;
    s_alarm_next_play_ms = 0;
    audio_out_stop();
}

static void timer_alarm_play_and_schedule(void)
{
    timer_audio_play_finished();
    uint32_t gap_ms = (uint32_t)s_timer_cfg.timer_repeat_gap_s * 1000u;
    s_alarm_next_play_ms = timer_now_ms() + gap_ms;
    s_alarm_repeat_armed = true;
}

static void timer_audio_repeat_if_finished(void)
{
    if (s_state != TIMER_STATE_FINISHED || !s_timer_cfg.timer_repeat_until_dismissed) return;
    if (!s_alarm_repeat_armed || audio_out_is_playing()) return;
    if (!timer_time_reached(timer_now_ms(), s_alarm_next_play_ms)) return;
    timer_alarm_play_and_schedule();
}

static void timer_apply_finish_light_action(void)
{
    switch (s_timer_cfg.timer_finish_light_action) {
        case 1:
            (void)cmd_tx_send_json("{\"on\":true,\"bri\":255,\"seg\":[{\"fx\":2,\"sx\":255,\"ix\":255,\"pal\":0,\"col\":[[255,72,0],[255,255,255],[0,0,0]]}]}");
            break;
        case 2:
            (void)cmd_tx_send_json("{\"ps\":1}");
            break;
        case 3:
            (void)cmd_tx_send_json("{\"on\":true,\"bri\":255,\"seg\":[{\"fx\":12,\"sx\":210,\"ix\":255,\"pal\":0,\"col\":[[255,70,0],[255,255,255],[0,0,0]]}]}");
            break;
        default:
            break;
    }
}

static void timer_start_duration_internal(uint32_t seconds, bool record_history)
{
    if (seconds == 0) seconds = s_timer_cfg.timer_default_seconds;
    seconds = timer_clamp_picker_seconds(seconds);
    if (record_history) timer_history_record(seconds);
    timer_stop_alarm();
    s_remaining_seconds = seconds;
    s_total_seconds = seconds;
    s_state = TIMER_STATE_RUNNING;
    s_blink_state = false;
    s_prealert_fired = false;
    s_timer_deadline_ms = timer_now_ms() + (seconds * 1000u);
    update_time_display();
    update_buttons();
}

static void timer_start_duration(uint32_t seconds)
{
    timer_start_duration_internal(seconds, false);
}

static void update_time_display(void)
{
    if (s_state == TIMER_STATE_FINISHED) {
        if (s_blink_state) {
            lv_obj_set_style_text_color(s_time_label, THEME_ACCENT_COLOR, LV_PART_MAIN);
            timer_outline_set_color(THEME_ACCENT_COLOR, LV_OPA_COVER);
        } else {
            lv_obj_set_style_text_color(s_time_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
            timer_outline_set_color(THEME_BORDER_COLOR, LV_OPA_80);
        }
        lv_label_set_text(s_time_label, "00:00");
        lv_obj_set_style_text_font(s_time_label, THEME_FONT_WX_TIME, LV_PART_MAIN);
        timer_outline_refresh();
        return;
    }

    uint32_t h = s_remaining_seconds / 3600;
    uint32_t m = (s_remaining_seconds % 3600) / 60;
    uint32_t s = s_remaining_seconds % 60;

    char buf[32];
    if (h > 0) {
        snprintf(buf, sizeof(buf), "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, h, m, s);
        lv_obj_set_style_text_font(s_time_label, THEME_FONT_WX_TIME, LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf), "%02" PRIu32 ":%02" PRIu32, m, s);
        lv_obj_set_style_text_font(s_time_label, THEME_FONT_WX_TIME, LV_PART_MAIN);
    }
    lv_label_set_text(s_time_label, buf);

    timer_outline_refresh();

    /* Standard Base Colors */
    lv_obj_set_style_text_color(s_time_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    timer_outline_set_color(THEME_PRIMARY_COLOR, s_total_seconds > 0 ? LV_OPA_COVER : LV_OPA_TRANSP);
}

static void update_buttons(void)
{
    timer_update_layout();

    bool finished = s_state == TIMER_STATE_FINISHED;
    bool idle_picker = timer_idle_picker_visible();
    uint32_t picker_seconds = idle_picker ? timer_picker_current_seconds() : 0;
    bool snooze_available = finished && s_timer_cfg.timer_snooze_limit > 0 &&
        (s_timer_cfg.timer_snooze_limit >= 10 || s_snooze_count < s_timer_cfg.timer_snooze_limit);

    if (s_repeat_btn) {
        if (finished) lv_obj_remove_flag(s_repeat_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_repeat_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_snooze_btn) {
        if (snooze_available) lv_obj_remove_flag(s_snooze_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_snooze_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (finished) {
        if (s_start_btn) lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        lv_coord_t finished_w = snooze_available ? TIMER_FINISHED_BTN_W : TIMER_ACTION_BTN_W;
        if (s_repeat_btn) lv_obj_set_size(s_repeat_btn, finished_w, TIMER_ACTION_BTN_H);
        if (s_snooze_btn) lv_obj_set_size(s_snooze_btn, TIMER_SNOOZE_BTN_W, TIMER_ACTION_BTN_H);
        if (s_reset_btn) {
            lv_obj_set_size(s_reset_btn, finished_w, TIMER_ACTION_BTN_H);
            lv_obj_remove_state(s_reset_btn, LV_STATE_DISABLED);
        }
        if (s_reset_btn_label) lv_label_set_text(s_reset_btn_label, "CLEAR");
        if (s_repeat_btn_label) lv_label_set_text(s_repeat_btn_label, LV_SYMBOL_REFRESH " REPEAT");
        const lv_font_t *finished_font = snooze_available ? THEME_FONT_TITLE : THEME_FONT_XLARGE;
        if (s_reset_btn_label) lv_obj_set_style_text_font(s_reset_btn_label, finished_font, LV_PART_MAIN);
        if (s_repeat_btn_label) lv_obj_set_style_text_font(s_repeat_btn_label, finished_font, LV_PART_MAIN);
        if (s_snooze_btn_label) lv_obj_set_style_text_font(s_snooze_btn_label, THEME_FONT_BODY, LV_PART_MAIN);
        return;
    }
    if (s_reset_btn_label) lv_obj_set_style_text_font(s_reset_btn_label, THEME_FONT_XLARGE, LV_PART_MAIN);
    if (s_repeat_btn_label) lv_obj_set_style_text_font(s_repeat_btn_label, THEME_FONT_XLARGE, LV_PART_MAIN);
    if (s_snooze_btn_label) lv_obj_set_style_text_font(s_snooze_btn_label, THEME_FONT_TITLE, LV_PART_MAIN);

    if (s_start_btn) {
        lv_obj_remove_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
        timer_action_btn_size(s_start_btn);
    }
    if (s_repeat_btn) {
        timer_scale_set_if_changed(s_repeat_btn, 256);
        timer_shadow_set_if_changed(s_repeat_btn, 0, THEME_PRIMARY_COLOR, LV_OPA_TRANSP);
    }
    if (s_reset_btn) {
        timer_action_btn_size(s_reset_btn);
        timer_scale_set_if_changed(s_reset_btn, 256);
        timer_shadow_set_if_changed(s_reset_btn, 4, lv_color_black(), LV_OPA_60);
    }
    if (s_reset_btn_label) lv_label_set_text(s_reset_btn_label, "CLEAR");

    if (s_state == TIMER_STATE_RUNNING) {
        lv_label_set_text(s_start_btn_label, LV_SYMBOL_PAUSE " STOP");
        lv_obj_set_style_bg_color(s_start_btn, THEME_ACCENT_COLOR, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_start_btn_label, LV_SYMBOL_PLAY " START");
        lv_obj_set_style_bg_color(s_start_btn, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    }

    if (s_start_btn) {
        if (idle_picker && picker_seconds == 0) lv_obj_add_state(s_start_btn, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_start_btn, LV_STATE_DISABLED);
    }

    if (s_remaining_seconds > 0 || s_total_seconds > 0 || picker_seconds > 0) {
        lv_obj_remove_state(s_reset_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_reset_btn, LV_STATE_DISABLED);
    }
}

/* 40ms/30FPS fluid breathing and glowing animation task callback */
static void anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_timer_root && lv_obj_has_flag(s_timer_root, LV_OBJ_FLAG_HIDDEN)) return;

    static uint32_t ms_count = 0;
    ms_count += 40; /* Increment animation timer counter */

    if (s_state == TIMER_STATE_RUNNING) {
        /* Premium deep sine pulse (2.0s period) */
        float freq = (float)ms_count * 0.00314f;
        float sin_val = (sinf(freq) + 1.0f) * 0.5f; /* Smooth range [0.0 - 1.0] */
        timer_outline_refresh();

        /* Dynamically morph shadow thickness and color light intensity */
        int32_t arc_shadow = 14 + (int32_t)(26.0f * sin_val); /* Breathe between 14px and 40px */
        int32_t corner_width = TIMER_OUTLINE_W - 2 + (int32_t)(4.0f * sin_val);
        int32_t btn_shadow = 8 + (int32_t)(20.0f * sin_val);  /* Start button pulse */
        lv_opa_t arc_opa = (lv_opa_t)(LV_OPA_20 + (int32_t)(35.0f * sin_val));
        lv_opa_t btn_opa = (lv_opa_t)(LV_OPA_20 + (int32_t)(30.0f * sin_val));

        if (s_time_panel) {
            timer_shadow_set_if_changed(s_time_panel, arc_shadow, THEME_PRIMARY_COLOR, arc_opa);
            timer_outline_corner_width_set(corner_width);
            timer_outline_set_color(THEME_PRIMARY_COLOR, LV_OPA_COVER);
        }

        if (s_start_btn) {
            timer_shadow_set_if_changed(s_start_btn, btn_shadow, THEME_ACCENT_COLOR, btn_opa);
        }
    } else if (s_state == TIMER_STATE_FINISHED) {
        /* Rapid, warning-style alarm visual strobe (0.6s period) */
        float freq = (float)ms_count * 0.0105f;
        float sin_val = (sinf(freq) + 1.0f) * 0.5f;

        int32_t alarm_shadow = 20 + (int32_t)(35.0f * sin_val);
        int32_t button_shadow = 16 + (int32_t)(30.0f * sin_val);
        int32_t corner_width = TIMER_OUTLINE_W + (int32_t)(5.0f * sin_val);
        int32_t scale = 256 + (int32_t)(18.0f * sin_val);
        lv_opa_t alarm_opa = (lv_opa_t)(LV_OPA_40 + (int32_t)(45.0f * sin_val));

        if (s_time_panel) {
            timer_shadow_set_if_changed(s_time_panel, alarm_shadow, THEME_ACCENT_COLOR, alarm_opa);
            timer_outline_corner_width_set(corner_width);
        }
        if (s_repeat_btn) {
            timer_scale_set_if_changed(s_repeat_btn, scale);
            timer_shadow_set_if_changed(s_repeat_btn, button_shadow, THEME_PRIMARY_COLOR, LV_OPA_60);
        }
        if (s_reset_btn) {
            timer_scale_set_if_changed(s_reset_btn, scale);
            timer_shadow_set_if_changed(s_reset_btn, button_shadow, THEME_ACCENT_COLOR, LV_OPA_60);
        }
    } else {
        /* Resting State: Calm slow breath (4.0s period) */
        float freq = (float)ms_count * 0.00157f;
        float sin_val = (sinf(freq) + 1.0f) * 0.5f;

        int32_t rest_arc_shadow = 4 + (int32_t)(10.0f * sin_val);
        int32_t rest_btn_shadow = 4 + (int32_t)(8.0f * sin_val);

        if (s_time_panel) {
            timer_shadow_set_if_changed(s_time_panel, rest_arc_shadow, theme_border_color(), LV_OPA_10);
            timer_outline_corner_width_set(TIMER_OUTLINE_W);
        }

        if (s_start_btn) {
            timer_shadow_set_if_changed(s_start_btn, rest_btn_shadow, THEME_PRIMARY_COLOR, LV_OPA_10);
        }
    }
}

static void timer_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_state == TIMER_STATE_RUNNING) {
        uint32_t remaining_ms = timer_remaining_ms();
        uint32_t next_remaining_seconds = timer_seconds_ceil(remaining_ms);
        if (next_remaining_seconds != s_remaining_seconds) {
            s_remaining_seconds = next_remaining_seconds;
            update_time_display();
        }
        if (!s_prealert_fired && s_timer_cfg.timer_prealert_s > 0 &&
            s_remaining_seconds <= s_timer_cfg.timer_prealert_s && !audio_out_is_playing()) {
            s_prealert_fired = true;
            uint8_t prealert_volume = s_audio_volume_pct > 20 ? (uint8_t)(s_audio_volume_pct / 2) : s_audio_volume_pct;
            (void)audio_out_play_chime(prealert_volume);
        }
        if (remaining_ms == 0) {
            s_state = TIMER_STATE_FINISHED;
            s_timer_deadline_ms = 0;
            s_blink_state = true;
            s_prealert_fired = true;
            if (s_timer_cfg.timer_auto_show_on_finish) nav_bar_show_page(s_timer_root);
            update_time_display();
            update_buttons();
            if (s_timer_cfg.timer_show_finish_toast) toast_show("Timer finished");
            timer_apply_finish_light_action();
            timer_alarm_play_and_schedule();
        }
    } else if (s_state == TIMER_STATE_FINISHED) {
        s_blink_state = !s_blink_state;
        update_time_display();
        timer_audio_repeat_if_finished();
    }
}

static void start_pause_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_state == TIMER_STATE_RUNNING) {
        s_remaining_seconds = timer_seconds_ceil(timer_remaining_ms());
        s_timer_deadline_ms = 0;
        s_state = TIMER_STATE_PAUSED;
    } else {
        timer_state_t previous_state = s_state;
        if (s_state == TIMER_STATE_FINISHED) timer_stop_alarm();
        if (s_remaining_seconds == 0) {
            uint32_t selected_seconds = timer_picker_current_seconds();
            if (selected_seconds == 0) {
                toast_show("Choose a time");
                update_buttons();
                return;
            }
            s_total_seconds = selected_seconds;
            s_remaining_seconds = selected_seconds;
        }
        if (previous_state == TIMER_STATE_READY) timer_history_record(s_remaining_seconds);
        s_state = TIMER_STATE_RUNNING;
        s_timer_deadline_ms = timer_now_ms() + (s_remaining_seconds * 1000u);
        s_prealert_fired = false;
    }
    update_time_display();
    update_buttons();
}

static void reset_click_cb(lv_event_t *e)
{
    (void)e;
    s_state = TIMER_STATE_READY;
    s_total_seconds = 0;
    s_remaining_seconds = 0;
    s_timer_deadline_ms = 0;
    s_blink_state = false;
    s_prealert_fired = false;
    s_snooze_count = 0;
    timer_picker_set_seconds(0, false);
    timer_stop_alarm();
    update_time_display();
    update_buttons();
}

static void repeat_click_cb(lv_event_t *e)
{
    (void)e;
    uint32_t seconds = s_total_seconds ? s_total_seconds : s_timer_cfg.timer_default_seconds;
    s_snooze_count = 0;
    timer_start_duration(seconds);
}

static void snooze_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_state != TIMER_STATE_FINISHED || s_timer_cfg.timer_snooze_limit == 0) return;
    if (s_timer_cfg.timer_snooze_limit < 10 && s_snooze_count >= s_timer_cfg.timer_snooze_limit) return;
    s_snooze_count++;
    timer_start_duration((uint32_t)s_timer_cfg.timer_snooze_min * 60u);
}

static void timer_format_duration_short(uint32_t seconds, char *buf, size_t len)
{
    if (seconds >= 3600 && seconds % 3600 == 0) {
        snprintf(buf, len, "%uh", (unsigned)(seconds / 3600));
    } else if (seconds >= 60 && seconds % 60 == 0) {
        snprintf(buf, len, "%um", (unsigned)(seconds / 60));
    } else {
        snprintf(buf, len, "%us", (unsigned)seconds);
    }
}

static void timer_format_duration_words(uint32_t seconds, char *buf, size_t len)
{
    uint32_t hours = seconds / 3600u;
    uint32_t minutes = (seconds % 3600u) / 60u;
    uint32_t secs = seconds % 60u;

    if (hours > 0 && minutes > 0) {
        snprintf(buf, len, "%u hr %u min", (unsigned)hours, (unsigned)minutes);
    } else if (hours > 0) {
        snprintf(buf, len, "%u hr", (unsigned)hours);
    } else if (minutes > 0 && secs > 0) {
        snprintf(buf, len, "%u min %u sec", (unsigned)minutes, (unsigned)secs);
    } else if (minutes > 0) {
        snprintf(buf, len, "%u min", (unsigned)minutes);
    } else {
        snprintf(buf, len, "%u sec", (unsigned)secs);
    }
}

static uint32_t timer_picker_current_seconds(void)
{
    if (s_hour_roller && s_minute_roller && s_second_roller) {
        uint32_t hours = lv_roller_get_selected(s_hour_roller);
        uint32_t minutes = lv_roller_get_selected(s_minute_roller);
        uint32_t seconds = lv_roller_get_selected(s_second_roller);
        return timer_clamp_picker_seconds((hours * 3600u) + (minutes * 60u) + seconds);
    }
    return timer_clamp_picker_seconds(s_picker_seconds);
}

static uint32_t timer_picker_raw_seconds(void)
{
    if (s_hour_roller && s_minute_roller && s_second_roller) {
        uint32_t hours = lv_roller_get_selected(s_hour_roller);
        uint32_t minutes = lv_roller_get_selected(s_minute_roller);
        uint32_t seconds = lv_roller_get_selected(s_second_roller);
        return (hours * 3600u) + (minutes * 60u) + seconds;
    }
    return s_picker_seconds;
}

static void timer_picker_set_seconds(uint32_t seconds, bool animate)
{
    seconds = timer_clamp_picker_seconds(seconds);
    s_picker_seconds = seconds;

    if (!s_hour_roller || !s_minute_roller || !s_second_roller) return;

    uint32_t hours = seconds / 3600u;
    uint32_t minutes = (seconds % 3600u) / 60u;
    uint32_t secs = seconds % 60u;
    lv_anim_enable_t anim = animate ? LV_ANIM_ON : LV_ANIM_OFF;

    s_picker_syncing = true;
    lv_roller_set_selected(s_hour_roller, hours, anim);
    lv_roller_set_selected(s_minute_roller, minutes, anim);
    lv_roller_set_selected(s_second_roller, secs, anim);
    s_picker_syncing = false;
}

static void timer_history_record(uint32_t seconds)
{
    seconds = timer_clamp_picker_seconds(seconds);
    if (seconds == 0) return;

    uint16_t value = (uint16_t)seconds;
    size_t found = APP_TIMER_HISTORY_COUNT;
    for (size_t i = 0; i < APP_TIMER_HISTORY_COUNT; i++) {
        if (s_timer_cfg.timer_history_seconds[i] == value) {
            found = i;
            break;
        }
    }

    if (found == 0) {
        timer_refresh_history_labels();
        return;
    }

    size_t shift_from = found < APP_TIMER_HISTORY_COUNT ? found : APP_TIMER_HISTORY_COUNT - 1;
    for (size_t i = shift_from; i > 0; i--) {
        s_timer_cfg.timer_history_seconds[i] = s_timer_cfg.timer_history_seconds[i - 1];
    }
    s_timer_cfg.timer_history_seconds[0] = value;

    esp_err_t err = app_config_tuning_save(&s_timer_cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "timer history save failed: %s", esp_err_to_name(err));
    timer_refresh_history_labels();
}

static void timer_refresh_history_labels(void)
{
    for (size_t i = 0; i < APP_TIMER_HISTORY_COUNT; i++) {
        if (!s_history_btns[i] || !s_history_labels[i]) continue;

        uint32_t seconds = s_timer_cfg.timer_history_seconds[i];
        if (seconds == 0) {
            lv_obj_add_state(s_history_btns[i], LV_STATE_DISABLED);
            lv_label_set_text(s_history_labels[i], "--");
            continue;
        }

        char duration[28];
        char label[40];
        timer_format_duration_words(seconds, duration, sizeof(duration));
        snprintf(label, sizeof(label), LV_SYMBOL_REFRESH " %s", duration);
        lv_label_set_text(s_history_labels[i], label);
        lv_obj_remove_state(s_history_btns[i], LV_STATE_DISABLED);
    }
}

static void timer_picker_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_picker_syncing) return;
    uint32_t raw_seconds = timer_picker_raw_seconds();
    s_picker_seconds = timer_clamp_picker_seconds(raw_seconds);
    if (raw_seconds != s_picker_seconds) timer_picker_set_seconds(s_picker_seconds, true);
    update_buttons();
}

static void timer_history_click_cb(lv_event_t *e)
{
    uint32_t index = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (index >= APP_TIMER_HISTORY_COUNT || !timer_idle_picker_visible()) return;
    uint32_t seconds = s_timer_cfg.timer_history_seconds[index];
    if (seconds == 0) return;
    timer_picker_set_seconds(seconds, true);
    update_buttons();
}

static void timer_refresh_quick_labels(void)
{
    for (size_t i = 0; i < APP_TIMER_QUICK_PRESET_COUNT; i++) {
        if (!s_quick_labels[i]) continue;
        char label[16];
        timer_format_duration_short(s_timer_cfg.timer_quick_seconds[i], label, sizeof(label));
        lv_label_set_text(s_quick_labels[i], label);
    }
}

static void quick_preset_click_cb(lv_event_t *e)
{
    uint32_t index = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (index >= APP_TIMER_QUICK_PRESET_COUNT) return;
    uint32_t seconds_to_add = s_timer_cfg.timer_quick_seconds[index];

    if (s_state == TIMER_STATE_READY && s_remaining_seconds == 0 && s_total_seconds == 0) {
        timer_start_duration_internal(seconds_to_add, true);
        return;
    }

    if (s_state == TIMER_STATE_FINISHED) {
        timer_stop_alarm();
        timer_start_duration_internal(seconds_to_add, true);
        return;
    }

    uint32_t base_remaining_ms = timer_remaining_ms();
    s_remaining_seconds = timer_seconds_ceil(base_remaining_ms) + seconds_to_add;
    s_total_seconds = s_remaining_seconds;
    s_prealert_fired = false;
    s_snooze_count = 0;
    if (s_state == TIMER_STATE_RUNNING) {
        s_timer_deadline_ms = timer_now_ms() + (s_remaining_seconds * 1000u);
    }

    update_time_display();
    update_buttons();
}

static void quick_preset_create(lv_obj_t *parent, uint32_t index)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, TIMER_PRESET_BTN_W, TIMER_PRESET_BTN_H);

    /* Sleek card styling + misty blue border accent following alaltitov's design patterns */
    theme_btn_style_secondary(btn);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x131A2D), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x273550), LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    /* Glow shadow styling on tap focus */
    lv_obj_set_style_shadow_color(btn, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 12, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    s_quick_labels[index] = lbl;
    char label[16];
    timer_format_duration_short(s_timer_cfg.timer_quick_seconds[index], label, sizeof(label));
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, quick_preset_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
}

static void timer_build_range_options(char *buf, size_t len, uint32_t max_value,
                                      const char *singular, const char *plural)
{
    size_t used = 0;
    for (uint32_t i = 0; i <= max_value && used < len; i++) {
        int written = snprintf(buf + used, len - used, "%s%u %s",
                               i == 0 ? "" : "\n",
                               (unsigned)i,
                               i == 1 ? singular : plural);
        if (written <= 0) break;
        if ((size_t)written >= len - used) {
            buf[len - 1] = '\0';
            break;
        }
        used += (size_t)written;
    }
}

static lv_obj_t *timer_picker_roller_create(lv_obj_t *parent, const char *options)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, TIMER_PICKER_VISIBLE_ROWS);
    lv_obj_set_size(roller, TIMER_PICKER_ROLLER_W, TIMER_PICKER_PANEL_H - 18);
    lv_obj_set_scrollbar_mode(roller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, lv_color_hex(0x4C5268), LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, THEME_FONT_XLARGE, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, THEME_TEXT_PRIMARY, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_SELECTED);
    lv_obj_add_event_cb(roller, timer_picker_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return roller;
}

#define TIMER_PICKER_HOUR_OPTIONS_BYTES   128
#define TIMER_PICKER_MINUTE_OPTIONS_BYTES 512
#define TIMER_PICKER_SECOND_OPTIONS_BYTES 512

static char *s_timer_hour_options;
static char *s_timer_minute_options;
static char *s_timer_second_options;

static bool timer_picker_options_ensure(void)
{
    if (s_timer_hour_options && s_timer_minute_options && s_timer_second_options) return true;

    s_timer_hour_options = heap_caps_calloc(1, TIMER_PICKER_HOUR_OPTIONS_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_timer_minute_options = heap_caps_calloc(1, TIMER_PICKER_MINUTE_OPTIONS_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_timer_second_options = heap_caps_calloc(1, TIMER_PICKER_SECOND_OPTIONS_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_timer_hour_options || !s_timer_minute_options || !s_timer_second_options) {
        free(s_timer_hour_options);
        free(s_timer_minute_options);
        free(s_timer_second_options);
        s_timer_hour_options = NULL;
        s_timer_minute_options = NULL;
        s_timer_second_options = NULL;
        ESP_LOGE(TAG, "Timer picker PSRAM option allocation failed");
        return false;
    }

    timer_build_range_options(s_timer_hour_options, TIMER_PICKER_HOUR_OPTIONS_BYTES,
                              TIMER_PICKER_MAX_HOURS, "hour", "hours");
    timer_build_range_options(s_timer_minute_options, TIMER_PICKER_MINUTE_OPTIONS_BYTES,
                              59, "min", "min");
    timer_build_range_options(s_timer_second_options, TIMER_PICKER_SECOND_OPTIONS_BYTES,
                              59, "sec", "sec");
    return true;
}

static void timer_picker_create(lv_obj_t *parent)
{
    if (!timer_picker_options_ensure()) return;

    s_picker_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_picker_panel);
    theme_style_glass_panel(s_picker_panel, 20);
    lv_obj_set_size(s_picker_panel, TIMER_PICKER_PANEL_W, TIMER_PICKER_PANEL_H);
    lv_obj_set_style_bg_color(s_picker_panel, lv_color_hex(0x06080F), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_picker_panel, lv_color_hex(0x20283E), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_picker_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_picker_panel, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_picker_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *selected_band = lv_obj_create(s_picker_panel);
    lv_obj_remove_style_all(selected_band);
    lv_obj_set_size(selected_band, TIMER_PICKER_PANEL_W - 28, 64);
    lv_obj_set_style_bg_color(selected_band, lv_color_hex(0x171A24), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(selected_band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(selected_band, 22, LV_PART_MAIN);
    lv_obj_clear_flag(selected_band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(selected_band, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *roller_row = lv_obj_create(s_picker_panel);
    lv_obj_remove_style_all(roller_row);
    lv_obj_set_size(roller_row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(roller_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(roller_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(roller_row, LV_OBJ_FLAG_SCROLLABLE);

    s_hour_roller = timer_picker_roller_create(roller_row, s_timer_hour_options);
    s_minute_roller = timer_picker_roller_create(roller_row, s_timer_minute_options);
    s_second_roller = timer_picker_roller_create(roller_row, s_timer_second_options);
}

static void timer_history_create(lv_obj_t *parent)
{
    s_history_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_history_panel);
    lv_obj_set_size(s_history_panel, TIMER_HISTORY_PANEL_W, TIMER_PICKER_PANEL_H);
    lv_obj_set_flex_flow(s_history_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_history_panel, TIMER_HISTORY_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(s_history_panel, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < APP_TIMER_HISTORY_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_history_panel);
        s_history_btns[i] = btn;
        lv_obj_set_size(btn, TIMER_HISTORY_PANEL_W, TIMER_HISTORY_BTN_H);
        theme_btn_style_secondary(btn);
        lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x131A2D), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x273550), LV_PART_MAIN);

        lv_obj_t *label = lv_label_create(btn);
        s_history_labels[i] = label;
        lv_label_set_text(label, "--");
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, TIMER_HISTORY_PANEL_W - 24);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, timer_history_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

static lv_obj_t *screen_root(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *page_root(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, TIMER_LAYOUT_GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(p, TIMER_PAGE_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(p, TIMER_PAGE_PAD_Y, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    return p;
}

static lv_obj_t *timer_settings_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 18);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    return card;
}

static void timer_stepper_update(timer_stepper_ctx_t *ctx)
{
    if (!ctx || !ctx->value_label || !ctx->read) return;
    char text[24];
    snprintf(text, sizeof(text), "%d%s", ctx->read(), ctx->suffix);
    lv_label_set_text(ctx->value_label, text);
}

static void timer_stepper_minus_cb(lv_event_t *e)
{
    timer_stepper_ctx_t *ctx = (timer_stepper_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->apply || !ctx->read) return;
    int value = ctx->read() - ctx->step;
    if (value < ctx->vmin) value = ctx->vmin;
    ctx->apply(value);
    timer_stepper_update(ctx);
}

static void timer_stepper_plus_cb(lv_event_t *e)
{
    timer_stepper_ctx_t *ctx = (timer_stepper_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->apply || !ctx->read) return;
    int value = ctx->read() + ctx->step;
    if (value > ctx->vmax) value = ctx->vmax;
    ctx->apply(value);
    timer_stepper_update(ctx);
}

static void timer_stepper_row(lv_obj_t *parent, const char *label, int step, int vmin, int vmax,
                              const char *suffix, void (*apply)(int), int (*read)(void))
{
    if (s_timer_stepper_count >= TIMER_SETTINGS_STEPPER_MAX) return;
    timer_stepper_ctx_t *ctx = &s_timer_steppers[s_timer_stepper_count++];
    ctx->step = step;
    ctx->vmin = vmin;
    ctx->vmax = vmax;
    ctx->apply = apply;
    ctx->read = read;
    snprintf(ctx->suffix, sizeof(ctx->suffix), "%s", suffix ? suffix : "");

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_obj_set_width(name, 252);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    lv_obj_t *controls = lv_obj_create(row);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, 174, 52);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(controls, 8, LV_PART_MAIN);
    lv_obj_clear_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *minus = lv_button_create(controls);
    lv_obj_set_size(minus, 46, 46);
    theme_btn_style_secondary(minus);
    lv_obj_set_style_radius(minus, 12, LV_PART_MAIN);
    lv_obj_t *minus_lbl = lv_label_create(minus);
    lv_label_set_text(minus_lbl, LV_SYMBOL_MINUS);
    lv_obj_center(minus_lbl);
    lv_obj_add_event_cb(minus, timer_stepper_minus_cb, LV_EVENT_CLICKED, ctx);

    ctx->value_label = lv_label_create(controls);
    lv_obj_set_size(ctx->value_label, 62, 46);
    lv_label_set_long_mode(ctx->value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->value_label, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->value_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_pad_top(ctx->value_label, 12, LV_PART_MAIN);

    lv_obj_t *plus = lv_button_create(controls);
    lv_obj_set_size(plus, 46, 46);
    theme_btn_style_secondary(plus);
    lv_obj_set_style_radius(plus, 12, LV_PART_MAIN);
    lv_obj_t *plus_lbl = lv_label_create(plus);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(plus_lbl);
    lv_obj_add_event_cb(plus, timer_stepper_plus_cb, LV_EVENT_CLICKED, ctx);

    timer_stepper_update(ctx);
}

static void timer_repeat_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_timer_cfg.timer_repeat_until_dismissed = lv_obj_has_state(sw, LV_STATE_CHECKED);
    timer_settings_save_config();
}

static void timer_snooze_limit_cb(lv_event_t *e)
{
    static const uint8_t values[] = {0, 1, 2, 3, 4, 5, 10};
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dd);
    if (selected >= sizeof(values)) selected = 0;
    s_timer_cfg.timer_snooze_limit = values[selected];
    timer_settings_save_config();
}

static void timer_finish_light_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    s_timer_cfg.timer_finish_light_action = (uint8_t)lv_dropdown_get_selected(dd);
    timer_settings_save_config();
}

static void timer_auto_show_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_timer_cfg.timer_auto_show_on_finish = lv_obj_has_state(sw, LV_STATE_CHECKED);
    timer_settings_save_config();
}

static void timer_finish_toast_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_timer_cfg.timer_show_finish_toast = lv_obj_has_state(sw, LV_STATE_CHECKED);
    timer_settings_save_config();
}

static uint16_t timer_snooze_limit_index(uint8_t value)
{
    switch (value) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 5;
        case 10: return 6;
        default: return 0;
    }
}

static lv_obj_t *timer_dropdown_row(lv_obj_t *parent, const char *label, const char *options, uint16_t selected,
                                    lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 58);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_obj_set_width(name, 252);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_obj_set_size(dd, 188, 48);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_DOWN);
    lv_obj_set_style_radius(dd, 12, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static lv_obj_t *timer_switch_row(lv_obj_t *parent, const char *label, bool checked, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 58);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_obj_set_width(name, 280);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 78, 40);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void timer_repeat_row(lv_obj_t *parent)
{
    s_repeat_switch = timer_switch_row(parent, "Repeat Until Dismissed",
                                       s_timer_cfg.timer_repeat_until_dismissed,
                                       timer_repeat_switch_cb);
}

static int rd_timer_vol(void) { return s_timer_cfg.timer_audio_volume_pct; }
static void ap_timer_vol(int v) { s_timer_cfg.timer_audio_volume_pct = (uint8_t)v; timer_settings_save_config(); }
static int rd_timer_gap(void) { return s_timer_cfg.timer_repeat_gap_s; }
static void ap_timer_gap(int v) { s_timer_cfg.timer_repeat_gap_s = (uint8_t)v; timer_settings_save_config(); }
static int rd_snooze_min(void) { return s_timer_cfg.timer_snooze_min; }
static void ap_snooze_min(int v) { s_timer_cfg.timer_snooze_min = (uint8_t)v; timer_settings_save_config(); }
static int rd_default_s(void) { return s_timer_cfg.timer_default_seconds; }
static void ap_default_s(int v) { s_timer_cfg.timer_default_seconds = (uint16_t)v; timer_settings_save_config(); }
static int rd_prealert_s(void) { return s_timer_cfg.timer_prealert_s; }
static void ap_prealert_s(int v) { s_timer_cfg.timer_prealert_s = (uint16_t)v; timer_settings_save_config(); }
static int rd_quick_1(void) { return s_timer_cfg.timer_quick_seconds[0]; }
static int rd_quick_2(void) { return s_timer_cfg.timer_quick_seconds[1]; }
static int rd_quick_3(void) { return s_timer_cfg.timer_quick_seconds[2]; }
static int rd_quick_4(void) { return s_timer_cfg.timer_quick_seconds[3]; }
static void ap_quick_1(int v) { s_timer_cfg.timer_quick_seconds[0] = (uint16_t)v; timer_settings_save_config(); }
static void ap_quick_2(int v) { s_timer_cfg.timer_quick_seconds[1] = (uint16_t)v; timer_settings_save_config(); }
static void ap_quick_3(int v) { s_timer_cfg.timer_quick_seconds[2] = (uint16_t)v; timer_settings_save_config(); }
static void ap_quick_4(int v) { s_timer_cfg.timer_quick_seconds[3] = (uint16_t)v; timer_settings_save_config(); }

void screen_timer_settings_create(lv_obj_t *parent)
{
    timer_audio_load_config();
    s_timer_stepper_count = 0;

    s_audio_library_card = timer_settings_card(parent, LV_SYMBOL_DOWNLOAD " Sound Library");

    s_audio_library_status_label = lv_label_create(s_audio_library_card);
    lv_label_set_text(s_audio_library_status_label, "Audio library ready");
    lv_obj_set_width(s_audio_library_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_audio_library_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_audio_library_status_label, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_audio_library_status_label, THEME_TEXT_MUTED, LV_PART_MAIN);

    lv_obj_t *download_row = lv_obj_create(s_audio_library_card);
    lv_obj_remove_style_all(download_row);
    lv_obj_set_size(download_row, LV_PCT(100), 60);
    lv_obj_set_flex_flow(download_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(download_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(download_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *download_caption = lv_label_create(download_row);
    lv_label_set_text(download_caption, "Mixkit alarm pack");
    lv_obj_set_width(download_caption, 260);
    lv_label_set_long_mode(download_caption, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(download_caption, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(download_caption, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    s_audio_download_btn = lv_button_create(download_row);
    lv_obj_set_size(s_audio_download_btn, 154, 54);
    theme_btn_style_primary(s_audio_download_btn);
    lv_obj_set_style_radius(s_audio_download_btn, 14, LV_PART_MAIN);
    s_audio_download_label = lv_label_create(s_audio_download_btn);
    lv_label_set_text(s_audio_download_label, LV_SYMBOL_DOWNLOAD " Download 10");
    lv_obj_set_style_text_font(s_audio_download_label, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(s_audio_download_label);
    lv_obj_add_event_cb(s_audio_download_btn, timer_audio_download_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *audio_card = timer_settings_card(parent, LV_SYMBOL_AUDIO " Alarm Sound");

    s_audio_dropdown = lv_dropdown_create(audio_card);
    lv_obj_set_size(s_audio_dropdown, LV_PCT(100), 48);
    lv_dropdown_set_symbol(s_audio_dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(s_audio_dropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_text_font(s_audio_dropdown, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_radius(s_audio_dropdown, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(s_audio_dropdown, timer_audio_dropdown_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tool_row = lv_obj_create(audio_card);
    lv_obj_remove_style_all(tool_row);
    lv_obj_set_size(tool_row, LV_PCT(100), 54);
    lv_obj_set_flex_flow(tool_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tool_row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(tool_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *refresh_btn = lv_button_create(tool_row);
    lv_obj_set_size(refresh_btn, 54, 54);
    theme_btn_style_secondary(refresh_btn);
    lv_obj_set_style_radius(refresh_btn, 14, LV_PART_MAIN);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(refresh_lbl, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_center(refresh_lbl);
    lv_obj_add_event_cb(refresh_btn, timer_audio_refresh_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *preview_btn = lv_button_create(tool_row);
    lv_obj_set_size(preview_btn, 160, 54);
    theme_btn_style_primary(preview_btn);
    lv_obj_set_style_radius(preview_btn, 14, LV_PART_MAIN);
    lv_obj_t *preview_lbl = lv_label_create(preview_btn);
    lv_label_set_text(preview_lbl, LV_SYMBOL_PLAY " Preview");
    lv_obj_set_style_text_font(preview_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(preview_lbl);
    lv_obj_add_event_cb(preview_btn, timer_audio_preview_cb, LV_EVENT_CLICKED, NULL);

    timer_stepper_row(audio_card, "Alarm Volume", 5, 0, 100, "%", ap_timer_vol, rd_timer_vol);

    s_audio_status_label = lv_label_create(audio_card);
    lv_label_set_text(s_audio_status_label, "Built-in chime");
    lv_obj_set_width(s_audio_status_label, LV_PCT(100));
    lv_label_set_long_mode(s_audio_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_audio_status_label, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_audio_status_label, THEME_TEXT_MUTED, LV_PART_MAIN);

    lv_obj_t *behavior_card = timer_settings_card(parent, LV_SYMBOL_SETTINGS " Alarm Behavior");
    timer_repeat_row(behavior_card);
    (void)timer_switch_row(behavior_card, "Open Timer On Finish",
                           s_timer_cfg.timer_auto_show_on_finish,
                           timer_auto_show_cb);
    (void)timer_switch_row(behavior_card, "Show Finish Toast",
                           s_timer_cfg.timer_show_finish_toast,
                           timer_finish_toast_cb);
    timer_stepper_row(behavior_card, "Repeat Gap", 1, 0, 30, "s", ap_timer_gap, rd_timer_gap);
    timer_stepper_row(behavior_card, "Pre-Alert", 5, 0, 300, "s", ap_prealert_s, rd_prealert_s);
    s_finish_light_dropdown = timer_dropdown_row(behavior_card, "Finish Light Action",
                                                "None\nFlash lights\nWLED preset 1\nPulse glow",
                                                s_timer_cfg.timer_finish_light_action, timer_finish_light_cb);

    lv_obj_t *snooze_card = timer_settings_card(parent, LV_SYMBOL_BELL " Snooze");
    timer_stepper_row(snooze_card, "Snooze Duration", 1, 1, 30, "m", ap_snooze_min, rd_snooze_min);
    s_snooze_limit_dropdown = timer_dropdown_row(snooze_card, "Snooze Limit",
                                                "Off\n1\n2\n3\n4\n5\nUnlimited",
                                                timer_snooze_limit_index(s_timer_cfg.timer_snooze_limit),
                                                timer_snooze_limit_cb);

    lv_obj_t *duration_card = timer_settings_card(parent, LV_SYMBOL_LIST " Durations");
    timer_stepper_row(duration_card, "Default Timer", 30, 30, APP_TIMER_MAX_SECONDS, "s", ap_default_s, rd_default_s);
    timer_stepper_row(duration_card, "Quick Preset 1", 10, 10, APP_TIMER_MAX_SECONDS, "s", ap_quick_1, rd_quick_1);
    timer_stepper_row(duration_card, "Quick Preset 2", 10, 10, APP_TIMER_MAX_SECONDS, "s", ap_quick_2, rd_quick_2);
    timer_stepper_row(duration_card, "Quick Preset 3", 30, 10, APP_TIMER_MAX_SECONDS, "s", ap_quick_3, rd_quick_3);
    timer_stepper_row(duration_card, "Quick Preset 4", 30, 10, APP_TIMER_MAX_SECONDS, "s", ap_quick_4, rd_quick_4);

    timer_audio_refresh_files(false);
    timer_audio_update_prompt();
}

lv_obj_t *screen_timer_create(lv_obj_t *parent)
{
    timer_audio_load_config();
    memset(s_quick_labels, 0, sizeof(s_quick_labels));
    memset(s_history_btns, 0, sizeof(s_history_btns));
    memset(s_history_labels, 0, sizeof(s_history_labels));

    lv_obj_t *root = screen_root(parent);
    s_timer_root = root;
    lv_obj_t *main_page = page_root(root);

    s_top_row = lv_obj_create(main_page);
    lv_obj_remove_style_all(s_top_row);
    lv_obj_set_size(s_top_row, LV_PCT(100), TIMER_TOP_ROW_H);
    lv_obj_set_flex_flow(s_top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_top_row, TIMER_CONTROL_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(s_top_row, LV_OBJ_FLAG_SCROLLABLE);

    s_time_panel = lv_obj_create(s_top_row);
    lv_obj_remove_style_all(s_time_panel);
    theme_style_glass_panel(s_time_panel, 8);
    lv_obj_set_size(s_time_panel, TIMER_TIME_PANEL_W, TIMER_TIME_PANEL_H);
    lv_obj_set_style_bg_color(s_time_panel, lv_color_hex(0x151B2E), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_time_panel, lv_color_hex(0x303A59), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_time_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_time_panel, TIMER_TIME_PANEL_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_time_panel, true, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_time_panel, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_time_panel, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_time_panel, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s_time_panel, 4, LV_PART_MAIN);
    lv_obj_clear_flag(s_time_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_outline_top = timer_outline_segment_create(s_time_panel);
    s_outline_right = timer_outline_segment_create(s_time_panel);
    s_outline_bottom = timer_outline_segment_create(s_time_panel);
    s_outline_left = timer_outline_segment_create(s_time_panel);
    s_outline_corner_tl = timer_outline_corner_create(s_time_panel, 0, 0, 180, 270);
    s_outline_corner_tr = timer_outline_corner_create(s_time_panel,
                                                       TIMER_TIME_PANEL_W - TIMER_OUTLINE_CORNER_SIZE, 0,
                                                       270, 360);
    s_outline_corner_br = timer_outline_corner_create(s_time_panel,
                                                       TIMER_TIME_PANEL_W - TIMER_OUTLINE_CORNER_SIZE,
                                                       TIMER_TIME_PANEL_H - TIMER_OUTLINE_CORNER_SIZE,
                                                       0, 90);
    s_outline_corner_bl = timer_outline_corner_create(s_time_panel, 0,
                                                       TIMER_TIME_PANEL_H - TIMER_OUTLINE_CORNER_SIZE,
                                                       90, 180);

    s_time_inner_panel = lv_obj_create(s_time_panel);
    lv_obj_remove_style_all(s_time_inner_panel);
    lv_obj_set_pos(s_time_inner_panel, TIMER_TIME_INNER_PAD, TIMER_TIME_INNER_PAD);
    lv_obj_set_size(s_time_inner_panel,
                    TIMER_TIME_PANEL_W - (TIMER_TIME_INNER_PAD * 2),
                    TIMER_TIME_PANEL_H - (TIMER_TIME_INNER_PAD * 2));
    lv_obj_set_style_bg_color(s_time_inner_panel, lv_color_hex(0x0C1021), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_time_inner_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_time_inner_panel, TIMER_TIME_INNER_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_time_inner_panel, lv_color_hex(0x1E2740), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_time_inner_panel, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_time_inner_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s_time_inner_panel, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_time_inner_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_time_label = lv_label_create(s_time_inner_panel);
    lv_label_set_text(s_time_label, "00:00");
    lv_obj_set_width(s_time_label, TIMER_TIME_PANEL_W - (TIMER_TIME_INNER_PAD * 2) - 20);
    lv_label_set_long_mode(s_time_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_time_label, THEME_FONT_WX_TIME, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_time_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, 3);

    timer_picker_create(s_top_row);
    timer_history_create(s_top_row);

    s_preset_row = lv_obj_create(main_page);
    lv_obj_remove_style_all(s_preset_row);
    lv_obj_set_size(s_preset_row, LV_PCT(100), TIMER_PRESET_BTN_H);
    lv_obj_set_flex_flow(s_preset_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_preset_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_preset_row, TIMER_CONTROL_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(s_preset_row, LV_OBJ_FLAG_SCROLLABLE);

    quick_preset_create(s_preset_row, 0);
    quick_preset_create(s_preset_row, 1);
    quick_preset_create(s_preset_row, 2);
    quick_preset_create(s_preset_row, 3);

    lv_obj_t *action_row = lv_obj_create(main_page);
    lv_obj_remove_style_all(action_row);
    lv_obj_set_size(action_row, LV_PCT(100), TIMER_ACTION_ROW_H);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(action_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(action_row, TIMER_CONTROL_GAP, LV_PART_MAIN);
    lv_obj_clear_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

    s_start_btn = lv_button_create(action_row);
    timer_action_btn_size(s_start_btn);
    theme_btn_style_primary(s_start_btn);
    lv_obj_set_style_radius(s_start_btn, 16, LV_PART_MAIN);

    s_start_btn_label = lv_label_create(s_start_btn);
    lv_label_set_text(s_start_btn_label, LV_SYMBOL_PLAY " START");
    lv_obj_set_style_text_font(s_start_btn_label, THEME_FONT_XLARGE, LV_PART_MAIN);
    lv_obj_center(s_start_btn_label);
    lv_obj_add_event_cb(s_start_btn, start_pause_click_cb, LV_EVENT_CLICKED, NULL);

    s_repeat_btn = lv_button_create(action_row);
    timer_action_btn_size(s_repeat_btn);
    theme_btn_style_primary(s_repeat_btn);
    lv_obj_set_style_radius(s_repeat_btn, 16, LV_PART_MAIN);
    s_repeat_btn_label = lv_label_create(s_repeat_btn);
    lv_label_set_text(s_repeat_btn_label, LV_SYMBOL_REFRESH " REPEAT");
    lv_obj_set_style_text_font(s_repeat_btn_label, THEME_FONT_XLARGE, LV_PART_MAIN);
    lv_obj_center(s_repeat_btn_label);
    lv_obj_add_event_cb(s_repeat_btn, repeat_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_repeat_btn, LV_OBJ_FLAG_HIDDEN);

    s_snooze_btn = lv_button_create(action_row);
    lv_obj_set_size(s_snooze_btn, TIMER_SNOOZE_BTN_W, TIMER_ACTION_BTN_H);
    theme_btn_style_secondary(s_snooze_btn);
    lv_obj_set_style_radius(s_snooze_btn, 16, LV_PART_MAIN);
    s_snooze_btn_label = lv_label_create(s_snooze_btn);
    lv_label_set_text(s_snooze_btn_label, "SNOOZE");
    lv_obj_set_style_text_font(s_snooze_btn_label, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_center(s_snooze_btn_label);
    lv_obj_add_event_cb(s_snooze_btn, snooze_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_snooze_btn, LV_OBJ_FLAG_HIDDEN);

    s_reset_btn = lv_button_create(action_row);
    timer_action_btn_size(s_reset_btn);
    theme_btn_style_secondary(s_reset_btn);
    lv_obj_set_style_radius(s_reset_btn, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_reset_btn, lv_color_hex(0x1B2135), LV_PART_MAIN);

    s_reset_btn_label = lv_label_create(s_reset_btn);
    lv_label_set_text(s_reset_btn_label, "CLEAR");
    lv_obj_set_style_text_font(s_reset_btn_label, THEME_FONT_XLARGE, LV_PART_MAIN);
    lv_obj_center(s_reset_btn_label);
    lv_obj_add_event_cb(s_reset_btn, reset_click_cb, LV_EVENT_CLICKED, NULL);

    /* Setup countdown timing loop task hook */
    s_tick_timer = lv_timer_create(timer_tick_cb, 1000, NULL);
    s_anim_timer = lv_timer_create(anim_timer_cb, 40, NULL);
    s_audio_download_timer = lv_timer_create(timer_audio_download_poll_cb, 750, NULL);

    timer_picker_set_seconds(s_timer_cfg.timer_default_seconds, false);
    timer_refresh_history_labels();
    timer_audio_refresh_files(false);
    timer_audio_download_poll_cb(s_audio_download_timer);
    update_time_display();
    update_buttons();

    return root;
}
