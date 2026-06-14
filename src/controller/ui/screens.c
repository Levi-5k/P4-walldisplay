/*
 * screens.c — Lights / Weather / Settings + idle ambient weather.
 *
 * Layout under ui_init's content container is a flex column page per tab
 * with rounded "deep" cards. Real state lives in led_state and
 * weather_state; UI subscribes to be notified of changes.
 */

#include "screens.h"
#include "theme.h"
#include "toast.h"
#include "ui_background.h"
#include "wled_effect_catalog.h"
#include "wled_palette_catalog.h"
#include "fonts/ui_icons.h"
#include "fonts/weather_icons.h"
#include "app_config.h"
#include "audio_fft.h"
#include "audio_in.h"
#include "backlight_manager.h"
#include "idle_manager.h"
#include "led_state.h"
#include "services.h"
#include "sd_storage.h"
#include "sound_sync_tx.h"
#include "status_bar.h"
#include "wled_state.h"
#include "weather_api.h"
#include "weather_history.h"
#include "weather_state.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LV_USE_QRCODE
#include "libs/qrcode/qrcodegen.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "screens";
static const char *WLED_WEB_URL = "http://wled-86box.local";

/* ---------- primitives ---------- */

static lv_obj_t *page_root(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    return p;
}

static lv_obj_t *deep_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    theme_style_glass_panel(c, 28);
    lv_obj_set_style_pad_all(c, 22, LV_PART_MAIN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *wled_qr_placeholder_create(lv_obj_t *parent)
{
    lv_obj_t *qr = lv_obj_create(parent);
    lv_obj_remove_style_all(qr);
    lv_obj_set_size(qr, 168, 168);
    lv_obj_set_style_bg_opa(qr, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(qr, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_color(qr, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(qr, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(qr, 8, LV_PART_MAIN);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *qr_unavailable = lv_label_create(qr);
    lv_label_set_text(qr_unavailable, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(qr_unavailable, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(qr_unavailable, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(qr_unavailable);
    return qr;
}

#if LV_USE_QRCODE
static lv_obj_t *wled_qr_grid_create(lv_obj_t *parent, const char *url)
{
    if (!url || !url[0]) return NULL;

    uint8_t *qr = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *temp = heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!qr || !temp) {
        free(qr);
        free(temp);
        return NULL;
    }
    bool ok = qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        free(qr);
        free(temp);
        return NULL;
    }

    const int32_t qr_px = 168;
    const int32_t quiet_modules = 4;
    int32_t qr_modules = qrcodegen_getSize(qr);
    if (qr_modules <= 0) {
        free(qr);
        free(temp);
        return NULL;
    }

    int32_t total_modules = qr_modules + quiet_modules * 2;
    int32_t module_px = qr_px / total_modules;
    if (module_px < 1) {
        free(qr);
        free(temp);
        return NULL;
    }
    int32_t offset = (qr_px - total_modules * module_px) / 2 + quiet_modules * module_px;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, qr_px, qr_px);
    lv_obj_set_style_bg_color(box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    for (int32_t y = 0; y < qr_modules; y++) {
        int32_t run_start = -1;
        for (int32_t x = 0; x <= qr_modules; x++) {
            bool black = x < qr_modules && qrcodegen_getModule(qr, x, y);
            if (black && run_start < 0) {
                run_start = x;
            } else if (!black && run_start >= 0) {
                lv_obj_t *bar = lv_obj_create(box);
                lv_obj_remove_style_all(bar);
                lv_obj_set_style_bg_color(bar, lv_color_hex(0x0B1020), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_pos(bar, offset + run_start * module_px, offset + y * module_px);
                lv_obj_set_size(bar, (x - run_start) * module_px, module_px);
                lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
                run_start = -1;
            }
        }
    }

    free(qr);
    free(temp);
    return box;
}
#endif

/* Map a Kelvin value to a tint roughly approximating black-body color:
 * warm (2000 K) → orange, neutral (4000 K) → white, cool (6500 K) → blue.
 * Used to color the K slider indicator. */
static lv_color_t kelvin_to_color(uint16_t k)
{
    if (k < 2000) k = 2000;
    if (k > 6500) k = 6500;

    /* Anchors chosen to mimic black-body appearance on the panel. */
    const uint16_t k0 = 2000, k1 = 4000, k2 = 6500;
    const lv_color_t c0 = lv_color_hex(0xFFA84A);
    const lv_color_t c1 = lv_color_hex(0xFFF4DD);
    const lv_color_t c2 = lv_color_hex(0xBFDFFF);

    uint8_t r0 = c0.red, g0 = c0.green, b0 = c0.blue;
    uint8_t r1 = c1.red, g1 = c1.green, b1 = c1.blue;
    uint8_t r2 = c2.red, g2 = c2.green, b2 = c2.blue;

    if (k <= k1) {
        uint32_t t = (uint32_t)(k - k0) * 255u / (k1 - k0);
        uint8_t r = (uint8_t)(r0 + (((int32_t)r1 - r0) * (int32_t)t) / 255);
        uint8_t g = (uint8_t)(g0 + (((int32_t)g1 - g0) * (int32_t)t) / 255);
        uint8_t b = (uint8_t)(b0 + (((int32_t)b1 - b0) * (int32_t)t) / 255);
        return lv_color_make(r, g, b);
    }

    uint32_t t = (uint32_t)(k - k1) * 255u / (k2 - k1);
    uint8_t r = (uint8_t)(r1 + (((int32_t)r2 - r1) * (int32_t)t) / 255);
    uint8_t g = (uint8_t)(g1 + (((int32_t)g2 - g1) * (int32_t)t) / 255);
    uint8_t b = (uint8_t)(b1 + (((int32_t)b2 - b1) * (int32_t)t) / 255);
    return lv_color_make(r, g, b);
}

static void label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *next = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, next) != 0) lv_label_set_text(label, next);
}

static bool rate_limit_ms(uint32_t *last_ms, uint32_t interval_ms)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (*last_ms && now_ms - *last_ms < interval_ms) return false;
    *last_ms = now_ms;
    return true;
}

static uint32_t ui_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool hold_active_until(uint32_t until_ms)
{
    return until_ms && (int32_t)(until_ms - ui_now_ms()) > 0;
}

static void preset_sync_labels_refresh(void);
static void preset_panels_rebuild(bool force);

static bool timer_page_hidden(lv_timer_t *timer, lv_obj_t *page)
{
    return timer && page && lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN);
}

/* ── Pull-to-refresh helper ──
 * Attach to any scrollable object. When user overscrolls at the top
 * (elastic bounce) and releases, the user_data callback fires. */
typedef void (*pull_refresh_cb_t)(void);
static void pull_refresh_scroll_end(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    int32_t scroll_y = lv_obj_get_scroll_y(obj);
    if (scroll_y < -40) {
        pull_refresh_cb_t cb = (pull_refresh_cb_t)lv_event_get_user_data(e);
        if (cb) {
            toast_show("Refreshing...");
            cb();
        }
    }
}
static void pull_refresh_enable(lv_obj_t *scrollable, pull_refresh_cb_t cb)
{
    lv_obj_add_flag(scrollable, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(scrollable, pull_refresh_scroll_end, LV_EVENT_SCROLL_END, (void *)cb);
}

static void slider_set_range_if_changed(lv_obj_t *slider, int32_t min_value, int32_t max_value)
{
    if (!slider) return;
    if (lv_slider_get_min_value(slider) != min_value || lv_slider_get_max_value(slider) != max_value) {
        lv_slider_set_range(slider, min_value, max_value);
    }
}

static void slider_set_value_if_changed(lv_obj_t *slider, int32_t value)
{
    if (slider && lv_slider_get_value(slider) != value) {
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
}

static void bg_color_set_if_changed(lv_obj_t *obj, lv_part_t part, lv_color_t color)
{
    if (obj && !lv_color_eq(lv_obj_get_style_bg_color(obj, part), color)) {
        lv_obj_set_style_bg_color(obj, color, part);
    }
}

static void text_color_set_if_changed(lv_obj_t *obj, lv_color_t color)
{
    if (obj && !lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
    }
}

/* ============================================================
 * LIGHTS PAGE
 * ============================================================ */

#define LIGHTS_PRESET_MAX 250
#define LIGHTS_COLOR_SLOT_PRIMARY 0
#define LIGHTS_COLOR_SLOT_SECONDARY 1
#define LIGHTS_LOCAL_ECHO_HOLD_MS 2500u
#define LIGHTS_PRESET_CMD_MAX 500

static lv_obj_t *s_power_btn;     /* vertical power switch track */
static lv_obj_t *s_power_knob;    /* sliding knob */
static lv_obj_t *s_power_icon;    /* power glyph inside the knob */
static lv_obj_t *s_bri_slider;
static lv_obj_t *s_color_slider;
static lv_obj_t *s_secondary_color_slider;
static lv_obj_t *s_kel_slider;
static lv_obj_t *s_bri_label;
static lv_obj_t *s_color_label;
static lv_obj_t *s_secondary_color_label;
static lv_obj_t *s_kel_label;
static lv_obj_t *s_pal_dropdown;
static lv_obj_t *s_pal_preview[WLED_PALETTE_PREVIEW_COUNT];
static lv_obj_t *s_transition_slider;
static lv_obj_t *s_transition_label;
static lv_obj_t *s_effect_dropdown;
static lv_obj_t *s_effect_param_rows[WLED_EFFECT_PARAM_COUNT];
static lv_obj_t *s_effect_param_labels[WLED_EFFECT_PARAM_COUNT];
static lv_obj_t *s_effect_param_sliders[WLED_EFFECT_PARAM_COUNT];
static lv_obj_t *s_effect_param_value_labels[WLED_EFFECT_PARAM_COUNT];
static lv_obj_t *s_preset_list;
static lv_obj_t *s_preset_save_bri_switch;
static lv_obj_t *s_preset_save_bounds_switch;
static lv_obj_t *s_preset_save_checked_switch;
static lv_obj_t *s_preset_playlist_count_dropdown;
static lv_obj_t *s_preset_playlist_dur_dropdown;
static lv_obj_t *s_preset_playlist_trans_dropdown;
static lv_obj_t *s_preset_playlist_repeat_dropdown;
static lv_obj_t *s_preset_playlist_end_dropdown;
static lv_obj_t *s_preset_command_dropdown;
static lv_obj_t *s_preset_command_ta;
static lv_obj_t *s_preset_command_keyboard;
static lv_obj_t *s_preset_name_ta;
static lv_obj_t *s_preset_delete_overlay;
static uint16_t s_preset_expanded_id;
static uint16_t s_preset_delete_pending_id;
static char s_preset_delete_pending_name[WLED_PRESET_NAME_MAX];
static uint32_t s_preset_render_hash;
static int16_t s_preset_edit_id = 1;
static lv_obj_t *s_lights_root;
static lv_obj_t *s_lights_home_page;
static lv_obj_t *s_lights_effects_page;
static lv_obj_t *s_lights_presets_page;
static lv_obj_t *s_lights_conn_badge;   /* connection pill in the hero header */
static lv_obj_t *s_lights_conn_label;
static lv_obj_t *s_lights_wled_detail;
static lv_obj_t *s_lights_picker_chip[3];
static lv_obj_t *s_lights_picker_box[3];
static lv_obj_t *s_lights_timer_card;
static lv_obj_t *s_lights_timer_chip_row;
static lv_obj_t *s_lights_timer_active_panel;
static lv_obj_t *s_lights_timer_arc;
static lv_obj_t *s_lights_timer_time;
static lv_obj_t *s_lights_timer_detail;
static lv_obj_t *s_lights_timer_slider;
static bool s_lights_timer_adjusting;
static uint32_t s_bri_update_ms;
static bool s_bri_dragging;
static uint32_t s_kel_update_ms;
static uint32_t s_transition_update_ms;
static uint32_t s_effect_param_update_ms[WLED_EFFECT_PARAM_COUNT];
static bool s_color_dragging[2];
static uint32_t s_color_send_ms[2];
static uint32_t s_color_hold_until_ms[2];
static uint8_t s_wled_hue_update_hz = 5;

static bool s_ui_updating;   /* guard against feedback loops */
static bool s_skip_lights_sync;
static volatile bool s_lights_led_dirty;
static volatile bool s_lights_wled_dirty;

static void lights_tuning_refresh(void)
{
    app_tuning_config_t cfg;
    if (app_config_tuning_load(&cfg) != ESP_OK) app_config_tuning_defaults(&cfg);
    s_wled_hue_update_hz = cfg.wled_hue_update_hz ? cfg.wled_hue_update_hz : 1;
}

static uint32_t hue_update_interval_ms(void)
{
    uint8_t hz = s_wled_hue_update_hz ? s_wled_hue_update_hz : 1;
    return (1000u + hz - 1u) / hz;
}

static void hold_color_slots(void)
{
    uint32_t until_ms = ui_now_ms() + LIGHTS_LOCAL_ECHO_HOLD_MS;
    s_color_hold_until_ms[LIGHTS_COLOR_SLOT_PRIMARY] = until_ms;
    s_color_hold_until_ms[LIGHTS_COLOR_SLOT_SECONDARY] = until_ms;
}

static bool color_slot_locally_owned(uint8_t slot)
{
    return slot <= LIGHTS_COLOR_SLOT_SECONDARY &&
           (s_color_dragging[slot] || hold_active_until(s_color_hold_until_ms[slot]));
}

static void brightness_label_set(uint8_t pct)
{
    /* Giant digits font has no '%' glyph; the unit lives in a separate label. */
    char b[8];
    snprintf(b, sizeof(b), "%u", pct);
    label_set_text_if_changed(s_bri_label, b);
}

static void hue_to_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t region = hue / 43;
    uint16_t rem = (hue - (region * 43)) * 6;
    uint8_t q = (uint8_t)(255 - rem);
    uint8_t t = (uint8_t)rem;

    switch (region) {
    default:
    case 0: *r = 255; *g = t;   *b = 0;   break;
    case 1: *r = q;   *g = 255; *b = 0;   break;
    case 2: *r = 0;   *g = 255; *b = t;   break;
    case 3: *r = 0;   *g = q;   *b = 255; break;
    case 4: *r = t;   *g = 0;   *b = 255; break;
    case 5: *r = 255; *g = 0;   *b = q;   break;
    }
}

static lv_color_t hue_to_color(uint8_t hue)
{
    uint8_t r, g, b;
    hue_to_rgb(hue, &r, &g, &b);
    return lv_color_make(r, g, b);
}

static void color_label_set(lv_obj_t *label, uint8_t hue)
{
    char b[16];
    snprintf(b, sizeof(b), "Hue %u", hue);
    label_set_text_if_changed(label, b);
}

static void color_tint_set(lv_obj_t *slider, uint8_t hue)
{
    lv_color_t color = hue_to_color(hue);
    bg_color_set_if_changed(slider, LV_PART_INDICATOR, color);
    bg_color_set_if_changed(slider, LV_PART_KNOB, color);
    if (slider == s_color_slider && s_lights_picker_chip[0]) {
        bg_color_set_if_changed(s_lights_picker_chip[0], LV_PART_MAIN, color);
    } else if (slider == s_secondary_color_slider && s_lights_picker_chip[1]) {
        bg_color_set_if_changed(s_lights_picker_chip[1], LV_PART_MAIN, color);
    }
}

static void kelvin_labels_set(uint16_t kelvin)
{
    char b[16];
    snprintf(b, sizeof(b), "%u K", kelvin);
    label_set_text_if_changed(s_kel_label, b);
}

static void kelvin_tint_set(uint16_t kelvin)
{
    lv_color_t tint = kelvin_to_color(kelvin);
    bg_color_set_if_changed(s_kel_slider, LV_PART_KNOB, tint);
    if (s_lights_picker_chip[2]) {
        bg_color_set_if_changed(s_lights_picker_chip[2], LV_PART_MAIN, tint);
    }
}

/* Vertical power switch geometry (track + sliding knob). */
#define PWR_SW_W          124
#define PWR_SW_H          320
#define PWR_KNOB          100
#define PWR_KNOB_MARGIN   12
#define PWR_KNOB_Y_ON     PWR_KNOB_MARGIN
#define PWR_KNOB_Y_OFF    (PWR_SW_H - PWR_KNOB - PWR_KNOB_MARGIN)

#define LIGHTS_TIMER_MODULE_W   PWR_SW_W
#define LIGHTS_TIMER_MODULE_H   PWR_SW_H
#define LIGHTS_TIMER_ARC_SIZE   116
#define LIGHTS_TIMER_SLIDER_W   72
#define LIGHTS_TIMER_SLIDER_H   178
#define LIGHTS_TIMER_TIME_FONT_NORMAL THEME_FONT_XLARGE
#define LIGHTS_TIMER_TIME_FONT_LONG   THEME_FONT_TITLE
#define LIGHTS_COLOR_PANEL_W    270
#define LIGHTS_COLOR_CHIP_SIZE  48
#define LIGHTS_COLOR_HISTORY_COUNT 5
#define LIGHTS_COLOR_PRESET_COUNT 6
#define LIGHTS_SWATCH_PRESET_BASE 0x100u

typedef struct {
    bool kelvin;
    uint16_t value;
} lights_color_swatch_t;

static const lights_color_swatch_t s_color_presets[LIGHTS_COLOR_PRESET_COUNT] = {
    {false, 0},    /* red */
    {false, 28},   /* amber */
    {false, 85},   /* green */
    {false, 128},  /* cyan */
    {false, 170},  /* blue */
    {false, 213},  /* violet */
};

static lights_color_swatch_t s_color_history[LIGHTS_COLOR_HISTORY_COUNT];
static bool s_color_history_valid[LIGHTS_COLOR_HISTORY_COUNT];
static lv_obj_t *s_color_history_btn[LIGHTS_COLOR_HISTORY_COUNT];
static lv_obj_t *s_color_preset_btn[LIGHTS_COLOR_PRESET_COUNT];
static uint8_t s_lights_picker_mode;

static void power_knob_anim_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void apply_power_visual(bool on)
{
    static bool s_visual_on;
    static bool s_visual_init;
    if (!s_power_btn) return;

    /* Track + knob colors (idempotent via set_if_changed). ON = primary glow,
     * knob rides to the top; OFF = muted track, knob rests at the bottom. */
    if (on) {
        bg_color_set_if_changed(s_power_btn, LV_PART_MAIN, theme_primary_color());
        if (s_power_knob) bg_color_set_if_changed(s_power_knob, LV_PART_MAIN, lv_color_hex(0xFFFFFF));
        if (s_power_icon) text_color_set_if_changed(s_power_icon, theme_primary_color());
    } else {
        bg_color_set_if_changed(s_power_btn, LV_PART_MAIN, THEME_SURFACE_COLOR);
        if (s_power_knob) bg_color_set_if_changed(s_power_knob, LV_PART_MAIN, THEME_CARD_COLOR);
        if (s_power_icon) text_color_set_if_changed(s_power_icon, THEME_TEXT_MUTED);
    }

    /* Slide the knob only when the state actually changes, so the 150 ms sync
     * timer can't restart the animation mid-flight. */
    bool changed = !s_visual_init || s_visual_on != on;
    s_visual_on = on;
    s_visual_init = true;
    if (s_power_knob && changed) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_power_knob);
        lv_anim_set_exec_cb(&a, power_knob_anim_y);
        lv_anim_set_values(&a, lv_obj_get_y(s_power_knob), on ? PWR_KNOB_Y_ON : PWR_KNOB_Y_OFF);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static const char *effect_param_key(size_t index)
{
    static const char *keys[WLED_EFFECT_PARAM_COUNT] = {"sx", "ix", "c1", "c2", "c3"};
    return index < WLED_EFFECT_PARAM_COUNT ? keys[index] : "sx";
}

static uint8_t effect_param_value(const wled_state_t *ws, size_t index)
{
    if (!ws) return 0;
    switch (index) {
    case 0: return ws->seg0_sx;
    case 1: return ws->seg0_ix;
    case 2: return ws->seg0_c1;
    case 3: return ws->seg0_c2;
    case 4: return ws->seg0_c3;
    default: return 0;
    }
}

static void effect_param_value_label_set(size_t index, uint8_t value)
{
    if (index >= WLED_EFFECT_PARAM_COUNT) return;
    char text[8];
    snprintf(text, sizeof(text), "%u", value);
    label_set_text_if_changed(s_effect_param_value_labels[index], text);
}

static void send_current_hue_slots(bool save)
{
    uint8_t primary_hue = s_color_slider ? (uint8_t)lv_slider_get_value(s_color_slider) : 0;
    uint8_t secondary_hue = s_secondary_color_slider ? (uint8_t)lv_slider_get_value(s_secondary_color_slider) : 0;
    hold_color_slots();
    led_state_set_hues(primary_hue, secondary_hue);
    if (save) led_state_persist_current();
}

static lv_color_t lights_swatch_color(const lights_color_swatch_t *swatch)
{
    if (!swatch) return lv_color_hex(0x202A3C);
    return swatch->kelvin ? kelvin_to_color(swatch->value) : hue_to_color((uint8_t)swatch->value);
}

static bool lights_swatch_same(const lights_color_swatch_t *lhs, const lights_color_swatch_t *rhs)
{
    return lhs && rhs && lhs->kelvin == rhs->kelvin && lhs->value == rhs->value;
}

static void lights_swatch_style(lv_obj_t *button, const lights_color_swatch_t *swatch, bool valid)
{
    if (!button) return;
    lv_obj_set_style_bg_color(button, valid ? lights_swatch_color(swatch) : lv_color_hex(0x1A2334), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, valid ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, valid ? lv_color_hex(0xF6F8FF) : lv_color_hex(0x2B354A), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, valid ? LV_OPA_70 : LV_OPA_40, LV_PART_MAIN);
}

static void lights_color_history_refresh(void)
{
    for (size_t history_index = 0; history_index < LIGHTS_COLOR_HISTORY_COUNT; history_index++) {
        lights_swatch_style(s_color_history_btn[history_index], &s_color_history[history_index],
                            s_color_history_valid[history_index]);
    }
}

static void lights_color_history_add(bool kelvin, uint16_t value)
{
    lights_color_swatch_t next = {.kelvin = kelvin, .value = value};
    if (next.kelvin) {
        if (next.value < 1000) next.value = 1000;
        if (next.value > 10000) next.value = 10000;
    } else if (next.value > 255) {
        next.value = 255;
    }

    if (s_color_history_valid[0] && lights_swatch_same(&s_color_history[0], &next)) return;

    for (size_t history_index = 0; history_index < LIGHTS_COLOR_HISTORY_COUNT; history_index++) {
        if (s_color_history_valid[history_index] && lights_swatch_same(&s_color_history[history_index], &next)) {
            for (size_t shift_index = history_index; shift_index > 0; shift_index--) {
                s_color_history[shift_index] = s_color_history[shift_index - 1];
                s_color_history_valid[shift_index] = s_color_history_valid[shift_index - 1];
            }
            s_color_history[0] = next;
            s_color_history_valid[0] = true;
            lights_color_history_refresh();
            return;
        }
    }

    for (size_t history_index = LIGHTS_COLOR_HISTORY_COUNT - 1; history_index > 0; history_index--) {
        s_color_history[history_index] = s_color_history[history_index - 1];
        s_color_history_valid[history_index] = s_color_history_valid[history_index - 1];
    }
    s_color_history[0] = next;
    s_color_history_valid[0] = true;
    lights_color_history_refresh();
}

static void lights_color_history_seed(const led_state_t *state)
{
    if (!state || s_color_history_valid[0]) return;
    lights_color_history_add(true, state->kelvin);
    lights_color_history_add(false, state->secondary_hue);
    lights_color_history_add(false, state->primary_hue);
}

static void lights_effect_selector_set(uint8_t fx)
{
    const wled_effect_catalog_item_t *item = wled_effect_catalog_find(fx);
    if (s_effect_dropdown) {
        int index = wled_effect_catalog_index_for_id(fx);
        if (index >= 0 && lv_dropdown_get_selected(s_effect_dropdown) != (uint16_t)index) {
            lv_dropdown_set_selected(s_effect_dropdown, (uint16_t)index);
        }
    }

    for (size_t i = 0; i < WLED_EFFECT_PARAM_COUNT; i++) {
        const char *label = item ? item->params[i] : "";
        if (!label || label[0] == '\0') {
            if (s_effect_param_rows[i]) lv_obj_add_flag(s_effect_param_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (s_effect_param_rows[i]) lv_obj_clear_flag(s_effect_param_rows[i], LV_OBJ_FLAG_HIDDEN);
        label_set_text_if_changed(s_effect_param_labels[i], label);
    }
}

static void lights_palette_preview_set(uint8_t pal)
{
    const wled_palette_catalog_item_t *item = wled_palette_catalog_find(pal);
    for (size_t i = 0; i < WLED_PALETTE_PREVIEW_COUNT; i++) {
        if (!s_pal_preview[i]) continue;
        uint32_t hex = item ? item->preview[i] : 0x202833;
        bg_color_set_if_changed(s_pal_preview[i], LV_PART_MAIN, lv_color_hex(hex));
    }
}

static void lights_palette_selector_set(uint8_t pal)
{
    if (s_pal_dropdown) {
        int index = wled_palette_catalog_index_for_id(pal);
        if (index >= 0 && lv_dropdown_get_selected(s_pal_dropdown) != (uint16_t)index) {
            lv_dropdown_set_selected(s_pal_dropdown, (uint16_t)index);
        }
    }
    lights_palette_preview_set(pal);
}

static void lights_apply_wled_state(const wled_state_t *ws)
{
    if (!ws || !ws->valid) return;
    s_ui_updating = true;
    lights_palette_selector_set(ws->seg0_pal);
    lights_effect_selector_set(ws->seg0_fx);
    if (s_transition_slider) {
        slider_set_value_if_changed(s_transition_slider, ws->transition);
        if (s_transition_label) {
            char tb[16];
            snprintf(tb, sizeof(tb), "%u.%u s", ws->transition / 10, ws->transition % 10);
            label_set_text_if_changed(s_transition_label, tb);
        }
    }

    for (size_t i = 0; i < WLED_EFFECT_PARAM_COUNT; i++) {
        uint8_t value = effect_param_value(ws, i);
        slider_set_value_if_changed(s_effect_param_sliders[i], value);
        effect_param_value_label_set(i, value);
    }
    preset_sync_labels_refresh();
    s_ui_updating = false;
}

static void lights_cct_gradient_update(uint16_t min_kelvin, uint16_t max_kelvin);

static void lights_apply_led_state(const led_state_t *s)
{
    if (!s) return;
    s_ui_updating = true;
    if (s_bri_slider && !s_bri_dragging) {
        slider_set_range_if_changed(s_bri_slider, 0, 100);
        slider_set_value_if_changed(s_bri_slider, s->brightness_pct);
        brightness_label_set(s->brightness_pct);
    }
    if (!color_slot_locally_owned(LIGHTS_COLOR_SLOT_PRIMARY)) {
        slider_set_value_if_changed(s_color_slider, s->primary_hue);
        color_label_set(s_color_label, s->primary_hue);
        color_tint_set(s_color_slider, s->primary_hue);
    }
    if (!color_slot_locally_owned(LIGHTS_COLOR_SLOT_SECONDARY)) {
        slider_set_value_if_changed(s_secondary_color_slider, s->secondary_hue);
        color_label_set(s_secondary_color_label, s->secondary_hue);
        color_tint_set(s_secondary_color_slider, s->secondary_hue);
    }
    if (s_kel_slider) {
        lights_cct_gradient_update(s->kelvin_min, s->kelvin_max);
        slider_set_range_if_changed(s_kel_slider, s->kelvin_min, s->kelvin_max);
        slider_set_value_if_changed(s_kel_slider, s->kelvin);
        kelvin_tint_set(s->kelvin);
        kelvin_labels_set(s->kelvin);
    }
    apply_power_visual(s->power);
    s_ui_updating = false;
}

static void lights_wled_state_cb(const wled_state_t *ws, void *user)
{
    (void)ws;
    (void)user;
    s_lights_wled_dirty = true;
}

static void lights_state_cb(const led_state_t *s, void *u)
{
    (void)s;
    (void)u;
    if (!s_skip_lights_sync) s_lights_led_dirty = true;
}

static void lights_sync_refresh(lv_timer_t *t)
{
    if (timer_page_hidden(t, s_lights_root)) return;

    if (s_lights_led_dirty) {
        s_lights_led_dirty = false;
        led_state_t state;
        led_state_get(&state);
        lights_apply_led_state(&state);
    }

    if (s_lights_wled_dirty) {
        s_lights_wled_dirty = false;
        wled_state_t ws;
        wled_state_get(&ws);
        lights_apply_wled_state(&ws);
    }
}

static lv_obj_t *lights_panel_create(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 10, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void lights_show_panel(lv_obj_t *panel)
{
    lv_obj_t *panels[] = {
        s_lights_home_page,
        s_lights_effects_page,
        s_lights_presets_page,
    };
    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (!panels[i]) continue;
        if (panels[i] == panel) lv_obj_clear_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lights_root) lv_obj_scroll_to_y(s_lights_root, 0, LV_ANIM_OFF);
}

/* WLED-style tab bar: Color / Effects / Presets segmented control. */
#define LIGHTS_TAB_COUNT 3
static lv_obj_t *s_lights_tab_btn[LIGHTS_TAB_COUNT];
static lv_obj_t *s_lights_tab_lbl[LIGHTS_TAB_COUNT];
static lv_obj_t *s_lights_tab_page[LIGHTS_TAB_COUNT];

static void lights_tab_select(int idx)
{
    if (idx < 0 || idx >= LIGHTS_TAB_COUNT || !s_lights_tab_page[idx]) return;
    lights_show_panel(s_lights_tab_page[idx]);
    for (int i = 0; i < LIGHTS_TAB_COUNT; i++) {
        if (!s_lights_tab_btn[i]) continue;
        bool on = (i == idx);
        /* Recessed segmented control: only the active tab carries a primary fill;
         * inactive tabs are transparent so the bar's glass track shows through. */
        bg_color_set_if_changed(s_lights_tab_btn[i], LV_PART_MAIN, theme_primary_color());
        lv_obj_set_style_bg_opa(s_lights_tab_btn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        if (s_lights_tab_lbl[i]) {
            lv_obj_set_style_text_color(s_lights_tab_lbl[i],
                                        on ? lv_color_black() : THEME_TEXT_SECONDARY, LV_PART_MAIN);
        }
    }
}

static void lights_tab_clicked(lv_event_t *e)
{
    lights_tab_select((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *lights_tabbar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    theme_style_glass_panel(bar, 16);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    /* Tight padding so the tab pills fill the 52px bar (≈46px tall) without
     * growing it; small gap keeps the segmented look. */
    lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    static const char *names[LIGHTS_TAB_COUNT] = {"Color", "Effects", "Presets"};
    for (int i = 0; i < LIGHTS_TAB_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_style_radius(btn, 13, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, theme_primary_color(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        /* Press feedback: dim the active fill / hint the inactive one. */
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, lights_tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_font(lbl, THEME_FONT_TITLE, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
        lv_obj_center(lbl);

        s_lights_tab_btn[i] = btn;
        s_lights_tab_lbl[i] = lbl;
    }
    return bar;
}

static void lights_status_refresh(lv_timer_t *t)
{
    if (timer_page_hidden(t, s_lights_root)) return;
    services_status_t st;
    services_status_get(&st);

    wled_state_t ws;
    wled_state_get(&ws);

    if (st.rs485_ready && st.wled_online && ws.valid) {
        if (s_lights_conn_badge) lv_obj_add_flag(s_lights_conn_badge, LV_OBJ_FLAG_HIDDEN);
        if (s_lights_wled_detail) lv_obj_add_flag(s_lights_wled_detail, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_lights_conn_badge) lv_obj_clear_flag(s_lights_conn_badge, LV_OBJ_FLAG_HIDDEN);
    if (s_lights_wled_detail) lv_obj_clear_flag(s_lights_wled_detail, LV_OBJ_FLAG_HIDDEN);

    /* Product wording on the hero pill; technical detail lives in
     * Settings > System. */
    const char *status_text;
    lv_color_t status_tint;
    char detail[64];

    if (!st.rs485_ready) {
        status_text = "Offline";
        status_tint = THEME_ERROR_COLOR;
        snprintf(detail, sizeof(detail), "Light controller unreachable");
    } else if (st.wled_online && ws.valid) {
        status_text = "Connected";
        status_tint = lv_color_hex(0x39D98A);
        if (ws.version[0]) {
            snprintf(detail, sizeof(detail), "%u LEDs  -  WLED %s", ws.led_count, ws.version);
        } else {
            snprintf(detail, sizeof(detail), "%u LEDs", ws.led_count);
        }
    } else {
        status_text = "Connecting...";
        status_tint = lv_color_hex(0xFFC53D);
        snprintf(detail, sizeof(detail), "Looking for the light controller");
    }

    label_set_text_if_changed(s_lights_conn_label, status_text);
    text_color_set_if_changed(s_lights_conn_label, status_tint);
    bg_color_set_if_changed(s_lights_conn_badge, LV_PART_MAIN, status_tint);
    label_set_text_if_changed(s_lights_wled_detail, detail);
}

static lv_color_t lights_timer_urgency_color(uint32_t remaining_progress)
{
    uint32_t urgency = remaining_progress >= 1000u ? 0u : 1000u - remaining_progress;
    if (urgency >= 900u) return lv_color_hex(0xFF4D4D);
    if (urgency >= 750u) return lv_color_hex(0xFF8A1F);
    if (urgency >= 500u) return lv_color_hex(0xFFB347);
    return THEME_PRIMARY_COLOR;
}

static void lights_timer_format_time(uint32_t remaining_ms, char *out, size_t out_len)
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

static const lv_font_t *lights_timer_time_font(uint32_t remaining_ms)
{
    uint32_t remaining_s = (remaining_ms + 999u) / 1000u;
    return remaining_s >= 3600u ? LIGHTS_TIMER_TIME_FONT_LONG : LIGHTS_TIMER_TIME_FONT_NORMAL;
}

static void lights_timer_time_set(uint32_t remaining_ms)
{
    if (!s_lights_timer_time) return;
    char time_text[12];
    lights_timer_format_time(remaining_ms, time_text, sizeof(time_text));
    lv_obj_set_style_text_font(s_lights_timer_time, lights_timer_time_font(remaining_ms), LV_PART_MAIN);
    label_set_text_if_changed(s_lights_timer_time, time_text);
    lv_obj_center(s_lights_timer_time);
}

static void lights_timer_format_minutes(uint16_t minutes, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (minutes >= 60u) snprintf(out, out_len, "%uh %02um", (unsigned)(minutes / 60u), (unsigned)(minutes % 60u));
    else snprintf(out, out_len, "%u min", (unsigned)minutes);
}

static void lights_timer_chip_clicked(lv_event_t *e)
{
    uint16_t minutes = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    if (!minutes) return;
    idle_manager_light_timer_start_minutes(minutes);
    toast_show("Light timer started");
}

static uint16_t lights_timer_slider_minutes(lv_obj_t *slider)
{
    int32_t value = slider ? lv_slider_get_value(slider) : 1;
    if (value < 1) value = 1;
    if (value > 240) value = 240;
    return (uint16_t)value;
}

static void lights_timer_preview_minutes(uint16_t minutes)
{
    uint32_t preview_ms = (uint32_t)minutes * 60u * 1000u;
    lights_timer_time_set(preview_ms);

    char minute_text[20];
    lights_timer_format_minutes(minutes, minute_text, sizeof(minute_text));
    char detail[40];
    snprintf(detail, sizeof(detail), "Remaining %s", minute_text);
    label_set_text_if_changed(s_lights_timer_detail, detail);
}

static void lights_timer_commit_slider(lv_obj_t *slider)
{
    if (!slider || !s_lights_timer_adjusting) return;
    s_lights_timer_adjusting = false;
    idle_manager_light_timer_adjust_minutes(lights_timer_slider_minutes(slider));

    idle_light_timer_state_t timer;
    idle_manager_light_timer_get(&timer);
    if (timer.active) {
        lv_slider_set_range(slider, 1, timer.max_minutes ? timer.max_minutes : 240);
        lv_slider_set_value(slider, timer.remaining_minutes, LV_ANIM_OFF);
    }
}

static void lights_timer_slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target(e);
    uint16_t minutes = lights_timer_slider_minutes(slider);
    if (code == LV_EVENT_PRESSED) {
        s_lights_timer_adjusting = true;
        lights_timer_preview_minutes(minutes);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (!s_lights_timer_adjusting) return;
        lights_timer_preview_minutes(minutes);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_CANCEL || code == LV_EVENT_DEFOCUSED) {
        lights_timer_commit_slider(slider);
    }
}

static void lights_timer_panel_refresh(lv_timer_t *t)
{
    if (timer_page_hidden(t, s_lights_root)) return;
    if (!s_lights_timer_card) return;

    idle_light_timer_state_t timer;
    idle_manager_light_timer_get(&timer);
    if (!timer.active) {
        s_lights_timer_adjusting = false;
        if (s_lights_timer_chip_row) lv_obj_clear_flag(s_lights_timer_chip_row, LV_OBJ_FLAG_HIDDEN);
        if (s_lights_timer_active_panel) lv_obj_add_flag(s_lights_timer_active_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (s_lights_timer_chip_row) lv_obj_add_flag(s_lights_timer_chip_row, LV_OBJ_FLAG_HIDDEN);
    if (s_lights_timer_active_panel) lv_obj_clear_flag(s_lights_timer_active_panel, LV_OBJ_FLAG_HIDDEN);

    if (s_lights_timer_adjusting && s_lights_timer_slider &&
        !lv_obj_has_state(s_lights_timer_slider, LV_STATE_PRESSED)) {
        lights_timer_commit_slider(s_lights_timer_slider);
        idle_manager_light_timer_get(&timer);
    }

    lights_timer_time_set(timer.remaining_ms);

    char minute_text[20];
    lights_timer_format_minutes(timer.remaining_minutes, minute_text, sizeof(minute_text));
    char detail[40];
    snprintf(detail, sizeof(detail), "Remaining %s", minute_text);
    if (!s_lights_timer_adjusting) label_set_text_if_changed(s_lights_timer_detail, detail);

    uint32_t progress = 1000u;
    if (timer.duration_ms > 0) {
        progress = timer.remaining_ms >= timer.duration_ms
            ? 1000u
            : (uint32_t)(((uint64_t)timer.remaining_ms * 1000ULL + (uint64_t)timer.duration_ms / 2ULL) /
                         (uint64_t)timer.duration_ms);
        if (progress > 1000u) progress = 1000u;
    }
    if (s_lights_timer_arc) {
        lv_arc_set_value(s_lights_timer_arc, (int32_t)progress);
        lv_color_t color = lights_timer_urgency_color(progress);
        lv_obj_set_style_arc_color(s_lights_timer_arc, color, LV_PART_INDICATOR);
    }
    if (s_lights_timer_slider) {
        lv_slider_set_range(s_lights_timer_slider, 1, timer.max_minutes ? timer.max_minutes : 240);
        if (!s_lights_timer_adjusting) {
            lv_slider_set_value(s_lights_timer_slider, timer.remaining_minutes, LV_ANIM_OFF);
        }
    }
}

static const int s_playlist_count_values[] = {2, 3, 4};
static const int s_playlist_dur_values[] = {50, 100, 300, 600, 3000};
static const int s_playlist_trans_values[] = {0, 7, 15, 30};
static const int s_playlist_repeat_values[] = {0, 1, 2, 5};

static int preset_dropdown_value(lv_obj_t *dropdown, const int *values, size_t count, int fallback)
{
    if (!dropdown || !values || count == 0) return fallback;
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    if (selected >= count) return fallback;
    return values[selected];
}

static bool preset_switch_checked(lv_obj_t *sw, bool fallback)
{
    if (!sw) return fallback;
    return lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static void preset_json_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    size_t out = 0;
    for (size_t i = 0; src && src[i] && out + 1 < dst_len; i++) {
        char ch = src[i];
        if ((ch == '"' || ch == '\\') && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = ch;
        } else if ((unsigned char)ch >= 0x20) {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

static const wled_preset_item_t *preset_item_for_id(const wled_state_t *ws, uint16_t id)
{
    if (!ws || !ws->presets || id == 0) return NULL;
    for (uint16_t i = 0; i < ws->preset_count; i++) {
        if (ws->presets[i].id == id) return &ws->presets[i];
    }
    return NULL;
}

static uint16_t preset_next_free_id(const wled_state_t *ws)
{
    for (uint16_t id = 1; id <= LIGHTS_PRESET_MAX; id++) {
        if (!preset_item_for_id(ws, id)) return id;
    }
    return 0;
}

static void preset_default_name(uint16_t id, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "Preset %u", (unsigned)id);
}

static const char *preset_current_name(uint16_t id, char *fallback, size_t fallback_len)
{
    if (s_preset_name_ta) {
        const char *text = lv_textarea_get_text(s_preset_name_ta);
        if (text && text[0]) return text;
    }

    wled_state_t ws;
    wled_state_get(&ws);
    const wled_preset_item_t *item = preset_item_for_id(&ws, id);
    if (item && item->name[0]) {
        snprintf(fallback, fallback_len, "%s", item->name);
        return fallback;
    }

    preset_default_name(id, fallback, fallback_len);
    return fallback;
}

static int preset_wrapped_id(int base, int offset)
{
    int id = base + offset;
    while (id > LIGHTS_PRESET_MAX) id -= LIGHTS_PRESET_MAX;
    while (id < 1) id += LIGHTS_PRESET_MAX;
    return id;
}

static int preset_playlist_build_json(char *json, size_t json_len)
{
    if (!json || json_len == 0) return -1;
    int count = preset_dropdown_value(s_preset_playlist_count_dropdown,
                                      s_playlist_count_values,
                                      sizeof(s_playlist_count_values) / sizeof(s_playlist_count_values[0]), 2);
    int dur = preset_dropdown_value(s_preset_playlist_dur_dropdown,
                                    s_playlist_dur_values,
                                    sizeof(s_playlist_dur_values) / sizeof(s_playlist_dur_values[0]), 100);
    int transition = preset_dropdown_value(s_preset_playlist_trans_dropdown,
                                           s_playlist_trans_values,
                                           sizeof(s_playlist_trans_values) / sizeof(s_playlist_trans_values[0]), 7);
    int repeat = preset_dropdown_value(s_preset_playlist_repeat_dropdown,
                                       s_playlist_repeat_values,
                                       sizeof(s_playlist_repeat_values) / sizeof(s_playlist_repeat_values[0]), 0);
    int end = 0;
    if (s_preset_playlist_end_dropdown && lv_dropdown_get_selected(s_preset_playlist_end_dropdown) == 1) {
        end = (int)s_preset_edit_id;
    }

    char ps[48] = {0};
    char dur_arr[48] = {0};
    char trans_arr[48] = {0};
    for (int i = 0; i < count; i++) {
        size_t ps_len = strlen(ps);
        size_t dur_len = strlen(dur_arr);
        size_t trans_len = strlen(trans_arr);
        snprintf(ps + ps_len, sizeof(ps) - ps_len, "%s%d", i ? "," : "", preset_wrapped_id((int)s_preset_edit_id, i));
        snprintf(dur_arr + dur_len, sizeof(dur_arr) - dur_len, "%s%d", i ? "," : "", dur);
        snprintf(trans_arr + trans_len, sizeof(trans_arr) - trans_len, "%s%d", i ? "," : "", transition);
    }

    return snprintf(json, json_len,
                    "{\"playlist\":{\"ps\":[%s],\"dur\":[%s],\"transition\":[%s],\"repeat\":%d,\"end\":%d}}",
                    ps, dur_arr, trans_arr, repeat, end);
}

static void preset_command_set_text(const char *json)
{
    if (!s_preset_command_ta || !json) return;
    lv_textarea_set_text(s_preset_command_ta, json);
}

static bool preset_command_clean_json(char *out, size_t out_len)
{
    if (!s_preset_command_ta || !out || out_len == 0) return false;
    const char *text = lv_textarea_get_text(s_preset_command_ta);
    if (!text) return false;

    while (*text == ' ' || *text == '\t') text++;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;

    if (len < 2 || len >= out_len || len >= 512) return false;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' || text[i] == '\r') return false;
    }
    if (text[0] != '{' || text[len - 1] != '}') return false;

    memcpy(out, text, len);
    out[len] = '\0';
    return true;
}

static void preset_sync_labels_refresh(void)
{
    preset_panels_rebuild(false);
}

static void preset_send(const char *json, const char *toast)
{
    if (!json || !json[0]) return;
    esp_err_t err = cmd_tx_send_json(json);
    if (err == ESP_OK) {
        if (toast && toast[0]) toast_show(toast);
    } else {
        toast_show(esp_err_to_name(err));
    }
}

static void preset_apply_pick(lv_event_t *e)
{
    (void)e;
    char json[32];
    snprintf(json, sizeof(json), "{\"ps\":%d}", (int)s_preset_edit_id);
    preset_send(json, "Preset applied");
}

static void preset_save_pick(lv_event_t *e)
{
    (void)e;
    char fallback[WLED_PRESET_NAME_MAX];
    char escaped[WLED_PRESET_NAME_MAX * 2];
    preset_json_escape(preset_current_name((uint16_t)s_preset_edit_id, fallback, sizeof(fallback)), escaped, sizeof(escaped));

    char json[160];
    snprintf(json, sizeof(json), "{\"psave\":%d,\"n\":\"%s\",\"ib\":%s,\"sb\":%s,\"sc\":%s}",
             (int)s_preset_edit_id,
             escaped,
             preset_switch_checked(s_preset_save_bri_switch, true) ? "true" : "false",
             preset_switch_checked(s_preset_save_bounds_switch, true) ? "true" : "false",
             preset_switch_checked(s_preset_save_checked_switch, true) ? "true" : "false");
    preset_send(json, "Preset saved");
}

static void preset_delete_pick(lv_event_t *e)
{
    (void)e;
    char json[32];
    snprintf(json, sizeof(json), "{\"pdel\":%d}", (int)s_preset_edit_id);
    preset_send(json, "Preset deleted");
    if (s_preset_expanded_id == (uint16_t)s_preset_edit_id) s_preset_expanded_id = 0;
    preset_panels_rebuild(true);
}

static void preset_create_clicked(lv_event_t *e)
{
    (void)e;
    wled_state_t ws;
    wled_state_get(&ws);
    uint16_t id = preset_next_free_id(&ws);
    if (!id) {
        toast_show("No free preset slots");
        return;
    }

    char name[WLED_PRESET_NAME_MAX];
    char escaped[WLED_PRESET_NAME_MAX * 2];
    preset_default_name(id, name, sizeof(name));
    preset_json_escape(name, escaped, sizeof(escaped));

    s_preset_edit_id = (int16_t)id;
    s_preset_expanded_id = id;
    char json[160];
    snprintf(json, sizeof(json), "{\"psave\":%u,\"n\":\"%s\",\"ib\":true,\"sb\":true,\"sc\":true}",
             (unsigned)id, escaped);
    preset_send(json, "Preset created");
}

static void preset_delete_confirm_close(void)
{
    if (s_preset_delete_overlay) {
        lv_obj_delete(s_preset_delete_overlay);
        s_preset_delete_overlay = NULL;
    }
    s_preset_delete_pending_id = 0;
    s_preset_delete_pending_name[0] = '\0';
}

static void preset_delete_cancel_clicked(lv_event_t *e)
{
    (void)e;
    preset_delete_confirm_close();
}

static void preset_delete_confirm_clicked(lv_event_t *e)
{
    (void)e;
    uint16_t id = s_preset_delete_pending_id;
    preset_delete_confirm_close();
    if (!id) return;
    s_preset_edit_id = (int16_t)id;
    preset_delete_pick(NULL);
}

static void preset_delete_overlay_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_preset_delete_overlay) preset_delete_confirm_close();
}

static void preset_next_cmd(lv_event_t *e)
{
    (void)e;
    preset_send("{\"np\":true}", "Next preset");
}

static void preset_playlist_cmd(lv_event_t *e)
{
    (void)e;
    char json[LIGHTS_PRESET_CMD_MAX];
    int written = preset_playlist_build_json(json, sizeof(json));
    if (written <= 0 || (size_t)written >= sizeof(json)) {
        toast_show("Playlist command too long");
        return;
    }
    preset_send(json, "Playlist started");
}

static void preset_sync_now_cmd(lv_event_t *e)
{
    (void)e;
    preset_send("{\"v\":true}", "Requested S3 preset state");
    preset_sync_labels_refresh();
}

static void preset_command_template_cmd(lv_event_t *e)
{
    (void)e;
    if (!s_preset_command_dropdown) return;
    char json[LIGHTS_PRESET_CMD_MAX];
    switch (lv_dropdown_get_selected(s_preset_command_dropdown)) {
        case 0:
            snprintf(json, sizeof(json), "{\"ps\":%d}", (int)s_preset_edit_id);
            break;
        case 1:
        {
            char fallback[WLED_PRESET_NAME_MAX];
            char escaped[WLED_PRESET_NAME_MAX * 2];
            preset_json_escape(preset_current_name((uint16_t)s_preset_edit_id, fallback, sizeof(fallback)), escaped, sizeof(escaped));
            snprintf(json, sizeof(json), "{\"psave\":%d,\"n\":\"%s\",\"ib\":%s,\"sb\":%s,\"sc\":%s}",
                     (int)s_preset_edit_id,
                     escaped,
                     preset_switch_checked(s_preset_save_bri_switch, true) ? "true" : "false",
                     preset_switch_checked(s_preset_save_bounds_switch, true) ? "true" : "false",
                     preset_switch_checked(s_preset_save_checked_switch, true) ? "true" : "false");
            break;
        }
        case 2:
            snprintf(json, sizeof(json), "{\"pdel\":%d}", (int)s_preset_edit_id);
            break;
        case 3:
            snprintf(json, sizeof(json), "{\"np\":true}");
            break;
        case 4:
            if (preset_playlist_build_json(json, sizeof(json)) <= 0) snprintf(json, sizeof(json), "{}");
            break;
        case 5:
            snprintf(json, sizeof(json), "{\"v\":true}");
            break;
        case 6:
            snprintf(json, sizeof(json), "{\"on\":true}");
            break;
        default:
            snprintf(json, sizeof(json), "{\"on\":false}");
            break;
    }
    preset_command_set_text(json);
}

static void preset_command_keyboard_hide(void)
{
    if (!s_preset_command_keyboard) return;
    lv_keyboard_set_textarea(s_preset_command_keyboard, NULL);
    lv_obj_add_flag(s_preset_command_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void preset_command_keyboard_show(lv_obj_t *ta)
{
    if (!s_preset_command_keyboard || !ta) return;
    lv_keyboard_set_textarea(s_preset_command_keyboard, ta);
    lv_keyboard_set_mode(s_preset_command_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(s_preset_command_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_preset_command_keyboard);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void preset_command_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED) {
        preset_command_keyboard_show(ta);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        preset_command_keyboard_hide();
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    }
}

static void preset_command_send_cmd(lv_event_t *e)
{
    (void)e;
    char json[LIGHTS_PRESET_CMD_MAX];
    if (!preset_command_clean_json(json, sizeof(json))) {
        toast_show("Invalid JSON command");
        return;
    }
    preset_command_keyboard_hide();
    preset_send(json, "Command sent");
}

static lv_obj_t *preset_action_button_create(lv_obj_t *parent, const char *label,
                                             lv_color_t bg, lv_coord_t height,
                                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, height);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, height / 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *preset_switch_row_create(lv_obj_t *parent, const char *label, bool checked)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_color(lb, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, theme_primary_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    return sw;
}

static lv_obj_t *preset_dropdown_row_create(lv_obj_t *parent, const char *label, const char *options)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_width(lb, 168);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lb, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_obj_set_size(dd, 210, 42);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(dd, LV_DIR_BOTTOM);
    lv_obj_set_style_bg_color(dd, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dd, 12, LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, LV_PART_MAIN);
    return dd;
}

static void preset_panel_toggle_clicked(lv_event_t *e)
{
    uint16_t id = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    if (!id) return;
    s_preset_edit_id = (int16_t)id;
    s_preset_expanded_id = (s_preset_expanded_id == id) ? 0 : id;
    preset_panels_rebuild(true);
}

static void preset_delete_button_clicked(lv_event_t *e)
{
    uint16_t id = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    if (!id) return;

    wled_state_t ws;
    wled_state_get(&ws);
    const wled_preset_item_t *item = preset_item_for_id(&ws, id);
    s_preset_delete_pending_id = id;
    if (item && item->name[0]) snprintf(s_preset_delete_pending_name, sizeof(s_preset_delete_pending_name), "%s", item->name);
    else preset_default_name(id, s_preset_delete_pending_name, sizeof(s_preset_delete_pending_name));

    if (s_preset_delete_overlay) lv_obj_delete(s_preset_delete_overlay);
    s_preset_delete_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_preset_delete_overlay);
    lv_obj_set_size(s_preset_delete_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_preset_delete_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preset_delete_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_preset_delete_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_preset_delete_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_preset_delete_overlay, preset_delete_overlay_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_preset_delete_overlay);

    lv_obj_t *card = lv_obj_create(s_preset_delete_overlay);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 20);
    lv_obj_set_width(card, 560);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 14, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Delete Preset");
    lv_obj_set_style_text_color(title, THEME_ERROR_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    char msg[160];
    snprintf(msg, sizeof(msg), "Delete \"%s\"?\nThis removes preset %u from WLED.",
             s_preset_delete_pending_name, (unsigned)id);
    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, msg);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(body, THEME_FONT_BODY, LV_PART_MAIN);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, LV_PART_MAIN);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cancel_btn = lv_button_create(btns);
    lv_obj_set_size(cancel_btn, 126, 44);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, preset_delete_cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    lv_obj_t *delete_btn = lv_button_create(btns);
    lv_obj_set_size(delete_btn, 126, 44);
    lv_obj_set_style_radius(delete_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(delete_btn, THEME_ERROR_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(delete_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(delete_btn, preset_delete_confirm_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *delete_lbl = lv_label_create(delete_btn);
    lv_label_set_text(delete_lbl, "Delete");
    lv_obj_set_style_text_color(delete_lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(delete_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(delete_lbl);
}

static lv_obj_t *preset_plus_button_create(lv_obj_t *parent, bool large)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, large ? 108 : 48, large ? 108 : 48);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, theme_primary_color(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, preset_create_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, large ? THEME_FONT_TITLE : THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

static uint32_t preset_list_hash(const wled_state_t *ws)
{
    uint32_t hash = 2166136261u;
    if (!ws) return hash;
    hash = (hash ^ (uint32_t)ws->ps) * 16777619u;
    hash = (hash ^ (uint32_t)ws->pl) * 16777619u;
    hash = (hash ^ ws->preset_count) * 16777619u;
    hash = (hash ^ (uint32_t)ws->presets_truncated) * 16777619u;
    hash = (hash ^ s_preset_expanded_id) * 16777619u;
    for (uint16_t i = 0; i < ws->preset_count; i++) {
        hash = (hash ^ ws->presets[i].id) * 16777619u;
        for (const char *p = ws->presets[i].name; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
    }
    return hash;
}

static void preset_panel_section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_SMALL, LV_PART_MAIN);
}

static void preset_expanded_body_create(lv_obj_t *card, const wled_preset_item_t *item)
{
    if (!item) return;
    s_preset_edit_id = (int16_t)item->id;

    preset_panel_section_label(card, "NAME");
    s_preset_name_ta = lv_textarea_create(card);
    lv_obj_set_size(s_preset_name_ta, LV_PCT(100), 48);
    lv_textarea_set_one_line(s_preset_name_ta, true);
    lv_textarea_set_max_length(s_preset_name_ta, WLED_PRESET_NAME_MAX - 1);
    lv_textarea_set_text(s_preset_name_ta, item->name[0] ? item->name : "Preset");
    lv_obj_set_style_bg_color(s_preset_name_ta, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preset_name_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_preset_name_ta, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_preset_name_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_preset_name_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_preset_name_ta, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_preset_name_ta, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_preset_name_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_preset_name_ta, 10, LV_PART_MAIN);
    lv_obj_add_flag(s_preset_name_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(s_preset_name_ta, preset_command_ta_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_preset_name_ta, preset_command_ta_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_preset_name_ta, preset_command_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_preset_name_ta, preset_command_ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_preset_name_ta, preset_command_ta_event, LV_EVENT_CANCEL, NULL);

    lv_obj_t *actions = lv_obj_create(card);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 10, LV_PART_MAIN);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    preset_action_button_create(actions, "Apply", theme_primary_color(), 48, preset_apply_pick);
    preset_action_button_create(actions, "Save", lv_color_hex(0x1E8A6B), 48, preset_save_pick);
    preset_action_button_create(actions, "Next", lv_color_hex(0x2A3E58), 48, preset_next_cmd);

    preset_panel_section_label(card, "SAVE OPTIONS");
    lv_obj_t *save_opts = lv_obj_create(card);
    lv_obj_remove_style_all(save_opts);
    lv_obj_set_size(save_opts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(save_opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(save_opts, 8, LV_PART_MAIN);
    lv_obj_clear_flag(save_opts, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_preset_save_bri_switch = preset_switch_row_create(save_opts, "Brightness", true);
    s_preset_save_bounds_switch = preset_switch_row_create(save_opts, "Segment bounds", true);
    s_preset_save_checked_switch = preset_switch_row_create(save_opts, "Selected segment", true);

    preset_panel_section_label(card, "PLAYLIST");
    lv_obj_t *playlist_opts = lv_obj_create(card);
    lv_obj_remove_style_all(playlist_opts);
    lv_obj_set_size(playlist_opts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(playlist_opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(playlist_opts, 8, LV_PART_MAIN);
    lv_obj_clear_flag(playlist_opts, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_preset_playlist_count_dropdown = preset_dropdown_row_create(playlist_opts, "Steps", "2 presets\n3 presets\n4 presets");
    s_preset_playlist_dur_dropdown = preset_dropdown_row_create(playlist_opts, "Duration", "5 s\n10 s\n30 s\n1 min\n5 min");
    lv_dropdown_set_selected(s_preset_playlist_dur_dropdown, 1);
    s_preset_playlist_trans_dropdown = preset_dropdown_row_create(playlist_opts, "Transition", "0 s\n0.7 s\n1.5 s\n3 s");
    lv_dropdown_set_selected(s_preset_playlist_trans_dropdown, 1);
    s_preset_playlist_repeat_dropdown = preset_dropdown_row_create(playlist_opts, "Repeat", "Forever\nOnce\n2 times\n5 times");
    s_preset_playlist_end_dropdown = preset_dropdown_row_create(playlist_opts, "When done", "Do nothing\nApply this");

    lv_obj_t *playlist_actions = lv_obj_create(card);
    lv_obj_remove_style_all(playlist_actions);
    lv_obj_set_size(playlist_actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(playlist_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(playlist_actions, 10, LV_PART_MAIN);
    lv_obj_clear_flag(playlist_actions, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    preset_action_button_create(playlist_actions, "Start playlist", lv_color_hex(0x2A5E72), 44, preset_playlist_cmd);

    preset_panel_section_label(card, "COMMAND MENU");
    s_preset_command_dropdown = preset_dropdown_row_create(card, "Template",
                                                          "Apply this\nSave this\nDelete this\nNext preset\nPlaylist from options\nSync state\nPower on\nPower off");

    lv_obj_t *command_btns = lv_obj_create(card);
    lv_obj_remove_style_all(command_btns);
    lv_obj_set_size(command_btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(command_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(command_btns, 10, LV_PART_MAIN);
    lv_obj_clear_flag(command_btns, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    preset_action_button_create(command_btns, "Load", lv_color_hex(0x2A3E58), 42, preset_command_template_cmd);
    preset_action_button_create(command_btns, "Send", theme_primary_color(), 42, preset_command_send_cmd);

    s_preset_command_ta = lv_textarea_create(card);
    lv_obj_set_size(s_preset_command_ta, LV_PCT(100), 92);
    lv_textarea_set_one_line(s_preset_command_ta, false);
    lv_textarea_set_max_length(s_preset_command_ta, LIGHTS_PRESET_CMD_MAX - 1);
    lv_textarea_set_placeholder_text(s_preset_command_ta, "{\"ps\":1}");
    char initial[32];
    snprintf(initial, sizeof(initial), "{\"ps\":%u}", (unsigned)item->id);
    lv_textarea_set_text(s_preset_command_ta, initial);
    lv_obj_set_style_bg_color(s_preset_command_ta, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preset_command_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_preset_command_ta, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_preset_command_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_preset_command_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_preset_command_ta, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_preset_command_ta, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_preset_command_ta, 12, LV_PART_MAIN);
    lv_obj_add_flag(s_preset_command_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(s_preset_command_ta, preset_command_ta_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_preset_command_ta, preset_command_ta_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_preset_command_ta, preset_command_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_preset_command_ta, preset_command_ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_preset_command_ta, preset_command_ta_event, LV_EVENT_CANCEL, NULL);
}

static void preset_panel_create(lv_obj_t *parent, const wled_preset_item_t *item, const wled_state_t *ws)
{
    if (!parent || !item) return;
    bool expanded = s_preset_expanded_id == item->id;
    bool active = ws && ws->ps == (int16_t)item->id;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, expanded ? 16 : 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, expanded ? 12 : 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, expanded ? THEME_CARD_COLOR : THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, active ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, active ? theme_primary_color() : THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_button_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 56);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_add_event_cb(header, preset_panel_toggle_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);

    lv_obj_t *left = lv_obj_create(header);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, LV_PART_MAIN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *name = lv_label_create(left);
    lv_label_set_text(name, item->name[0] ? item->name : "Preset");
    lv_obj_set_width(name, 390);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(name, THEME_FONT_LABEL, LV_PART_MAIN);

    char meta[48];
    if (active) snprintf(meta, sizeof(meta), "Preset %u  -  Active", (unsigned)item->id);
    else snprintf(meta, sizeof(meta), "Preset %u", (unsigned)item->id);
    lv_obj_t *sub = lv_label_create(left);
    lv_label_set_text(sub, meta);
    lv_obj_set_style_text_color(sub, active ? theme_primary_color() : THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, THEME_FONT_SMALL, LV_PART_MAIN);

    if (expanded) {
        lv_obj_t *delete_btn = lv_button_create(header);
        lv_obj_set_size(delete_btn, 42, 42);
        lv_obj_set_style_radius(delete_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0x7D2B3D), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(delete_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(delete_btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(delete_btn, preset_delete_button_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)item->id);
        lv_obj_t *delete_lbl = lv_label_create(delete_btn);
        lv_label_set_text(delete_lbl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(delete_lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
        lv_obj_center(delete_lbl);
    }

    lv_obj_t *arrow = lv_label_create(header);
    lv_label_set_text(arrow, expanded ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(arrow, THEME_FONT_BODY, LV_PART_MAIN);

    if (expanded) preset_expanded_body_create(card, item);
}

static void preset_panels_rebuild(bool force)
{
    if (!s_preset_list) return;

    wled_state_t ws;
    wled_state_get(&ws);
    if (s_preset_expanded_id && !preset_item_for_id(&ws, s_preset_expanded_id)) s_preset_expanded_id = 0;

    uint32_t hash = preset_list_hash(&ws);
    if (!force && hash == s_preset_render_hash) return;
    s_preset_render_hash = hash;

    s_preset_save_bri_switch = NULL;
    s_preset_save_bounds_switch = NULL;
    s_preset_save_checked_switch = NULL;
    s_preset_playlist_count_dropdown = NULL;
    s_preset_playlist_dur_dropdown = NULL;
    s_preset_playlist_trans_dropdown = NULL;
    s_preset_playlist_repeat_dropdown = NULL;
    s_preset_playlist_end_dropdown = NULL;
    s_preset_command_dropdown = NULL;
    s_preset_command_ta = NULL;
    s_preset_name_ta = NULL;

    preset_command_keyboard_hide();
    lv_obj_clean(s_preset_list);
    if (ws.preset_count == 0) {
        lv_obj_set_flex_align(s_preset_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        preset_plus_button_create(s_preset_list, true);
        return;
    }

    lv_obj_set_flex_align(s_preset_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *top = lv_obj_create(s_preset_list);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(top);
    char title_text[48];
    snprintf(title_text, sizeof(title_text), "%u%s presets", (unsigned)ws.preset_count,
             ws.presets_truncated ? "+" : "");
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_color(title, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_SMALL, LV_PART_MAIN);
    preset_plus_button_create(top, false);

    for (uint16_t i = 0; i < ws.preset_count; i++) {
        preset_panel_create(s_preset_list, &ws.presets[i], &ws);
    }
}

static void fx_prev_clicked(lv_event_t *e)
{
    (void)e;
    wled_state_t ws;
    wled_state_get(&ws);
    uint8_t fx = wled_effect_catalog_adjacent_id(ws.seg0_fx, -1);
    lights_effect_selector_set(fx);
    char json[48];
    snprintf(json, sizeof(json), "{\"seg\":[{\"fx\":%u}]}", fx);
    cmd_tx_send_json(json);
}

static void fx_next_clicked(lv_event_t *e)
{
    (void)e;
    wled_state_t ws;
    wled_state_get(&ws);
    uint8_t fx = wled_effect_catalog_adjacent_id(ws.seg0_fx, 1);
    lights_effect_selector_set(fx);
    char json[48];
    snprintf(json, sizeof(json), "{\"seg\":[{\"fx\":%u}]}", fx);
    cmd_tx_send_json(json);
}

static void fx_dropdown_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    uint16_t index = lv_dropdown_get_selected(lv_event_get_target(e));
    uint8_t fx = wled_effect_catalog_id_for_index(index);
    lights_effect_selector_set(fx);
    char json[48];
    snprintf(json, sizeof(json), "{\"seg\":[{\"fx\":%u}]}", fx);
    cmd_tx_send_json(json);
}

static void pal_dropdown_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    uint16_t index = lv_dropdown_get_selected(lv_event_get_target(e));
    uint8_t pal = wled_palette_catalog_id_for_index(index);
    lights_palette_preview_set(pal);
    char json[48];
    snprintf(json, sizeof(json), "{\"seg\":[{\"pal\":%u}]}", pal);
    cmd_tx_send_json(json);
}

static void effect_param_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    size_t index = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (index >= WLED_EFFECT_PARAM_COUNT) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED && !rate_limit_ms(&s_effect_param_update_ms[index], 120)) return;
    int v = lv_slider_get_value(lv_event_get_target(e));
    effect_param_value_label_set(index, (uint8_t)v);
    char json[48];
    snprintf(json, sizeof(json), "{\"seg\":[{\"%s\":%d}]}", effect_param_key(index), v);
    cmd_tx_send_json(json);
}

static void power_clicked(lv_event_t *e)
{
    (void)e;
    led_state_t s; led_state_get(&s);
    led_state_set_power(!s.power);
    toast_show(s.power ? "Lights OFF" : "Lights ON");
}

static void bri_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_bri_dragging = true;
        s_bri_update_ms = 0;
        return;
    }

    int v = lv_slider_get_value(lv_event_get_target(e));
    brightness_label_set((uint8_t)v);

    if (code == LV_EVENT_VALUE_CHANGED && !rate_limit_ms(&s_bri_update_ms, 90)) return;

    s_skip_lights_sync = true;
    led_state_set_brightness((uint8_t)v);
    s_skip_lights_sync = false;

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_bri_dragging = false;
        led_state_persist_current();
        char buf[24]; snprintf(buf, sizeof(buf), "Brightness  %d%%", v);
        toast_show(buf);
    }
}

static void color_slot_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    uint8_t slot = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (slot > LIGHTS_COLOR_SLOT_SECONDARY) return;
    lv_event_code_t code = lv_event_get_code(e);
    int v = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_PRESSED) {
        s_color_dragging[slot] = true;
        s_color_hold_until_ms[slot] = 0;
        s_color_send_ms[slot] = 0;
        return;
    }

    color_label_set(slot == LIGHTS_COLOR_SLOT_PRIMARY ? s_color_label : s_secondary_color_label, (uint8_t)v);
    color_tint_set(lv_event_get_target(e), (uint8_t)v);

    if (code == LV_EVENT_VALUE_CHANGED) {
        if (rate_limit_ms(&s_color_send_ms[slot], hue_update_interval_ms())) send_current_hue_slots(false);
        return;
    }

    s_color_dragging[slot] = false;
    send_current_hue_slots(true);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lights_color_history_add(false, (uint16_t)v);
        char buf[32]; snprintf(buf, sizeof(buf), "%s hue %d", slot == LIGHTS_COLOR_SLOT_PRIMARY ? "Primary" : "Secondary", v);
        toast_show(buf);
    }
}

static void kel_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED && !rate_limit_ms(&s_kel_update_ms, 90)) return;
    int v = lv_slider_get_value(lv_event_get_target(e));
    s_skip_lights_sync = true;
    led_state_set_kelvin((uint16_t)v);
    s_skip_lights_sync = false;
    kelvin_labels_set((uint16_t)v);
    kelvin_tint_set((uint16_t)v);
    if (code != LV_EVENT_VALUE_CHANGED) {
        led_state_persist_current();
        lights_color_history_add(true, (uint16_t)v);
        char buf[24]; snprintf(buf, sizeof(buf), "Color  %d K", v);
        toast_show(buf);
    }
}

static void effect_param_row_create(lv_obj_t *parent, size_t index, lv_color_t color)
{
    if (index >= WLED_EFFECT_PARAM_COUNT) return;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 58);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "Param");
    lv_obj_set_width(label, 170);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 30);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 128, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, effect_param_changed, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
    lv_obj_add_event_cb(slider, effect_param_changed, LV_EVENT_RELEASED, (void *)(intptr_t)index);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, "128");
    lv_obj_set_width(value, 42);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(value, THEME_FONT_LABEL, LV_PART_MAIN);

    s_effect_param_rows[index] = row;
    s_effect_param_labels[index] = label;
    s_effect_param_sliders[index] = slider;
    s_effect_param_value_labels[index] = value;
}

static void transition_changed(lv_event_t *e)
{
    if (s_ui_updating) return;
    lv_event_code_t code = lv_event_get_code(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (s_transition_label) {
        char b[16];
        snprintf(b, sizeof(b), "%d.%d s", v / 10, v % 10);
        label_set_text_if_changed(s_transition_label, b);
    }
    if (code == LV_EVENT_VALUE_CHANGED && !rate_limit_ms(&s_transition_update_ms, 120)) return;
    char json[32];
    snprintf(json, sizeof(json), "{\"transition\":%d}", v);
    cmd_tx_send_json(json);
}

/* Runtime-generated true gradients (RGB565) used by hue/CCT strips. */
#define LIGHTS_GRADIENT_W 256
static uint16_t s_hue_gradient_565[LIGHTS_GRADIENT_W];
static uint16_t s_cct_gradient_565[LIGHTS_GRADIENT_W];
static lv_image_dsc_t s_hue_gradient_dsc;
static lv_image_dsc_t s_cct_gradient_dsc;
static lv_obj_t *s_cct_gradient_img;
static uint16_t s_cct_gradient_min = 2000;
static uint16_t s_cct_gradient_max = 6500;
static bool s_gradients_ready;

static void lights_cct_gradient_update(uint16_t min_kelvin, uint16_t max_kelvin)
{
    if (min_kelvin < 1000) min_kelvin = 1000;
    if (max_kelvin > 10000) max_kelvin = 10000;
    if (max_kelvin <= min_kelvin) {
        min_kelvin = 2000;
        max_kelvin = 6500;
    }

    if (s_cct_gradient_min == min_kelvin && s_cct_gradient_max == max_kelvin) return;
    s_cct_gradient_min = min_kelvin;
    s_cct_gradient_max = max_kelvin;

    for (int i = 0; i < LIGHTS_GRADIENT_W; i++) {
        uint16_t kelvin = (uint16_t)(min_kelvin +
                           ((uint32_t)i * (uint32_t)(max_kelvin - min_kelvin)) /
                           (LIGHTS_GRADIENT_W - 1));
        s_cct_gradient_565[i] = lv_color_to_u16(kelvin_to_color(kelvin));
    }

    if (s_cct_gradient_img) {
        lv_image_set_src(s_cct_gradient_img, &s_cct_gradient_dsc);
        lv_obj_invalidate(s_cct_gradient_img);
    }
}

static void lights_gradients_init(void)
{
    if (s_gradients_ready) return;

    for (int i = 0; i < LIGHTS_GRADIENT_W; i++) {
        uint8_t hue = (uint8_t)((i * 255) / (LIGHTS_GRADIENT_W - 1));
        s_hue_gradient_565[i] = lv_color_to_u16(hue_to_color(hue));
        uint16_t kelvin = (uint16_t)(s_cct_gradient_min +
                           ((uint32_t)i * (uint32_t)(s_cct_gradient_max - s_cct_gradient_min)) /
                           (LIGHTS_GRADIENT_W - 1));
        s_cct_gradient_565[i] = lv_color_to_u16(kelvin_to_color(kelvin));
    }

    memset(&s_hue_gradient_dsc, 0, sizeof(s_hue_gradient_dsc));
    s_hue_gradient_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_hue_gradient_dsc.header.w = LIGHTS_GRADIENT_W;
    s_hue_gradient_dsc.header.h = 1;
    s_hue_gradient_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_hue_gradient_dsc.header.stride = LIGHTS_GRADIENT_W * 2;
    s_hue_gradient_dsc.data_size = LIGHTS_GRADIENT_W * 2;
    s_hue_gradient_dsc.data = (const uint8_t *)s_hue_gradient_565;

    memset(&s_cct_gradient_dsc, 0, sizeof(s_cct_gradient_dsc));
    s_cct_gradient_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_cct_gradient_dsc.header.w = LIGHTS_GRADIENT_W;
    s_cct_gradient_dsc.header.h = 1;
    s_cct_gradient_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_cct_gradient_dsc.header.stride = LIGHTS_GRADIENT_W * 2;
    s_cct_gradient_dsc.data_size = LIGHTS_GRADIENT_W * 2;
    s_cct_gradient_dsc.data = (const uint8_t *)s_cct_gradient_565;

    s_gradients_ready = true;
}

/* WLED-style hue slider: true gradient strip rendered from RGB565 image,
 * with transparent slider track/indicator so only the white-ring knob moves. */
static lv_obj_t *lights_hue_slider_create(lv_obj_t *parent, uint8_t slot_id)
{
    lights_gradients_init();

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), 48);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *strip = lv_obj_create(box);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(strip, 24, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(strip, true, LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(strip, lv_color_hex(0x232D42), LV_PART_MAIN);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *grad = lv_image_create(strip);
    lv_obj_set_size(grad, LV_PCT(100), LV_PCT(100));
    lv_image_set_src(grad, &s_hue_gradient_dsc);
    lv_image_set_inner_align(grad, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_clear_flag(grad, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *slider = lv_slider_create(box);
    lv_obj_set_size(slider, LV_PCT(100), 48);
    lv_obj_center(slider);
    lv_slider_set_range(slider, 0, 255);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, color_slot_changed, LV_EVENT_PRESSED, (void *)(intptr_t)slot_id);
    lv_obj_add_event_cb(slider, color_slot_changed, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)slot_id);
    lv_obj_add_event_cb(slider, color_slot_changed, LV_EVENT_RELEASED, (void *)(intptr_t)slot_id);
    lv_obj_add_event_cb(slider, color_slot_changed, LV_EVENT_PRESS_LOST, (void *)(intptr_t)slot_id);
    return slider;
}

/* Warm-to-cool slider uses the same true-gradient image strip pattern. */
static lv_obj_t *lights_cct_slider_create(lv_obj_t *parent)
{
    lights_gradients_init();

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), 48);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *strip = lv_obj_create(box);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(strip, 24, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(strip, true, LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(strip, lv_color_hex(0x232D42), LV_PART_MAIN);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *grad = lv_image_create(strip);
    lv_obj_set_size(grad, LV_PCT(100), LV_PCT(100));
    lv_image_set_src(grad, &s_cct_gradient_dsc);
    lv_image_set_inner_align(grad, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_clear_flag(grad, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_cct_gradient_img = grad;

    lv_obj_t *slider = lv_slider_create(box);
    lv_obj_set_size(slider, LV_PCT(100), 48);
    lv_obj_center(slider);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, kel_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, kel_changed, LV_EVENT_RELEASED, NULL);
    return slider;
}

static void lights_mode_select(int idx)
{
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    s_lights_picker_mode = (uint8_t)idx;
    for (int i = 0; i < 3; i++) {
        if (s_lights_picker_box[i]) {
            if (i == idx) lv_obj_clear_flag(s_lights_picker_box[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_lights_picker_box[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_lights_picker_chip[i]) {
            bool on = (i == idx);
            lv_obj_set_style_border_width(s_lights_picker_chip[i], on ? 3 : 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_lights_picker_chip[i],
                                          on ? lv_color_white() : lv_color_hex(0x263754),
                                          LV_PART_MAIN);
        }
    }
}

static void lights_mode_clicked(lv_event_t *e)
{
    lights_mode_select((int)(intptr_t)lv_event_get_user_data(e));
}

static void lights_apply_swatch(const lights_color_swatch_t *swatch)
{
    if (!swatch) return;

    if (swatch->kelvin) {
        uint16_t kelvin = swatch->value;
        lights_mode_select(2);
        slider_set_value_if_changed(s_kel_slider, kelvin);
        kelvin_labels_set(kelvin);
        kelvin_tint_set(kelvin);
        s_skip_lights_sync = true;
        led_state_set_kelvin(kelvin);
        s_skip_lights_sync = false;
        led_state_persist_current();
        lights_color_history_add(true, kelvin);
        toast_show("White applied");
        return;
    }

    if (s_lights_picker_mode == 2) lights_mode_select(0);
    uint8_t hue = (uint8_t)swatch->value;
    uint8_t slot = s_lights_picker_mode == 1 ? LIGHTS_COLOR_SLOT_SECONDARY : LIGHTS_COLOR_SLOT_PRIMARY;
    lv_obj_t *slider = slot == LIGHTS_COLOR_SLOT_PRIMARY ? s_color_slider : s_secondary_color_slider;
    lv_obj_t *label = slot == LIGHTS_COLOR_SLOT_PRIMARY ? s_color_label : s_secondary_color_label;
    slider_set_value_if_changed(slider, hue);
    color_label_set(label, hue);
    color_tint_set(slider, hue);
    send_current_hue_slots(true);
    lights_color_history_add(false, hue);
    toast_show(slot == LIGHTS_COLOR_SLOT_PRIMARY ? "Primary applied" : "Accent applied");
}

static void lights_color_swatch_clicked(lv_event_t *e)
{
    uintptr_t code = (uintptr_t)lv_event_get_user_data(e);
    if (code >= LIGHTS_SWATCH_PRESET_BASE) {
        size_t preset_index = code - LIGHTS_SWATCH_PRESET_BASE;
        if (preset_index < LIGHTS_COLOR_PRESET_COUNT) lights_apply_swatch(&s_color_presets[preset_index]);
        return;
    }

    size_t history_index = code;
    if (history_index < LIGHTS_COLOR_HISTORY_COUNT && s_color_history_valid[history_index]) {
        lights_apply_swatch(&s_color_history[history_index]);
    }
}

static lv_obj_t *lights_color_panel_section(lv_obj_t *parent, const char *label_text, lv_coord_t height)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_remove_style_all(section);
    lv_obj_set_size(section, LV_PCT(100), height);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(section, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(section, 6, LV_PART_MAIN);
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(section);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_SMALL, LV_PART_MAIN);
    return section;
}

static lv_obj_t *lights_color_swatch_button(lv_obj_t *parent, lv_coord_t size, uintptr_t code)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(button, lights_color_swatch_clicked, LV_EVENT_CLICKED, (void *)code);
    return button;
}

static lv_obj_t *lights_color_swatch_row(lv_obj_t *parent, lv_coord_t height, lv_coord_t gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static lv_obj_t *lights_current_color_chip_create(lv_obj_t *parent, int mode, const char *label_text)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 70, LV_PCT(100));
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(item, 6, LV_PART_MAIN);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *chip = lv_button_create(item);
    lv_obj_set_size(chip, LIGHTS_COLOR_CHIP_SIZE, LIGHTS_COLOR_CHIP_SIZE);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_hex(0x263754), LV_PART_MAIN);
    lv_obj_add_event_cb(chip, lights_mode_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)mode);
    s_lights_picker_chip[mode] = chip;

    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_SMALL, LV_PART_MAIN);
    return chip;
}

lv_obj_t *screen_lights_create(lv_obj_t *parent)
{
    lights_tuning_refresh();

    lv_obj_t *p = page_root(parent);
    s_lights_root = p;
    lv_obj_set_style_pad_row(p, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 4, LV_PART_MAIN);

    s_lights_home_page = lights_panel_create(p);
    s_lights_effects_page = lights_panel_create(p);
    s_lights_presets_page = lights_panel_create(p);
    lv_obj_set_height(s_lights_home_page, 1);
    lv_obj_set_flex_grow(s_lights_home_page, 1);

    /* WLED-style tab bar pinned above the panels (Color / Effects / Presets). */
    s_lights_tab_page[0] = s_lights_home_page;
    s_lights_tab_page[1] = s_lights_effects_page;
    s_lights_tab_page[2] = s_lights_presets_page;
    lv_obj_t *tabbar = lights_tabbar_create(p);
    lv_obj_move_to_index(tabbar, 0);

    /* ═══════════════════════════════════════════════════════════
     * DEVICE HERO — ecobee composition: giant brightness readout on
     * the left, tall vertical brightness slider in the middle, big
     * sliding power switch on the right.
     * ═══════════════════════════════════════════════════════════ */
    lv_obj_t *hero = lv_obj_create(s_lights_home_page);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(hero, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 18, LV_PART_MAIN);
    lv_obj_set_style_radius(hero, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hero, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hero, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_width(hero, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(hero, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    s_lights_conn_badge = NULL;
    s_lights_conn_label = NULL;
    s_lights_wled_detail = NULL;

    s_lights_timer_card = lv_obj_create(hero);
    lv_obj_remove_style_all(s_lights_timer_card);
    lv_obj_set_size(s_lights_timer_card, LIGHTS_TIMER_MODULE_W, LIGHTS_TIMER_MODULE_H);
    lv_obj_set_style_pad_all(s_lights_timer_card, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_lights_timer_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lights_timer_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_lights_timer_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_lights_timer_chip_row = lv_obj_create(s_lights_timer_card);
    lv_obj_remove_style_all(s_lights_timer_chip_row);
    lv_obj_set_size(s_lights_timer_chip_row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_lights_timer_chip_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lights_timer_chip_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_lights_timer_chip_row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(s_lights_timer_chip_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    static const struct { const char *label; uint16_t minutes; } timer_chips[] = {
        {"30m", 30}, {"1h", 60}, {"2h", 120},
    };
    for (size_t i = 0; i < sizeof(timer_chips) / sizeof(timer_chips[0]); i++) {
        lv_obj_t *chip = lv_button_create(s_lights_timer_chip_row);
        lv_obj_set_size(chip, LIGHTS_TIMER_MODULE_W, 96);
        lv_obj_set_style_radius(chip, 28, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chip, THEME_SURFACE_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chip, theme_surface_opa(), LV_PART_MAIN);
        lv_obj_set_style_border_color(chip, THEME_BORDER_COLOR, LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(chip, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(chip, lights_timer_chip_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)timer_chips[i].minutes);
        lv_obj_t *chip_label = lv_label_create(chip);
        lv_label_set_text(chip_label, timer_chips[i].label);
        lv_obj_set_style_text_color(chip_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_text_font(chip_label, THEME_FONT_LABEL, LV_PART_MAIN);
        lv_obj_center(chip_label);
    }

    s_lights_timer_active_panel = lv_obj_create(s_lights_timer_card);
    lv_obj_remove_style_all(s_lights_timer_active_panel);
    lv_obj_set_size(s_lights_timer_active_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_lights_timer_active_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_lights_timer_active_panel, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_lights_timer_active_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_lights_timer_active_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_lights_timer_active_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lights_timer_active_panel, LV_OBJ_FLAG_HIDDEN);

    s_lights_timer_arc = lv_arc_create(s_lights_timer_active_panel);
    lv_obj_set_size(s_lights_timer_arc, LIGHTS_TIMER_ARC_SIZE, LIGHTS_TIMER_ARC_SIZE);
    lv_arc_set_rotation(s_lights_timer_arc, 270);
    lv_arc_set_bg_angles(s_lights_timer_arc, 0, 360);
    lv_arc_set_range(s_lights_timer_arc, 0, 1000);
    lv_arc_set_mode(s_lights_timer_arc, LV_ARC_MODE_REVERSE);
    lv_arc_set_value(s_lights_timer_arc, 1000);
    lv_obj_set_style_arc_width(s_lights_timer_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_lights_timer_arc, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_lights_timer_arc, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lights_timer_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lights_timer_arc, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_lights_timer_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_lights_timer_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_lights_timer_arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(s_lights_timer_arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_lights_timer_time = lv_label_create(s_lights_timer_arc);
    lv_label_set_text(s_lights_timer_time, "--:--");
    lv_obj_set_style_text_color(s_lights_timer_time, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lights_timer_time, LIGHTS_TIMER_TIME_FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(s_lights_timer_time);

    s_lights_timer_detail = NULL;

    s_lights_timer_slider = lv_slider_create(s_lights_timer_active_panel);
    lv_obj_set_size(s_lights_timer_slider, LIGHTS_TIMER_SLIDER_W, LIGHTS_TIMER_SLIDER_H);
    lv_slider_set_orientation(s_lights_timer_slider, LV_SLIDER_ORIENTATION_VERTICAL);
    lv_slider_set_range(s_lights_timer_slider, 1, 240);
    lv_obj_set_style_bg_color(s_lights_timer_slider, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_lights_timer_slider, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_lights_timer_slider, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_lights_timer_slider, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_lights_timer_slider, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_lights_timer_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_lights_timer_slider, 28, LV_PART_MAIN);
    lv_obj_set_style_radius(s_lights_timer_slider, 28, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_lights_timer_slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_lights_timer_slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_lights_timer_slider, lights_timer_slider_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_lights_timer_slider, lights_timer_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_lights_timer_slider, lights_timer_slider_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_lights_timer_slider, lights_timer_slider_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_lights_timer_slider, lights_timer_slider_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(s_lights_timer_slider, lights_timer_slider_event, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *chip_panel = lv_obj_create(hero);
    lv_obj_remove_style_all(chip_panel);
    lv_obj_set_size(chip_panel, LIGHTS_COLOR_PANEL_W, PWR_SW_H);
    lv_obj_set_style_pad_all(chip_panel, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(chip_panel, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(chip_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chip_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(chip_panel, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip_panel, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip_panel, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(chip_panel, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(chip_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(chip_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *history_section = lights_color_panel_section(chip_panel, "HISTORY", 72);
    lv_obj_t *history_row = lights_color_swatch_row(history_section, 34, 8);
    for (size_t history_index = 0; history_index < LIGHTS_COLOR_HISTORY_COUNT; history_index++) {
        s_color_history_btn[history_index] = lights_color_swatch_button(history_row, 30, history_index);
    }
    lights_color_history_refresh();

    lv_obj_t *preset_section = lights_color_panel_section(chip_panel, "PRESETS", 72);
    lv_obj_t *preset_row = lights_color_swatch_row(preset_section, 34, 6);
    for (size_t preset_index = 0; preset_index < LIGHTS_COLOR_PRESET_COUNT; preset_index++) {
        s_color_preset_btn[preset_index] = lights_color_swatch_button(preset_row, 30,
                                                                      LIGHTS_SWATCH_PRESET_BASE + preset_index);
        lights_swatch_style(s_color_preset_btn[preset_index], &s_color_presets[preset_index], true);
    }

    lv_obj_t *current_section = lights_color_panel_section(chip_panel, "CURRENT", 122);
    lv_obj_t *current_row = lights_color_swatch_row(current_section, 78, 8);
    lights_current_color_chip_create(current_row, 0, "Primary");
    lights_current_color_chip_create(current_row, 1, "Accent");
    lights_current_color_chip_create(current_row, 2, "White");
    lv_obj_move_to_index(chip_panel, 0);

    /* Tall vertical brightness slider (ecobee set-point slider). */
    s_bri_slider = lv_slider_create(hero);
    lv_obj_set_size(s_bri_slider, 72, PWR_SW_H);
    lv_slider_set_range(s_bri_slider, 0, 100);
    lv_obj_set_style_bg_color(s_bri_slider, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bri_slider, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bri_slider, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bri_slider, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bri_slider, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bri_slider, THEME_ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bri_slider, 28, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bri_slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_bri_slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_bri_slider, bri_changed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_bri_slider, bri_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bri_slider, bri_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_bri_slider, bri_changed, LV_EVENT_PRESS_LOST, NULL);

    /* Bright / dim end glyphs drawn over the track. */
    lv_obj_t *bri_bright = lv_label_create(s_bri_slider);
    lv_label_set_text(bri_bright, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(bri_bright, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(bri_bright, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_align(bri_bright, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_clear_flag(bri_bright, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *bri_dim = lv_label_create(s_bri_slider);
    lv_label_set_text(bri_dim, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(bri_dim, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(bri_dim, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_align(bri_dim, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_clear_flag(bri_dim, LV_OBJ_FLAG_CLICKABLE);

    /* Big vertical power switch: pill track with a sliding knob. */
    s_power_btn = lv_obj_create(hero);
    lv_obj_remove_style_all(s_power_btn);
    lv_obj_set_size(s_power_btn, PWR_SW_W, PWR_SW_H);
    lv_obj_set_style_radius(s_power_btn, PWR_SW_W / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_power_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_power_btn, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_power_btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_power_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_power_btn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_power_btn, theme_shadow_width(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_power_btn, LV_OPA_20, LV_PART_MAIN);
    lv_obj_add_flag(s_power_btn, LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_power_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_power_btn, power_clicked, LV_EVENT_CLICKED, NULL);

    s_power_knob = lv_obj_create(s_power_btn);
    lv_obj_remove_style_all(s_power_knob);
    lv_obj_set_size(s_power_knob, PWR_KNOB, PWR_KNOB);
    lv_obj_set_pos(s_power_knob, PWR_KNOB_MARGIN, PWR_KNOB_Y_OFF);
    lv_obj_set_style_radius(s_power_knob, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_power_knob, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_power_knob, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_clear_flag(s_power_knob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_power_icon = lv_label_create(s_power_knob);
    lv_label_set_text(s_power_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(s_power_icon, THEME_FONT_XLARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_power_icon, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_center(s_power_icon);

    /* ── Home picker panel (opened by top circles) ── */
    lv_obj_t *mode_card = lv_obj_create(s_lights_home_page);
    lv_obj_remove_style_all(mode_card);
    lv_obj_set_size(mode_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(mode_card, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(mode_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mode_card, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(mode_card, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mode_card, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mode_card, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_width(mode_card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mode_card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mode_card, LV_OBJ_FLAG_SCROLLABLE);

    s_lights_picker_box[0] = lv_obj_create(mode_card);
    lv_obj_remove_style_all(s_lights_picker_box[0]);
    lv_obj_set_size(s_lights_picker_box[0], LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lights_picker_box[0], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_lights_picker_box[0], 10, LV_PART_MAIN);
    lv_obj_clear_flag(s_lights_picker_box[0], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *color_head = lv_obj_create(s_lights_picker_box[0]);
    lv_obj_remove_style_all(color_head);
    lv_obj_set_size(color_head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(color_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(color_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(color_head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *color_cap = lv_label_create(color_head);
    lv_label_set_text(color_cap, "Color");
    lv_obj_set_style_text_color(color_cap, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(color_cap, THEME_FONT_LABEL, LV_PART_MAIN);

    s_color_label = lv_label_create(color_head);
    lv_label_set_text(s_color_label, "Hue 0");
    lv_obj_set_style_text_color(s_color_label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_color_label, THEME_FONT_SMALL, LV_PART_MAIN);

    s_color_slider = lights_hue_slider_create(s_lights_picker_box[0], LIGHTS_COLOR_SLOT_PRIMARY);

    s_lights_picker_box[1] = lv_obj_create(mode_card);
    lv_obj_remove_style_all(s_lights_picker_box[1]);
    lv_obj_set_size(s_lights_picker_box[1], LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lights_picker_box[1], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_lights_picker_box[1], 10, LV_PART_MAIN);
    lv_obj_clear_flag(s_lights_picker_box[1], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *accent_head = lv_obj_create(s_lights_picker_box[1]);
    lv_obj_remove_style_all(accent_head);
    lv_obj_set_size(accent_head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(accent_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(accent_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(accent_head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *accent_cap = lv_label_create(accent_head);
    lv_label_set_text(accent_cap, "Accent");
    lv_obj_set_style_text_color(accent_cap, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(accent_cap, THEME_FONT_LABEL, LV_PART_MAIN);

    s_secondary_color_label = lv_label_create(accent_head);
    lv_label_set_text(s_secondary_color_label, "Hue 0");
    lv_obj_set_style_text_color(s_secondary_color_label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_secondary_color_label, THEME_FONT_SMALL, LV_PART_MAIN);

    s_secondary_color_slider = lights_hue_slider_create(s_lights_picker_box[1], LIGHTS_COLOR_SLOT_SECONDARY);

    s_lights_picker_box[2] = lv_obj_create(mode_card);
    lv_obj_remove_style_all(s_lights_picker_box[2]);
    lv_obj_set_size(s_lights_picker_box[2], LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_lights_picker_box[2], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_lights_picker_box[2], 10, LV_PART_MAIN);
    lv_obj_clear_flag(s_lights_picker_box[2], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *kel_head = lv_obj_create(s_lights_picker_box[2]);
    lv_obj_remove_style_all(kel_head);
    lv_obj_set_size(kel_head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kel_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kel_head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(kel_head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *kel_cap = lv_label_create(kel_head);
    lv_label_set_text(kel_cap, "White temperature");
    lv_obj_set_style_text_color(kel_cap, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(kel_cap, THEME_FONT_LABEL, LV_PART_MAIN);

    s_kel_label = lv_label_create(kel_head);
    lv_label_set_text(s_kel_label, "");
    lv_obj_set_style_text_color(s_kel_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_kel_label, THEME_FONT_LABEL, LV_PART_MAIN);

    s_kel_slider = lights_cct_slider_create(s_lights_picker_box[2]);

    lv_obj_t *kel_scale = lv_obj_create(s_lights_picker_box[2]);
    lv_obj_remove_style_all(kel_scale);
    lv_obj_set_size(kel_scale, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(kel_scale, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kel_scale, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(kel_scale, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *kel_warm = lv_label_create(kel_scale);
    lv_label_set_text(kel_warm, "Warm");
    lv_obj_set_style_text_color(kel_warm, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(kel_warm, THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *kel_cool = lv_label_create(kel_scale);
    lv_label_set_text(kel_cool, "Cool");
    lv_obj_set_style_text_color(kel_cool, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(kel_cool, THEME_FONT_SMALL, LV_PART_MAIN);

    /* ── Effect card: effect picker + palette picker with preview ── */
    lv_obj_t *effect_card = lv_obj_create(s_lights_effects_page);
    lv_obj_remove_style_all(effect_card);
    lv_obj_set_size(effect_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(effect_card, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(effect_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(effect_card, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(effect_card, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(effect_card, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(effect_card, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_width(effect_card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(effect_card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(effect_card, LV_OBJ_FLAG_SCROLLABLE);

    theme_section_header(effect_card, "EFFECT");

    lv_obj_t *fx_row = lv_obj_create(effect_card);
    lv_obj_remove_style_all(fx_row);
    lv_obj_set_size(fx_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fx_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fx_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fx_row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(fx_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *fx_prev = lv_button_create(fx_row);
    lv_obj_set_size(fx_prev, 48, 48);
    lv_obj_set_style_radius(fx_prev, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(fx_prev, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(fx_prev, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(fx_prev, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(fx_prev, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(fx_prev, fx_prev_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_lbl = lv_label_create(fx_prev);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(prev_lbl);

    s_effect_dropdown = lv_dropdown_create(fx_row);
    lv_obj_set_height(s_effect_dropdown, 48);
    lv_obj_set_flex_grow(s_effect_dropdown, 1);
    lv_dropdown_set_options(s_effect_dropdown, wled_effect_catalog_options());
    lv_dropdown_set_symbol(s_effect_dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(s_effect_dropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_bg_color(s_effect_dropdown, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_effect_dropdown, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_effect_dropdown, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_effect_dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_effect_dropdown, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_effect_dropdown, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_effect_dropdown, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_effect_dropdown, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_effect_dropdown, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(s_effect_dropdown, fx_dropdown_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *fx_next = lv_button_create(fx_row);
    lv_obj_set_size(fx_next, 48, 48);
    lv_obj_set_style_radius(fx_next, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(fx_next, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(fx_next, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(fx_next, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(fx_next, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(fx_next, fx_next_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_lbl = lv_label_create(fx_next);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_center(next_lbl);

    theme_section_header(effect_card, "PALETTE");

    s_pal_dropdown = lv_dropdown_create(effect_card);
    lv_obj_set_size(s_pal_dropdown, LV_PCT(100), 48);
    lv_dropdown_set_options(s_pal_dropdown, wled_palette_catalog_options());
    lv_dropdown_set_symbol(s_pal_dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(s_pal_dropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_bg_color(s_pal_dropdown, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pal_dropdown, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pal_dropdown, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pal_dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pal_dropdown, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pal_dropdown, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_pal_dropdown, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_pal_dropdown, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_pal_dropdown, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(s_pal_dropdown, pal_dropdown_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Palette preview strip attached to the picker (4 solid blocks; no
     * gradient fill on this build). */
    lv_obj_t *pal_preview = lv_obj_create(effect_card);
    lv_obj_remove_style_all(pal_preview);
    lv_obj_set_size(pal_preview, LV_PCT(100), 16);
    lv_obj_set_style_radius(pal_preview, 8, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(pal_preview, true, LV_PART_MAIN);
    lv_obj_set_flex_flow(pal_preview, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(pal_preview, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < WLED_PALETTE_PREVIEW_COUNT; i++) {
        lv_obj_t *blk = lv_obj_create(pal_preview);
        lv_obj_remove_style_all(blk);
        lv_obj_set_flex_grow(blk, 1);
        lv_obj_set_height(blk, LV_PCT(100));
        lv_obj_set_style_bg_color(blk, lv_color_hex(0x202833), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s_pal_preview[i] = blk;
    }

    s_preset_list = lv_obj_create(s_lights_presets_page);
    lv_obj_remove_style_all(s_preset_list);
    lv_obj_set_size(s_preset_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(s_preset_list, 440, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_preset_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_preset_list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_preset_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_preset_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (!s_preset_command_keyboard) {
        s_preset_command_keyboard = lv_keyboard_create(lv_screen_active());
        lv_obj_set_size(s_preset_command_keyboard, LV_PCT(100), 220);
        lv_obj_align(s_preset_command_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(s_preset_command_keyboard, THEME_CARD_COLOR, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_preset_command_keyboard, 0, LV_PART_MAIN);
        lv_obj_add_flag(s_preset_command_keyboard, LV_OBJ_FLAG_HIDDEN);
    }

    preset_panels_rebuild(true);

    lv_obj_t *tune_card = lv_obj_create(s_lights_effects_page);
    lv_obj_remove_style_all(tune_card);
    lv_obj_set_size(tune_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(tune_card, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(tune_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tune_card, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(tune_card, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tune_card, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tune_card, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tune_card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tune_card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tune_card, LV_OBJ_FLAG_SCROLLABLE);

    theme_section_header(tune_card, "FINE TUNING");

    lv_color_t param_colors[WLED_EFFECT_PARAM_COUNT] = {
        THEME_PRIMARY_COLOR,
        THEME_ACCENT_COLOR,
        lv_color_hex(0x2EC4B6),
        lv_color_hex(0xFFCA3A),
        lv_color_hex(0xB967FF),
    };
    for (size_t i = 0; i < WLED_EFFECT_PARAM_COUNT; i++) {
        effect_param_row_create(tune_card, i, param_colors[i]);
    }
    lights_effect_selector_set(0);

    /* Crossfade between colors/effects (power-user setting). */
    lv_obj_t *xfade_row = lv_obj_create(tune_card);
    lv_obj_remove_style_all(xfade_row);
    lv_obj_set_size(xfade_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(xfade_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(xfade_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(xfade_row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(xfade_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *xfade_cap = lv_label_create(xfade_row);
    lv_label_set_text(xfade_cap, "Crossfade");
    lv_obj_set_style_text_color(xfade_cap, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(xfade_cap, THEME_FONT_LABEL, LV_PART_MAIN);

    s_transition_label = lv_label_create(xfade_row);
    lv_label_set_text(s_transition_label, "0.7 s");
    lv_obj_set_style_text_color(s_transition_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_transition_label, THEME_FONT_LABEL, LV_PART_MAIN);

    s_transition_slider = lv_slider_create(tune_card);
    lv_obj_set_width(s_transition_slider, LV_PCT(100));
    lv_obj_set_height(s_transition_slider, 18);
    lv_slider_set_range(s_transition_slider, 0, 100);
    lv_obj_set_style_bg_color(s_transition_slider, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_radius(s_transition_slider, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_transition_slider, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_transition_slider, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_transition_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(s_transition_slider, transition_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_transition_slider, transition_changed, LV_EVENT_RELEASED, NULL);

    /* Subscribe + initial sync */
    led_state_subscribe(lights_state_cb, NULL);
    wled_state_subscribe(lights_wled_state_cb, NULL);
    s_lights_led_dirty = false;
    s_lights_wled_dirty = false;
    led_state_t init; led_state_get(&init);
    lights_color_history_seed(&init);
    lights_apply_led_state(&init);
    wled_state_t winit; wled_state_get(&winit);
    lights_apply_wled_state(&winit);
    lights_status_refresh(NULL);
    lights_timer_panel_refresh(NULL);
    lv_timer_create(lights_sync_refresh, 150, NULL);
    lv_timer_create(lights_status_refresh, 1500, NULL);
    lv_timer_create(lights_timer_panel_refresh, 500, NULL);
    lights_tab_select(0);
    lights_mode_select(0);

    return p;
}

/* ============================================================
 * WEATHER PAGE
 * ============================================================ */

static lv_obj_t *s_wx_temp;
static lv_obj_t *s_wx_feels;
static lv_obj_t *s_wx_hum;
static lv_obj_t *s_wx_wind;
static lv_obj_t *s_wx_cond;
static lv_obj_t *s_wx_desc;
static lv_obj_t *s_wx_age;
static lv_obj_t *s_wx_icon;
static lv_obj_t *s_wx_hilo;
static lv_obj_t *s_wx_pressure;
static lv_obj_t *s_wx_visibility;
static lv_obj_t *s_wx_clouds;
static lv_obj_t *s_wx_uv;
static lv_obj_t *s_wx_gust;
static lv_obj_t *s_wx_precip;
static lv_obj_t *s_wx_sunrise;
static lv_obj_t *s_wx_sunset;
static lv_obj_t *s_wx_dewpoint;
static lv_obj_t *s_wx_daylight;
static lv_obj_t *s_wx_city;
static lv_obj_t *s_wx_observed;
static lv_obj_t *s_wx_moon_icon;
static lv_obj_t *s_wx_moon_label;
static lv_obj_t *s_wx_wind_compass;
static lv_obj_t *s_wx_wind_degree;
static lv_obj_t *s_wx_wind_needle;
static lv_obj_t *s_wx_wind_arrow_head;
static lv_point_precise_t s_wx_wind_points[2];
static lv_point_precise_t s_wx_wind_head_points[3];

#define WX_HISTORY_CHART_POINTS WEATHER_HISTORY_MAX_POINTS
#define WX_HISTORY_WINDOW_SECONDS (24u * 60u * 60u)
#define WX_HISTORY_SLOT_SECONDS (WX_HISTORY_WINDOW_SECONDS / WX_HISTORY_CHART_POINTS)
#define WX_HISTORY_VALID_NOW_UTC 1704067200u
#define WX_HISTORY_SERIES_REAL_A 0
#define WX_HISTORY_SERIES_REAL_B 1
#define WX_HISTORY_SERIES_FILL_A 2
#define WX_HISTORY_SERIES_FILL_B 3
#define WX_HISTORY_SERIES_COUNT 4
#define WX_GRAPH_TIME_LABELS 5
typedef int32_t wx_history_chart_values_t[WX_HISTORY_SERIES_COUNT][WX_HISTORY_CHART_POINTS];

typedef enum {
    WX_GRAPH_TEMP,
    WX_GRAPH_HUM_CLOUDS,
    WX_GRAPH_PRESSURE,
    WX_GRAPH_WIND,
    WX_GRAPH_PRECIP,
    WX_GRAPH_COUNT,
} wx_graph_mode_t;

typedef struct {
    lv_obj_t *carousel;
    lv_obj_t *chart[WX_GRAPH_COUNT];
    lv_chart_series_t *fill_series[WX_GRAPH_COUNT][2];
    lv_chart_series_t *series[WX_GRAPH_COUNT][2];
    lv_obj_t *data_a[WX_GRAPH_COUNT];
    lv_obj_t *data_b[WX_GRAPH_COUNT];
    lv_obj_t *axis_hi[WX_GRAPH_COUNT];
    lv_obj_t *axis_lo[WX_GRAPH_COUNT];
    lv_obj_t *time_label[WX_GRAPH_COUNT][WX_GRAPH_TIME_LABELS];
    lv_obj_t *status;
} wx_history_graph_view_t;

static wx_history_graph_view_t s_wx_history_view;
static weather_history_t *s_wx_history_cache;
static wx_history_chart_values_t *s_wx_history_values;

#define WX_FORECAST_SLOTS WEATHER_FORECAST_DAYS
static lv_obj_t *s_wx_day_name[WX_FORECAST_SLOTS];
static lv_obj_t *s_wx_day_icon[WX_FORECAST_SLOTS];
static lv_obj_t *s_wx_day_temp[WX_FORECAST_SLOTS];
static lv_obj_t *s_wx_day_pop[WX_FORECAST_SLOTS];
static lv_obj_t *s_wx_day_cond[WX_FORECAST_SLOTS];

#define WX_FORECAST_GRAPH_POINTS WEATHER_FORECAST_HOURS
typedef int32_t wx_forecast_chart_values_t[WX_FORECAST_GRAPH_POINTS];

typedef enum {
    WX_FC_GRAPH_PRESSURE,
    WX_FC_GRAPH_TEMP,
    WX_FC_GRAPH_HUMIDITY,
    WX_FC_GRAPH_PRECIP,
    WX_FC_GRAPH_COUNT,
} wx_forecast_graph_t;

typedef struct {
    lv_obj_t *carousel;
    lv_obj_t *chart[WX_FC_GRAPH_COUNT];
    lv_chart_series_t *series[WX_FC_GRAPH_COUNT];
    lv_obj_t *data_hi[WX_FC_GRAPH_COUNT];
    lv_obj_t *data_lo[WX_FC_GRAPH_COUNT];
    lv_obj_t *axis_hi[WX_FC_GRAPH_COUNT];
    lv_obj_t *axis_lo[WX_FC_GRAPH_COUNT];
    lv_obj_t *time_label[WX_FC_GRAPH_COUNT][WX_GRAPH_TIME_LABELS];
    lv_obj_t *status;
} wx_forecast_graph_view_t;

static wx_forecast_graph_view_t s_wx_fc_view;
static wx_forecast_graph_view_t s_idle_fc_view;
static lv_timer_t *s_wx_fc_cycle_timer;
static wx_forecast_chart_values_t *s_wx_fc_values;
#define WX_CAROUSEL_MANUAL_PAUSE_MS 15000u
#define WX_CAROUSEL_AUTO_SCROLL_GUARD_MS 2000u
static uint32_t s_wx_carousel_manual_pause_until_ms;
static uint32_t s_wx_carousel_auto_scroll_guard_until_ms;
static lv_obj_t *s_idle_screen;
static bool s_idle_graph_visible;

static lv_obj_t *s_weather_page_root;
static lv_timer_t *s_weather_refresh_timer;
static uint32_t s_weather_activate_refresh_ms;

static app_tuning_config_t load_tuning_config(void)
{
    app_tuning_config_t cfg;
    if (app_config_tuning_load(&cfg) != ESP_OK) app_config_tuning_defaults(&cfg);
    return cfg;
}

static uint32_t weather_page_update_period_ms(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    return (uint32_t)cfg.weather_page_update_s * 1000u;
}

static void weather_refresh_timer_apply(void)
{
    if (s_weather_refresh_timer) lv_timer_set_period(s_weather_refresh_timer, weather_page_update_period_ms());
}

static uint32_t weather_graph_cycle_period_ms(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    return (uint32_t)cfg.weather_graph_cycle_s * 1000u;
}

static void weather_graph_cycle_timer_apply(void)
{
    if (s_wx_fc_cycle_timer) {
        lv_timer_set_period(s_wx_fc_cycle_timer, weather_graph_cycle_period_ms());
        lv_timer_reset(s_wx_fc_cycle_timer);
    }
}

static void weather_graph_cycle_timer_reset(void)
{
    if (s_wx_fc_cycle_timer) lv_timer_reset(s_wx_fc_cycle_timer);
}

static weather_history_t *wx_history_cache_get(void)
{
    if (s_wx_history_cache) return s_wx_history_cache;
    s_wx_history_cache = heap_caps_calloc(1, sizeof(*s_wx_history_cache),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wx_history_cache;
}

static wx_history_chart_values_t *wx_history_values_get(void)
{
    if (s_wx_history_values) return s_wx_history_values;
    s_wx_history_values = heap_caps_calloc(WX_GRAPH_COUNT, sizeof(*s_wx_history_values),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wx_history_values;
}

static wx_forecast_chart_values_t *wx_forecast_values_get(void)
{
    if (s_wx_fc_values) return s_wx_fc_values;
    s_wx_fc_values = heap_caps_calloc(WX_FC_GRAPH_COUNT, sizeof(*s_wx_fc_values),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_wx_fc_values;
}

static uint8_t wx_carousel_index(lv_obj_t *carousel, uint8_t count)
{
    if (!carousel || count == 0) return 0;
    lv_coord_t width = lv_obj_get_width(carousel);
    if (width <= 0) return 0;
    lv_coord_t x = lv_obj_get_scroll_x(carousel);
    int32_t idx = (x + width / 2) / width;
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;
    return (uint8_t)idx;
}

static void wx_carousel_scroll_to(lv_obj_t *carousel, uint8_t index, uint8_t count, lv_anim_enable_t anim)
{
    if (!carousel || count == 0) return;
    if (index >= count) index = 0;
    lv_coord_t width = lv_obj_get_width(carousel);
    if (width <= 0) return;
    lv_obj_scroll_to_x(carousel, (lv_coord_t)index * width, anim);
}

static void wx_carousel_advance(lv_obj_t *carousel, uint8_t count)
{
    if (!carousel || count == 0 || lv_obj_has_flag(carousel, LV_OBJ_FLAG_HIDDEN)) return;
    uint8_t index = wx_carousel_index(carousel, count);
    s_wx_carousel_auto_scroll_guard_until_ms = ui_now_ms() + WX_CAROUSEL_AUTO_SCROLL_GUARD_MS;
    wx_carousel_scroll_to(carousel, (uint8_t)((index + 1) % count), count, LV_ANIM_ON);
}

static void wx_carousel_manual_scroll_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    bool manual = false;

    if (hold_active_until(s_wx_carousel_auto_scroll_guard_until_ms)) return;

    if (lv_event_get_code(e) == LV_EVENT_SCROLL_THROW_BEGIN) {
        manual = true;
    } else {
        lv_indev_t *indev = lv_indev_active();
        if (indev && lv_indev_get_scroll_obj(indev) == target) manual = true;
    }

    if (manual) {
        s_wx_carousel_manual_pause_until_ms = ui_now_ms() + WX_CAROUSEL_MANUAL_PAUSE_MS;
        weather_graph_cycle_timer_reset();
    }
}

static void wx_carousel_attach_manual_pause(lv_obj_t *carousel)
{
    if (!carousel) return;
    lv_obj_add_event_cb(carousel, wx_carousel_manual_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(carousel, wx_carousel_manual_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    lv_obj_add_event_cb(carousel, wx_carousel_manual_scroll_cb, LV_EVENT_SCROLL_THROW_BEGIN, NULL);
}

static void weather_graph_cycle_cb(lv_timer_t *timer)
{
    (void)timer;
    if (hold_active_until(s_wx_carousel_manual_pause_until_ms)) return;
    if (s_wx_fc_view.carousel && s_weather_page_root && !lv_obj_has_flag(s_weather_page_root, LV_OBJ_FLAG_HIDDEN)) {
        wx_carousel_advance(s_wx_fc_view.carousel, WX_FC_GRAPH_COUNT);
    }
    if (s_wx_history_view.carousel && s_weather_page_root && !lv_obj_has_flag(s_weather_page_root, LV_OBJ_FLAG_HIDDEN)) {
        wx_carousel_advance(s_wx_history_view.carousel, WX_GRAPH_COUNT);
    }
    if (s_idle_fc_view.carousel && s_idle_screen && lv_screen_active() == s_idle_screen && s_idle_graph_visible) {
        wx_carousel_advance(s_idle_fc_view.carousel, WX_FC_GRAPH_COUNT);
    }
}

static const char *weather_symbol(const char *condition, const char *icon_code, bool is_night)
{
    if (icon_code && icon_code[0]) {
        bool icon_night = icon_code[2] == 'n' ? true : is_night;
        if (strncmp(icon_code, "01", 2) == 0) return icon_night ? WI_NIGHT_CLEAR  : WI_DAY_SUNNY;
        if (strncmp(icon_code, "02", 2) == 0) return icon_night ? WI_NIGHT_CLOUDY : WI_DAY_CLOUDY;
        if (strncmp(icon_code, "03", 2) == 0) return icon_night ? WI_NIGHT_CLOUDY : WI_CLOUD;
        if (strncmp(icon_code, "04", 2) == 0) return WI_CLOUD;
        if (strncmp(icon_code, "09", 2) == 0) return WI_RAIN;
        if (strncmp(icon_code, "10", 2) == 0) return icon_night ? WI_NIGHT_RAIN   : WI_DAY_RAIN;
        if (strncmp(icon_code, "11", 2) == 0) return icon_night ? WI_NIGHT_THUNDER: WI_DAY_THUNDER;
        if (strncmp(icon_code, "13", 2) == 0) return icon_night ? WI_NIGHT_SNOW   : WI_DAY_SNOW;
        if (strncmp(icon_code, "50", 2) == 0) return icon_night ? WI_NIGHT_FOG    : WI_DAY_FOG;
    }

    if (!condition || !condition[0]) return WI_NA;
    if (strstr(condition, "Thunder"))   return is_night ? WI_NIGHT_THUNDER : WI_DAY_THUNDER;
    if (strstr(condition, "Drizzle"))   return is_night ? WI_NIGHT_RAIN    : WI_DAY_RAIN;
    if (strstr(condition, "Rain"))      return is_night ? WI_NIGHT_RAIN    : WI_DAY_RAIN;
    if (strstr(condition, "Snow"))      return is_night ? WI_NIGHT_SNOW    : WI_DAY_SNOW;
    if (strstr(condition, "Mist") || strstr(condition, "Fog") ||
        strstr(condition, "Haze") || strstr(condition, "Smoke") ||
        strstr(condition, "Dust") || strstr(condition, "Sand") ||
        strstr(condition, "Ash"))       return is_night ? WI_NIGHT_FOG     : WI_DAY_FOG;
    if (strstr(condition, "Tornado"))   return WI_TORNADO;
    if (strstr(condition, "Squall"))    return WI_STRONG_WIND;
    if (strstr(condition, "Cloud"))     return is_night ? WI_NIGHT_CLOUDY  : WI_DAY_CLOUDY;
    if (strstr(condition, "Clear"))     return is_night ? WI_NIGHT_CLEAR   : WI_DAY_SUNNY;
    return WI_NA;
}

static bool moon_phase_info(const weather_state_t *w, const char **icon, const char **label,
                            const char **compact_label)
{
    static const char *icons[28] = {
        WI_MOON_NEW,
        WI_MOON_WAXING_CRESCENT_1, WI_MOON_WAXING_CRESCENT_2, WI_MOON_WAXING_CRESCENT_3,
        WI_MOON_WAXING_CRESCENT_4, WI_MOON_WAXING_CRESCENT_5, WI_MOON_WAXING_CRESCENT_6,
        WI_MOON_FIRST_QUARTER,
        WI_MOON_WAXING_GIBBOUS_1, WI_MOON_WAXING_GIBBOUS_2, WI_MOON_WAXING_GIBBOUS_3,
        WI_MOON_WAXING_GIBBOUS_4, WI_MOON_WAXING_GIBBOUS_5, WI_MOON_WAXING_GIBBOUS_6,
        WI_MOON_FULL,
        WI_MOON_WANING_GIBBOUS_1, WI_MOON_WANING_GIBBOUS_2, WI_MOON_WANING_GIBBOUS_3,
        WI_MOON_WANING_GIBBOUS_4, WI_MOON_WANING_GIBBOUS_5, WI_MOON_WANING_GIBBOUS_6,
        WI_MOON_THIRD_QUARTER,
        WI_MOON_WANING_CRESCENT_1, WI_MOON_WANING_CRESCENT_2, WI_MOON_WANING_CRESCENT_3,
        WI_MOON_WANING_CRESCENT_4, WI_MOON_WANING_CRESCENT_5, WI_MOON_WANING_CRESCENT_6,
    };
    static const char *labels[8] = {
        "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
        "Full moon", "Waning gibbous", "Third quarter", "Waning crescent",
    };
    static const char *compact_labels[8] = {
        "New moon", "Waxing", "First quarter", "Waxing",
        "Full moon", "Waning", "Third quarter", "Waning",
    };

    uint32_t utc = w && w->observed_utc ? w->observed_utc : (uint32_t)time(NULL);
    if (utc < 1704067200u) return false;

    const double synodic_month_days = 29.530588853;
    const double reference_new_moon_utc = 947182440.0;
    double age_days = fmod((((double)utc - reference_new_moon_utc) / 86400.0), synodic_month_days);
    if (age_days < 0.0) age_days += synodic_month_days;
    double fraction = age_days / synodic_month_days;
    uint8_t icon_index = (uint8_t)floor((fraction * 28.0) + 0.5);
    uint8_t label_index = (uint8_t)floor((fraction * 8.0) + 0.5);
    if (icon_index >= 28) icon_index = 0;
    label_index &= 7;

    if (icon) *icon = icons[icon_index];
    if (label) *label = labels[label_index];
    if (compact_label) *compact_label = compact_labels[label_index];
    return true;
}

static void moon_phase_labels_set(lv_obj_t *icon_obj, lv_obj_t *label_obj,
                                  const weather_state_t *w, bool compact)
{
    const char *icon = WI_NA;
    const char *label = "Moon —";
    const char *compact_label = label;
    if (moon_phase_info(w, &icon, &label, &compact_label)) {
        label = compact ? compact_label : label;
    }
    label_set_text_if_changed(icon_obj, icon);
    label_set_text_if_changed(label_obj, label);
}

/* Is the current wall-clock time after sunset / before sunrise? */
static bool weather_is_night(const weather_state_t *w)
{
    time_t now = time(NULL);
    if (w && w->sunrise_utc && w->sunset_utc) {
        return ((uint32_t)now < w->sunrise_utc) || ((uint32_t)now > w->sunset_utc);
    }
    struct tm lt;
    localtime_r(&now, &lt);
    return lt.tm_hour < 6 || lt.tm_hour >= 19;
}

/* Accent color matching a condition for icon tinting. */
static lv_color_t weather_color(const char *condition)
{
    if (!condition || !condition[0]) return THEME_TEXT_MUTED;
    if (strstr(condition, "Thunder"))   return lv_color_hex(0xFFB020);
    if (strstr(condition, "Rain") || strstr(condition, "Drizzle"))
                                        return lv_color_hex(0x4FA8FF);
    if (strstr(condition, "Snow"))      return lv_color_hex(0xE3F2FF);
    if (strstr(condition, "Cloud"))     return lv_color_hex(0x9AAACD);
    if (strstr(condition, "Clear"))     return lv_color_hex(0xFFD23F);
    if (strstr(condition, "Mist") || strstr(condition, "Fog") ||
        strstr(condition, "Haze") || strstr(condition, "Smoke"))
                                        return lv_color_hex(0xB8C1E6);
    return THEME_PRIMARY_COLOR;
}

static const char *wind_compass(uint16_t deg)
{
    static const char *pts[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    int idx = ((int)deg + 11) / 22;  /* 22.5 deg bucket, rounded */
    idx %= 16;
    return pts[idx];
}

static int32_t dew_point_f(int32_t temp_f, uint8_t humidity_pct)
{
    if (humidity_pct > 100) humidity_pct = 100;
    int32_t temp_c_x10 = ((temp_f - 32) * 50) / 9;
    int32_t dew_c_x10 = temp_c_x10 - (int32_t)(100 - humidity_pct) * 2;
    return ((dew_c_x10 * 9) + (dew_c_x10 >= 0 ? 25 : -25)) / 50 + 32;
}

static void format_duration_hm(char *out, size_t n, uint32_t seconds)
{
    uint32_t minutes = seconds / 60;
    snprintf(out, n, "%luh %02lum", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
}

static void format_local_hm(char *out, size_t n, uint32_t utc, int32_t tz)
{
    if (!utc) { snprintf(out, n, "--:--"); return; }
    time_t t = (time_t)utc + tz;
    struct tm tm; gmtime_r(&t, &tm);
    int hour12 = tm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(out, n, "%d:%02d %s", hour12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

static void format_local_weekday(char *out, size_t n, uint32_t utc, int32_t tz)
{
    static const char *names[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    if (!utc) { snprintf(out, n, "---"); return; }
    time_t t = (time_t)utc + tz;
    struct tm tm; gmtime_r(&t, &tm);
    snprintf(out, n, "%s", names[tm.tm_wday % 7]);
}

static int32_t wx_hpa_to_inhg_x100(uint16_t hpa)
{
    return (int32_t)(((uint32_t)hpa * 2953u + 500u) / 1000u);
}

static uint16_t wx_mm_x10_to_in_x100(uint16_t mm_x10)
{
    uint32_t value = ((uint32_t)mm_x10 * 100u + 127u) / 254u;
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

static void wx_format_x100(char *out, size_t out_len, int32_t value, const char *unit)
{
    if (!out || out_len == 0) return;
    int64_t mag = value;
    bool neg = mag < 0;
    if (neg) mag = -mag;
    if (unit && unit[0]) {
        snprintf(out, out_len, "%s%lld.%02lld %s", neg ? "-" : "",
                 (long long)(mag / 100), (long long)(mag % 100), unit);
    } else {
        snprintf(out, out_len, "%s%lld.%02lld", neg ? "-" : "",
                 (long long)(mag / 100), (long long)(mag % 100));
    }
}

static void wx_format_pressure_inhg(char *out, size_t out_len, uint16_t hpa)
{
    wx_format_x100(out, out_len, wx_hpa_to_inhg_x100(hpa), "inHg");
}

static void wx_format_precip_1h(char *out, size_t out_len, uint16_t rain_mm_x10, uint16_t snow_mm_x10)
{
    if (!out || out_len == 0) return;
    uint16_t rain_in_x100 = wx_mm_x10_to_in_x100(rain_mm_x10);
    uint16_t snow_in_x100 = wx_mm_x10_to_in_x100(snow_mm_x10);
    if (rain_mm_x10 || snow_mm_x10) {
        char rain[16];
        char snow[16];
        wx_format_x100(rain, sizeof(rain), rain_in_x100, NULL);
        wx_format_x100(snow, sizeof(snow), snow_in_x100, NULL);
        snprintf(out, out_len, "R %s  S %s in", rain, snow);
    } else {
        snprintf(out, out_len, "0.00 in/h");
    }
}

static void wx_wind_visual_set(const weather_state_t *w)
{
    if (!w || !w->valid) {
        label_set_text_if_changed(s_wx_wind_compass, "--");
        label_set_text_if_changed(s_wx_wind_degree, "No wind data");
        if (s_wx_wind_needle) lv_obj_add_flag(s_wx_wind_needle, LV_OBJ_FLAG_HIDDEN);
        if (s_wx_wind_arrow_head) lv_obj_add_flag(s_wx_wind_arrow_head, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    uint16_t deg = w->wind_deg % 360;
    const char *dir = wind_compass(deg);
    char b[48];
    label_set_text_if_changed(s_wx_wind_compass, dir);
    snprintf(b, sizeof(b), "%u°  %u.%u mph", deg, w->wind_mph_x10 / 10, w->wind_mph_x10 % 10);
    label_set_text_if_changed(s_wx_wind_degree, b);

    if (s_wx_wind_needle && s_wx_wind_arrow_head) {
        const float rad = (float)deg * 0.01745329252f;
        const float dx = sinf(rad);
        const float dy = -cosf(rad);
        const float px = -dy;
        const float py = dx;
        const float cx = 62.0f;
        const float cy = 62.0f;
        const float tip_radius = 47.0f;
        const float head_len = 15.0f;
        const float head_half = 8.0f;
        float tip_x = cx + dx * tip_radius;
        float tip_y = cy + dy * tip_radius;
        float base_x = tip_x - dx * head_len;
        float base_y = tip_y - dy * head_len;

        s_wx_wind_points[0].x = 62;
        s_wx_wind_points[0].y = 62;
        s_wx_wind_points[1].x = (lv_coord_t)roundf(tip_x);
        s_wx_wind_points[1].y = (lv_coord_t)roundf(tip_y);
        s_wx_wind_head_points[0].x = (lv_coord_t)roundf(base_x + px * head_half);
        s_wx_wind_head_points[0].y = (lv_coord_t)roundf(base_y + py * head_half);
        s_wx_wind_head_points[1].x = (lv_coord_t)roundf(tip_x);
        s_wx_wind_head_points[1].y = (lv_coord_t)roundf(tip_y);
        s_wx_wind_head_points[2].x = (lv_coord_t)roundf(base_x - px * head_half);
        s_wx_wind_head_points[2].y = (lv_coord_t)roundf(base_y - py * head_half);
        lv_line_set_points_mutable(s_wx_wind_needle, s_wx_wind_points, 2);
        lv_line_set_points_mutable(s_wx_wind_arrow_head, s_wx_wind_head_points, 3);
        lv_obj_clear_flag(s_wx_wind_needle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wx_wind_arrow_head, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_wx_wind_needle);
        lv_obj_invalidate(s_wx_wind_arrow_head);
    }
}

static const char *wx_graph_title(wx_graph_mode_t mode)
{
    switch (mode) {
        case WX_GRAPH_TEMP:       return "Temperature";
        case WX_GRAPH_HUM_CLOUDS: return "Humidity + Clouds";
        case WX_GRAPH_PRESSURE:   return "Pressure";
        case WX_GRAPH_WIND:       return "Wind";
        case WX_GRAPH_PRECIP:     return "Precip 1h";
        default:                  return "Weather";
    }
}

static const char *wx_graph_series_label(wx_graph_mode_t mode, uint8_t series)
{
    switch (mode) {
        case WX_GRAPH_TEMP:       return series ? "Feels" : "Temp";
        case WX_GRAPH_HUM_CLOUDS: return series ? "Clouds" : "Humidity";
        case WX_GRAPH_PRESSURE:   return series ? "Ground" : "Pressure";
        case WX_GRAPH_WIND:       return series ? "Gust" : "Wind";
        case WX_GRAPH_PRECIP:     return series ? "Snow" : "Rain";
        default:                  return series ? "B" : "A";
    }
}

static lv_color_t wx_graph_series_color(wx_graph_mode_t mode, uint8_t series)
{
    switch (mode) {
        case WX_GRAPH_TEMP:       return series ? lv_color_hex(0xFF8C42) : THEME_PRIMARY_COLOR;
        case WX_GRAPH_HUM_CLOUDS: return series ? lv_color_hex(0x9AAACD) : lv_color_hex(0x4FA8FF);
        case WX_GRAPH_PRESSURE:   return series ? lv_color_hex(0x6EE7D8) : lv_color_hex(0x9D7BFF);
        case WX_GRAPH_WIND:       return series ? lv_color_hex(0xFFD23F) : lv_color_hex(0x4FA8FF);
        case WX_GRAPH_PRECIP:     return series ? lv_color_hex(0xE3F2FF) : lv_color_hex(0x4FA8FF);
        default:                  return series ? THEME_TEXT_SECONDARY : THEME_PRIMARY_COLOR;
    }
}

static bool wx_graph_sample_value(const weather_history_sample_t *sample,
                                  wx_graph_mode_t mode, uint8_t series,
                                  int32_t *out)
{
    if (!sample || !out) return false;
    switch (mode) {
        case WX_GRAPH_TEMP:
            *out = series ? sample->feels_f : sample->temp_f;
            return true;
        case WX_GRAPH_HUM_CLOUDS:
            *out = series ? sample->clouds_pct : sample->humidity_pct;
            return true;
        case WX_GRAPH_PRESSURE:
            if (series) {
                if (!sample->grnd_level_hpa) return false;
                *out = wx_hpa_to_inhg_x100(sample->grnd_level_hpa);
            } else {
                uint16_t pressure = sample->sea_level_hpa ? sample->sea_level_hpa : sample->pressure_hpa;
                if (!pressure) return false;
                *out = wx_hpa_to_inhg_x100(pressure);
            }
            return true;
        case WX_GRAPH_WIND:
            *out = (int32_t)((series ? sample->wind_gust_mph_x10 : sample->wind_mph_x10) + 5u) / 10;
            return true;
        case WX_GRAPH_PRECIP:
            *out = wx_mm_x10_to_in_x100(series ? sample->snow_1h_mm_x10 : sample->rain_1h_mm_x10);
            return true;
        default:
            return false;
    }
}

static void wx_graph_format_range(char *out, size_t out_len, wx_graph_mode_t mode,
                                  int32_t min_value, int32_t max_value)
{
    if (!out || out_len == 0) return;
    switch (mode) {
        case WX_GRAPH_TEMP:
            snprintf(out, out_len, "%ld° to %ld°", (long)min_value, (long)max_value);
            break;
        case WX_GRAPH_HUM_CLOUDS:
            snprintf(out, out_len, "%ld%% to %ld%%", (long)min_value, (long)max_value);
            break;
        case WX_GRAPH_PRESSURE:
            {
                char min_text[16];
                char max_text[16];
                wx_format_x100(min_text, sizeof(min_text), min_value, NULL);
                wx_format_x100(max_text, sizeof(max_text), max_value, NULL);
                snprintf(out, out_len, "%s to %s inHg", min_text, max_text);
            }
            break;
        case WX_GRAPH_WIND:
            snprintf(out, out_len, "%ld to %ld mph", (long)min_value, (long)max_value);
            break;
        case WX_GRAPH_PRECIP:
            {
                char min_text[16];
                char max_text[16];
                wx_format_x100(min_text, sizeof(min_text), min_value, NULL);
                wx_format_x100(max_text, sizeof(max_text), max_value, NULL);
                snprintf(out, out_len, "%s to %s in", min_text, max_text);
            }
            break;
        default:
            snprintf(out, out_len, "%ld to %ld", (long)min_value, (long)max_value);
            break;
    }
}

static void wx_history_axis_format(char *out, size_t out_len, wx_graph_mode_t mode, int32_t value)
{
    if (!out || out_len == 0) return;
    switch (mode) {
        case WX_GRAPH_TEMP:
            snprintf(out, out_len, "%ld°", (long)value);
            break;
        case WX_GRAPH_HUM_CLOUDS:
            snprintf(out, out_len, "%ld%%", (long)value);
            break;
        case WX_GRAPH_PRESSURE:
            wx_format_x100(out, out_len, value, NULL);
            break;
        case WX_GRAPH_WIND:
            snprintf(out, out_len, "%ld", (long)value);
            break;
        case WX_GRAPH_PRECIP:
            wx_format_x100(out, out_len, value, NULL);
            break;
        default:
            snprintf(out, out_len, "%ld", (long)value);
            break;
    }
}

static void wx_history_graph_view_set_chart(wx_history_graph_view_t *view,
                                            wx_graph_mode_t mode,
                                            int32_t axis_min,
                                            int32_t axis_max)
{
    if (!view || mode >= WX_GRAPH_COUNT || !view->chart[mode] ||
        !view->fill_series[mode][0] || !view->fill_series[mode][1] ||
        !view->series[mode][0] || !view->series[mode][1]) return;
    wx_history_chart_values_t *values = wx_history_values_get();
    if (!values) return;
    lv_chart_set_axis_range(view->chart[mode], LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
    lv_chart_set_axis_range(view->chart[mode], LV_CHART_AXIS_SECONDARY_Y, axis_min, axis_max);
    lv_chart_set_series_values(view->chart[mode], view->fill_series[mode][0],
                               values[mode][WX_HISTORY_SERIES_FILL_A], WX_HISTORY_CHART_POINTS);
    lv_chart_set_series_values(view->chart[mode], view->fill_series[mode][1],
                               values[mode][WX_HISTORY_SERIES_FILL_B], WX_HISTORY_CHART_POINTS);
    lv_chart_set_series_values(view->chart[mode], view->series[mode][0],
                               values[mode][WX_HISTORY_SERIES_REAL_A], WX_HISTORY_CHART_POINTS);
    lv_chart_set_series_values(view->chart[mode], view->series[mode][1],
                               values[mode][WX_HISTORY_SERIES_REAL_B], WX_HISTORY_CHART_POINTS);
    lv_chart_refresh(view->chart[mode]);
}

static void wx_history_fill_missing_line(wx_history_chart_values_t *values,
                                         uint8_t real_series,
                                         uint8_t fill_series,
                                         const bool present[WX_HISTORY_CHART_POINTS])
{
    if (!values || !present || real_series >= WX_HISTORY_SERIES_COUNT || fill_series >= WX_HISTORY_SERIES_COUNT) return;
    int prev = -1;
    for (uint8_t i = 0; i < WX_HISTORY_CHART_POINTS; i++) {
        if (!present[i]) continue;
        if (prev >= 0) {
            uint8_t gap = (uint8_t)(i - (uint8_t)prev);
            if (gap > 1) {
                int32_t from = (*values)[real_series][prev];
                int32_t to = (*values)[real_series][i];
                int32_t delta = to - from;
                for (uint8_t slot = (uint8_t)prev; slot <= i; slot++) {
                    uint8_t offset = (uint8_t)(slot - (uint8_t)prev);
                    (*values)[fill_series][slot] = from + (int32_t)((int64_t)delta * offset / gap);
                }
            }
        }
        prev = i;
    }
}

static void wx_history_graph_view_update_labels(wx_history_graph_view_t *view,
                                                wx_graph_mode_t mode,
                                                uint32_t window_start_utc,
                                                uint32_t window_end_utc,
                                                int32_t tz_offset_s,
                                                bool have_values,
                                                int32_t axis_min,
                                                int32_t axis_max,
                                                bool have_a,
                                                int32_t min_a,
                                                int32_t max_a,
                                                bool have_b,
                                                int32_t min_b,
                                                int32_t max_b)
{
    if (!view || mode >= WX_GRAPH_COUNT) return;

    char range[36];
    char text[64];
    if (have_a) {
        wx_graph_format_range(range, sizeof(range), mode, min_a, max_a);
        snprintf(text, sizeof(text), "%s %s", wx_graph_series_label(mode, 0), range);
    } else {
        snprintf(text, sizeof(text), "%s —", wx_graph_series_label(mode, 0));
    }
    label_set_text_if_changed(view->data_a[mode], text);

    if (have_b) {
        wx_graph_format_range(range, sizeof(range), mode, min_b, max_b);
        snprintf(text, sizeof(text), "%s %s", wx_graph_series_label(mode, 1), range);
    } else {
        snprintf(text, sizeof(text), "%s —", wx_graph_series_label(mode, 1));
    }
    label_set_text_if_changed(view->data_b[mode], text);

    if (have_values) {
        wx_history_axis_format(text, sizeof(text), mode, axis_max);
        label_set_text_if_changed(view->axis_hi[mode], text);
        wx_history_axis_format(text, sizeof(text), mode, axis_min);
        label_set_text_if_changed(view->axis_lo[mode], text);
    } else {
        label_set_text_if_changed(view->axis_hi[mode], "—");
        label_set_text_if_changed(view->axis_lo[mode], "—");
    }

    if (window_start_utc && window_end_utc && window_end_utc >= window_start_utc) {
        char hm[16];
        uint32_t span_s = window_end_utc - window_start_utc;
        for (uint8_t i = 0; i < WX_GRAPH_TIME_LABELS; i++) {
            uint32_t label_utc = window_start_utc + (uint32_t)(((uint64_t)span_s * i) / (WX_GRAPH_TIME_LABELS - 1u));
            format_local_hm(hm, sizeof(hm), label_utc, tz_offset_s);
            label_set_text_if_changed(view->time_label[mode][i], hm);
        }
    } else {
        for (uint8_t i = 0; i < WX_GRAPH_TIME_LABELS; i++) {
            label_set_text_if_changed(view->time_label[mode][i], "--:--");
        }
    }
}

static void wx_history_graph_view_clear(wx_history_graph_view_t *view, const char *status)
{
    if (!view) return;
    wx_history_chart_values_t *values = wx_history_values_get();
    if (!values) return;
    for (int mode = 0; mode < WX_GRAPH_COUNT; mode++) {
        for (uint8_t series = 0; series < WX_HISTORY_SERIES_COUNT; series++) {
            for (uint8_t i = 0; i < WX_HISTORY_CHART_POINTS; i++) {
                values[mode][series][i] = LV_CHART_POINT_NONE;
            }
        }
        wx_history_graph_view_set_chart(view, (wx_graph_mode_t)mode, 0, 100);
        wx_history_graph_view_update_labels(view, (wx_graph_mode_t)mode, 0, 0, 0,
                                            false, 0, 100,
                                            false, 0, 0,
                                            false, 0, 0);
    }
    label_set_text_if_changed(view->status, status ? status : "History unavailable");
}

static void wx_history_refresh(void)
{
    weather_history_t *history = wx_history_cache_get();
    if (!history) {
        wx_history_graph_view_clear(&s_wx_history_view, "History buffer unavailable");
        return;
    }
    wx_history_chart_values_t *values = wx_history_values_get();
    if (!values) {
        wx_history_graph_view_clear(&s_wx_history_view, "History chart buffer unavailable");
        return;
    }
    if (weather_history_get(history) != ESP_OK) return;

    int32_t tz_offset_s = 0;
    weather_state_t current;
    if (weather_state_get(&current) == ESP_OK && current.valid) tz_offset_s = current.tz_offset_s;

    if (!history->count) {
        wx_history_graph_view_clear(&s_wx_history_view, history->storage_detail);
        return;
    }

    uint32_t latest_utc = history->samples[history->count - 1].observed_utc;
    uint32_t window_end_utc = latest_utc;
    time_t now = time(NULL);
    if (now >= (time_t)WX_HISTORY_VALID_NOW_UTC && (uint32_t)now >= latest_utc) {
        window_end_utc = (uint32_t)now;
    }
    uint32_t window_start_utc = window_end_utc > WX_HISTORY_WINDOW_SECONDS ?
                                window_end_utc - WX_HISTORY_WINDOW_SECONDS : 0;
    uint8_t window_sample_count = 0;

    for (uint8_t i = 0; i < history->count; i++) {
        const uint32_t observed = history->samples[i].observed_utc;
        if (observed >= window_start_utc && observed <= window_end_utc) window_sample_count++;
    }

    for (int mode = 0; mode < WX_GRAPH_COUNT; mode++) {
        for (uint8_t series = 0; series < WX_HISTORY_SERIES_COUNT; series++) {
            for (uint8_t i = 0; i < WX_HISTORY_CHART_POINTS; i++) {
                values[mode][series][i] = LV_CHART_POINT_NONE;
            }
        }
        bool present[2][WX_HISTORY_CHART_POINTS] = {0};

        bool have_values = false;
        bool have_a = false;
        bool have_b = false;
        int32_t min_value = 0;
        int32_t max_value = 0;
        int32_t min_a = 0;
        int32_t max_a = 0;
        int32_t min_b = 0;
        int32_t max_b = 0;

        for (uint8_t i = 0; i < history->count; i++) {
            const weather_history_sample_t *sample = &history->samples[i];
            if (sample->observed_utc < window_start_utc || sample->observed_utc > window_end_utc) continue;
            uint32_t elapsed_s = sample->observed_utc - window_start_utc;
            uint8_t slot = (uint8_t)(elapsed_s / WX_HISTORY_SLOT_SECONDS);
            if (slot >= WX_HISTORY_CHART_POINTS) slot = WX_HISTORY_CHART_POINTS - 1u;
            int32_t value = 0;
            if (wx_graph_sample_value(sample, (wx_graph_mode_t)mode, 0, &value)) {
                values[mode][WX_HISTORY_SERIES_REAL_A][slot] = value;
                present[0][slot] = true;
                if (!have_values || value < min_value) min_value = value;
                if (!have_values || value > max_value) max_value = value;
                if (!have_a || value < min_a) min_a = value;
                if (!have_a || value > max_a) max_a = value;
                have_values = true;
                have_a = true;
            }
            if (wx_graph_sample_value(sample, (wx_graph_mode_t)mode, 1, &value)) {
                values[mode][WX_HISTORY_SERIES_REAL_B][slot] = value;
                present[1][slot] = true;
                if (!have_values || value < min_value) min_value = value;
                if (!have_values || value > max_value) max_value = value;
                if (!have_b || value < min_b) min_b = value;
                if (!have_b || value > max_b) max_b = value;
                have_values = true;
                have_b = true;
            }
        }

        wx_history_fill_missing_line(&values[mode], WX_HISTORY_SERIES_REAL_A,
                                     WX_HISTORY_SERIES_FILL_A, present[0]);
        wx_history_fill_missing_line(&values[mode], WX_HISTORY_SERIES_REAL_B,
                                     WX_HISTORY_SERIES_FILL_B, present[1]);

        int32_t axis_min = 0;
        int32_t axis_max = 100;
        if (mode == WX_GRAPH_HUM_CLOUDS) {
            axis_min = 0;
            axis_max = 100;
        } else if (have_values) {
            int32_t pad = 4;
            if (mode == WX_GRAPH_PRESSURE) pad = 6;
            else if (mode == WX_GRAPH_WIND) pad = 3;
            else if (mode == WX_GRAPH_PRECIP) pad = 5;
            axis_min = min_value - pad;
            axis_max = max_value + pad;
            if (mode == WX_GRAPH_WIND || mode == WX_GRAPH_PRECIP) axis_min = 0;
            if (axis_min == axis_max) axis_max = axis_min + (mode == WX_GRAPH_PRESSURE ? 8 : 8);
        }

        wx_history_graph_view_set_chart(&s_wx_history_view, (wx_graph_mode_t)mode, axis_min, axis_max);
        wx_history_graph_view_update_labels(&s_wx_history_view, (wx_graph_mode_t)mode,
                                            window_sample_count ? window_start_utc : 0,
                                            window_sample_count ? window_end_utc : 0,
                                            tz_offset_s,
                                            have_values, axis_min, axis_max,
                                            have_a, min_a, max_a,
                                            have_b, min_b, max_b);
    }

    char start_hm[16];
    char end_hm[16];
    format_local_hm(start_hm, sizeof(start_hm), window_sample_count ? window_start_utc : 0, tz_offset_s);
    format_local_hm(end_hm, sizeof(end_hm), window_sample_count ? window_end_utc : 0, tz_offset_s);
    char status[128];
    if (window_sample_count) {
        snprintf(status, sizeof(status), "%u/%u samples in last 24h: %s-%s - %s",
                 window_sample_count, history->count, start_hm, end_hm, history->storage_detail);
    } else {
        snprintf(status, sizeof(status), "No samples in last 24h - %s", history->storage_detail);
    }
    label_set_text_if_changed(s_wx_history_view.status, status);
}

static const char *wx_forecast_graph_title(wx_forecast_graph_t mode)
{
    switch (mode) {
        case WX_FC_GRAPH_PRESSURE: return "Pressure";
        case WX_FC_GRAPH_TEMP:     return "Temperature";
        case WX_FC_GRAPH_HUMIDITY: return "Humidity";
        case WX_FC_GRAPH_PRECIP:   return "Rain / Snow";
        default:                   return "Forecast";
    }
}

static lv_color_t wx_forecast_graph_color(wx_forecast_graph_t mode)
{
    switch (mode) {
        case WX_FC_GRAPH_PRESSURE: return lv_color_hex(0x9D7BFF);
        case WX_FC_GRAPH_TEMP:     return lv_color_hex(0xFF8C42);
        case WX_FC_GRAPH_HUMIDITY: return lv_color_hex(0x4FA8FF);
        case WX_FC_GRAPH_PRECIP:   return lv_color_hex(0x6EE7D8);
        default:                   return THEME_PRIMARY_COLOR;
    }
}

static bool wx_forecast_graph_value(const weather_hour_t *hour,
                                    wx_forecast_graph_t mode,
                                    int32_t *out)
{
    if (!hour || !hour->dt_utc || !out) return false;
    switch (mode) {
        case WX_FC_GRAPH_PRESSURE:
            if (!hour->pressure_hpa) return false;
            *out = wx_hpa_to_inhg_x100(hour->pressure_hpa);
            return true;
        case WX_FC_GRAPH_TEMP:
            *out = hour->temp_f;
            return true;
        case WX_FC_GRAPH_HUMIDITY:
            *out = hour->humidity_pct;
            return true;
        case WX_FC_GRAPH_PRECIP: {
            uint32_t in_x100 = hour->precip_in_x100;
            if (!in_x100) in_x100 = (uint32_t)hour->rain_in_x100 + hour->snow_in_x100;
            if (in_x100 > INT32_MAX) in_x100 = INT32_MAX;
            *out = (int32_t)in_x100;
            return true;
        }
        default:
            return false;
    }
}

static void wx_forecast_axis_format(char *out, size_t out_len,
                                    wx_forecast_graph_t mode,
                                    int32_t value)
{
    if (!out || out_len == 0) return;
    switch (mode) {
        case WX_FC_GRAPH_PRESSURE:
            wx_format_x100(out, out_len, value, NULL);
            break;
        case WX_FC_GRAPH_TEMP:
            snprintf(out, out_len, "%ld°", (long)value);
            break;
        case WX_FC_GRAPH_HUMIDITY:
            snprintf(out, out_len, "%ld%%", (long)value);
            break;
        case WX_FC_GRAPH_PRECIP:
            wx_format_x100(out, out_len, value, NULL);
            break;
        default:
            snprintf(out, out_len, "%ld", (long)value);
            break;
    }
}

static void wx_forecast_value_format(char *out, size_t out_len,
                                     wx_forecast_graph_t mode,
                                     int32_t value)
{
    if (!out || out_len == 0) return;
    switch (mode) {
        case WX_FC_GRAPH_PRESSURE:
            wx_format_x100(out, out_len, value, "inHg");
            break;
        case WX_FC_GRAPH_TEMP:
            snprintf(out, out_len, "%ld°", (long)value);
            break;
        case WX_FC_GRAPH_HUMIDITY:
            snprintf(out, out_len, "%ld%%", (long)value);
            break;
        case WX_FC_GRAPH_PRECIP:
            wx_format_x100(out, out_len, value, "in");
            break;
        default:
            snprintf(out, out_len, "%ld", (long)value);
            break;
    }
}

static void wx_forecast_graph_view_update_labels(wx_forecast_graph_view_t *view,
                                                 wx_forecast_graph_t mode,
                                                 const weather_state_t *w,
                                                 uint8_t count,
                                                 bool have_values,
                                                 int32_t min_value,
                                                 int32_t max_value,
                                                 int32_t axis_min,
                                                 int32_t axis_max)
{
    if (!view) return;
    char text[32];
    if (have_values) {
        wx_forecast_value_format(text, sizeof(text), mode, max_value);
        char hi[40];
        snprintf(hi, sizeof(hi), "High %s", text);
        label_set_text_if_changed(view->data_hi[mode], hi);
        wx_forecast_value_format(text, sizeof(text), mode, min_value);
        char lo[40];
        snprintf(lo, sizeof(lo), "Low %s", text);
        label_set_text_if_changed(view->data_lo[mode], lo);

        wx_forecast_axis_format(text, sizeof(text), mode, axis_max);
        label_set_text_if_changed(view->axis_hi[mode], text);
        wx_forecast_axis_format(text, sizeof(text), mode, axis_min);
        label_set_text_if_changed(view->axis_lo[mode], text);
    } else {
        label_set_text_if_changed(view->data_hi[mode], "High —");
        label_set_text_if_changed(view->data_lo[mode], "Low —");
        label_set_text_if_changed(view->axis_hi[mode], "—");
        label_set_text_if_changed(view->axis_lo[mode], "—");
    }

    if (w && count > 0) {
        char hm[16];
        for (uint8_t i = 0; i < WX_GRAPH_TIME_LABELS; i++) {
            uint8_t idx = count > 1 ? (uint8_t)(((uint16_t)(count - 1u) * i) / (WX_GRAPH_TIME_LABELS - 1u)) : 0;
            format_local_hm(hm, sizeof(hm), w->hours[idx].dt_utc, w->tz_offset_s);
            label_set_text_if_changed(view->time_label[mode][i], hm);
        }
    } else {
        for (uint8_t i = 0; i < WX_GRAPH_TIME_LABELS; i++) {
            label_set_text_if_changed(view->time_label[mode][i], "--:--");
        }
    }
}

static void wx_forecast_graph_view_set_chart(wx_forecast_graph_view_t *view,
                                             wx_forecast_graph_t mode,
                                             int32_t axis_min,
                                             int32_t axis_max)
{
    if (!view || !view->chart[mode] || !view->series[mode]) return;
    wx_forecast_chart_values_t *values = wx_forecast_values_get();
    if (!values) return;
    lv_chart_set_axis_range(view->chart[mode], LV_CHART_AXIS_PRIMARY_Y, axis_min, axis_max);
    lv_chart_set_series_values(view->chart[mode], view->series[mode],
                               values[mode], WX_FORECAST_GRAPH_POINTS);
    lv_chart_refresh(view->chart[mode]);
}

static void wx_forecast_graph_view_clear(wx_forecast_graph_view_t *view, const char *status)
{
    if (!view) return;
    for (int mode = 0; mode < WX_FC_GRAPH_COUNT; mode++) {
        wx_forecast_graph_view_set_chart(view, (wx_forecast_graph_t)mode, 0, 100);
        wx_forecast_graph_view_update_labels(view, (wx_forecast_graph_t)mode, NULL, 0, false, 0, 0, 0, 100);
    }
    label_set_text_if_changed(view->status, status ? status : "Forecast graphs unavailable");
}

static void wx_forecast_graphs_clear(const char *status)
{
    wx_forecast_chart_values_t *values = wx_forecast_values_get();
    if (!values) return;
    for (int mode = 0; mode < WX_FC_GRAPH_COUNT; mode++) {
        for (uint8_t i = 0; i < WX_FORECAST_GRAPH_POINTS; i++) {
            values[mode][i] = LV_CHART_POINT_NONE;
        }
    }
    wx_forecast_graph_view_clear(&s_wx_fc_view, status);
    wx_forecast_graph_view_clear(&s_idle_fc_view, status);
}

static void wx_forecast_graphs_refresh(const weather_state_t *w)
{
    if (!w || !w->valid || !w->hour_count) {
        wx_forecast_graphs_clear("Hourly forecast unavailable");
        return;
    }
    wx_forecast_chart_values_t *values = wx_forecast_values_get();
    if (!values) {
        wx_forecast_graphs_clear("Forecast chart buffer unavailable");
        return;
    }

    for (int mode = 0; mode < WX_FC_GRAPH_COUNT; mode++) {
        bool have_values = false;
        int32_t min_value = 0;
        int32_t max_value = 0;
        for (uint8_t i = 0; i < WX_FORECAST_GRAPH_POINTS; i++) {
            values[mode][i] = LV_CHART_POINT_NONE;
        }

        uint8_t count = w->hour_count > WX_FORECAST_GRAPH_POINTS ? WX_FORECAST_GRAPH_POINTS : w->hour_count;
        for (uint8_t i = 0; i < count; i++) {
            int32_t value = 0;
            if (!wx_forecast_graph_value(&w->hours[i], (wx_forecast_graph_t)mode, &value)) continue;
            values[mode][i] = value;
            if (!have_values || value < min_value) min_value = value;
            if (!have_values || value > max_value) max_value = value;
            have_values = true;
        }

        int32_t axis_min = 0;
        int32_t axis_max = 100;
        if (mode == WX_FC_GRAPH_HUMIDITY) {
            axis_min = 0;
            axis_max = 100;
        } else if (have_values) {
            int32_t pad = 4;
            if (mode == WX_FC_GRAPH_PRESSURE) pad = 6;
            else if (mode == WX_FC_GRAPH_PRECIP) pad = 5;
            axis_min = min_value - pad;
            axis_max = max_value + pad;
            if (mode == WX_FC_GRAPH_PRECIP) axis_min = 0;
            if (axis_min == axis_max) axis_max = axis_min + (mode == WX_FC_GRAPH_PRESSURE ? 8 : 8);
        }

        wx_forecast_graph_view_set_chart(&s_wx_fc_view, (wx_forecast_graph_t)mode, axis_min, axis_max);
        wx_forecast_graph_view_set_chart(&s_idle_fc_view, (wx_forecast_graph_t)mode, axis_min, axis_max);
        wx_forecast_graph_view_update_labels(&s_wx_fc_view, (wx_forecast_graph_t)mode, w, count,
                                             have_values, min_value, max_value, axis_min, axis_max);
        wx_forecast_graph_view_update_labels(&s_idle_fc_view, (wx_forecast_graph_t)mode, w, count,
                                             have_values, min_value, max_value, axis_min, axis_max);
    }

    char start[16];
    char end[16];
    format_local_hm(start, sizeof(start), w->hours[0].dt_utc, w->tz_offset_s);
    format_local_hm(end, sizeof(end), w->hours[w->hour_count - 1].dt_utc, w->tz_offset_s);
    char status[72];
    snprintf(status, sizeof(status), "Next %u hours: %s-%s", w->hour_count, start, end);
    label_set_text_if_changed(s_wx_fc_view.status, status);
    label_set_text_if_changed(s_idle_fc_view.status, status);
}

static void weather_refresh(lv_timer_t *t)
{
    if (timer_page_hidden(t, s_weather_page_root)) return;
    weather_state_t w;
    if (weather_state_get(&w) != ESP_OK || !w.valid) {
        label_set_text_if_changed(s_wx_temp, "--°");
        label_set_text_if_changed(s_wx_feels, "Feels —°");
        label_set_text_if_changed(s_wx_hum, "Humidity —");
        label_set_text_if_changed(s_wx_wind, "Wind —");
        label_set_text_if_changed(s_wx_cond, "No data");
        label_set_text_if_changed(s_wx_desc, "");
        label_set_text_if_changed(s_wx_icon, WI_NA);
        label_set_text_if_changed(s_wx_hilo, "Hi —  Lo —");
        label_set_text_if_changed(s_wx_pressure, "—");
        label_set_text_if_changed(s_wx_visibility, "—");
        label_set_text_if_changed(s_wx_clouds, "—");
        label_set_text_if_changed(s_wx_uv, "—");
        label_set_text_if_changed(s_wx_gust, "—");
        label_set_text_if_changed(s_wx_precip, "—");
        label_set_text_if_changed(s_wx_sunrise, "—:—");
        label_set_text_if_changed(s_wx_sunset, "—:—");
        label_set_text_if_changed(s_wx_dewpoint, "—");
        label_set_text_if_changed(s_wx_daylight, "—");
        label_set_text_if_changed(s_wx_city, "");
        label_set_text_if_changed(s_wx_observed, "");
        moon_phase_labels_set(s_wx_moon_icon, s_wx_moon_label, NULL, false);
        label_set_text_if_changed(s_wx_age, "Waiting for first fetch...");
        wx_wind_visual_set(NULL);
        for (int i = 0; i < WX_FORECAST_SLOTS; i++) {
            label_set_text_if_changed(s_wx_day_name[i], "---");
            label_set_text_if_changed(s_wx_day_icon[i], WI_NA);
            label_set_text_if_changed(s_wx_day_temp[i], "—/—");
            label_set_text_if_changed(s_wx_day_pop[i], "");
            label_set_text_if_changed(s_wx_day_cond[i], "");
        }
        wx_forecast_graphs_clear("Waiting for hourly forecast");
        wx_history_refresh();
        return;
    }
    char b[64];

    snprintf(b, sizeof(b), "%ld°", (long)w.temp_f);
    label_set_text_if_changed(s_wx_temp, b);
    snprintf(b, sizeof(b), "Feels %ld°", (long)w.feels_f);
    label_set_text_if_changed(s_wx_feels, b);
    snprintf(b, sizeof(b), "Humidity %u%%", w.humidity_pct);
    label_set_text_if_changed(s_wx_hum, b);
    snprintf(b, sizeof(b), "Wind %u.%u mph %s",
             w.wind_mph_x10 / 10, w.wind_mph_x10 % 10, wind_compass(w.wind_deg));
    label_set_text_if_changed(s_wx_wind, b);
    wx_wind_visual_set(&w);
    label_set_text_if_changed(s_wx_cond, w.condition[0] ? w.condition : "—");
    label_set_text_if_changed(s_wx_desc, w.description);
    bool night = weather_is_night(&w);
    label_set_text_if_changed(s_wx_icon, weather_symbol(w.condition, w.icon, night));
    lv_obj_set_style_text_color(s_wx_icon, weather_color(w.condition), LV_PART_MAIN);

    if (w.temp_min_f || w.temp_max_f) {
        snprintf(b, sizeof(b), "Hi %ld°  Lo %ld°",
                 (long)w.temp_max_f, (long)w.temp_min_f);
    } else {
        snprintf(b, sizeof(b), "Hi —  Lo —");
    }
    label_set_text_if_changed(s_wx_hilo, b);

    if (w.pressure_hpa) {
        wx_format_pressure_inhg(b, sizeof(b), w.pressure_hpa);
    } else snprintf(b, sizeof(b), "—");
    label_set_text_if_changed(s_wx_pressure, b);

    if (w.visibility_m) {
        double mi = (double)w.visibility_m / 1609.344;
        snprintf(b, sizeof(b), "%.1f mi", mi);
    } else snprintf(b, sizeof(b), "—");
    label_set_text_if_changed(s_wx_visibility, b);

    snprintf(b, sizeof(b), "%u%%", w.clouds_pct);
    label_set_text_if_changed(s_wx_clouds, b);

    if (w.uv_index_valid) snprintf(b, sizeof(b), "%u", w.uv_index);
    else                  snprintf(b, sizeof(b), "—");
    label_set_text_if_changed(s_wx_uv, b);

    if (w.wind_gust_valid) {
        snprintf(b, sizeof(b), "%u.%u mph",
                 w.wind_gust_mph_x10 / 10, w.wind_gust_mph_x10 % 10);
    } else snprintf(b, sizeof(b), "—");
    label_set_text_if_changed(s_wx_gust, b);

    wx_format_precip_1h(b, sizeof(b), w.rain_1h_mm_x10, w.snow_1h_mm_x10);
    label_set_text_if_changed(s_wx_precip, b);

    char hm[16];
    format_local_hm(hm, sizeof(hm), w.sunrise_utc, w.tz_offset_s);
    label_set_text_if_changed(s_wx_sunrise, hm);
    format_local_hm(hm, sizeof(hm), w.sunset_utc, w.tz_offset_s);
    label_set_text_if_changed(s_wx_sunset, hm);

    snprintf(b, sizeof(b), "%ld°", (long)dew_point_f(w.temp_f, w.humidity_pct));
    label_set_text_if_changed(s_wx_dewpoint, b);

    if (w.sunrise_utc && w.sunset_utc && w.sunset_utc > w.sunrise_utc) {
        format_duration_hm(b, sizeof(b), w.sunset_utc - w.sunrise_utc);
    } else snprintf(b, sizeof(b), "—");
    label_set_text_if_changed(s_wx_daylight, b);

    if (w.city[0]) {
        if (w.country[0]) snprintf(b, sizeof(b), LV_SYMBOL_GPS " %s, %s", w.city, w.country);
        else              snprintf(b, sizeof(b), LV_SYMBOL_GPS " %s", w.city);
    } else b[0] = '\0';
    label_set_text_if_changed(s_wx_city, b);

    if (w.observed_utc) {
        format_local_hm(hm, sizeof(hm), w.observed_utc, w.tz_offset_s);
        snprintf(b, sizeof(b), LV_SYMBOL_REFRESH " %s", hm);
    } else b[0] = '\0';
    label_set_text_if_changed(s_wx_observed, b);
    moon_phase_labels_set(s_wx_moon_icon, s_wx_moon_label, &w, false);

    /* Forecast row */
    for (int i = 0; i < WX_FORECAST_SLOTS; i++) {
        if (i < w.day_count) {
            char name[8];
            format_local_weekday(name, sizeof(name), w.days[i].dt_utc, w.tz_offset_s);
            label_set_text_if_changed(s_wx_day_name[i], name);
            /* Forecast days are always rendered in daytime variants. */
            label_set_text_if_changed(s_wx_day_icon[i], weather_symbol(w.days[i].condition,
                                                                       w.days[i].icon, false));
            lv_obj_set_style_text_color(s_wx_day_icon[i],
                                        weather_color(w.days[i].condition), LV_PART_MAIN);
            snprintf(b, sizeof(b), "%d° / %d°", w.days[i].hi_f, w.days[i].lo_f);
            label_set_text_if_changed(s_wx_day_temp[i], b);
            label_set_text_if_changed(s_wx_day_cond[i], w.days[i].condition);
            if (w.days[i].pop_pct > 0) {
                snprintf(b, sizeof(b), LV_SYMBOL_DOWNLOAD " %u%%", w.days[i].pop_pct);
            } else b[0] = '\0';
            label_set_text_if_changed(s_wx_day_pop[i], b);
        } else {
            label_set_text_if_changed(s_wx_day_name[i], "---");
            label_set_text_if_changed(s_wx_day_icon[i], WI_NA);
            label_set_text_if_changed(s_wx_day_temp[i], "—/—");
            label_set_text_if_changed(s_wx_day_pop[i], "");
            label_set_text_if_changed(s_wx_day_cond[i], "");
        }
    }
    wx_forecast_graphs_refresh(&w);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t age_s  = (now_ms - w.fetched_at_ms) / 1000;
    snprintf(b, sizeof(b), "Updated %lus ago", (unsigned long)age_s);
    label_set_text_if_changed(s_wx_age, b);
    wx_history_refresh();
}

/* Build a metric tile: small colored icon + label + bold value. Returns the
 * value label so the refresh routine can update it cheaply. */
static lv_obj_t *wx_metric_tile(lv_obj_t *parent, const char *icon,
                                 const char *name, const char *initial,
                                 lv_color_t accent)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 200, 64);
    lv_obj_set_style_bg_color(tile, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(tile, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(tile, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tile, 10, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(tile);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, THEME_FONT_WX_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, accent, LV_PART_MAIN);

    lv_obj_t *txt_col = lv_obj_create(tile);
    lv_obj_remove_style_all(txt_col);
    lv_obj_set_size(txt_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(txt_col, 1);
    lv_obj_clear_flag(txt_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(txt_col);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_MUTED, LV_PART_MAIN);

    lv_obj_t *val = lv_label_create(txt_col);
    lv_label_set_text(val, initial);
    lv_obj_set_style_text_font(val, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    return val;
}

/* Section header for a card. */
static void wx_card_header(lv_obj_t *parent, const char *icon, const char *text,
                            lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, accent, LV_PART_MAIN);
    lv_obj_set_style_text_font(ic, THEME_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(t, THEME_FONT_LABEL, LV_PART_MAIN);
}

static void wx_daily_forecast_card_create(lv_obj_t *parent)
{
    lv_obj_t *fc_card = deep_card(parent);
    lv_obj_set_size(fc_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fc_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(fc_card, 8, LV_PART_MAIN);
    wx_card_header(fc_card, LV_SYMBOL_REFRESH, "DAILY FORECAST", THEME_PRIMARY_COLOR);

    lv_obj_t *days = lv_obj_create(fc_card);
    lv_obj_remove_style_all(days);
    lv_obj_set_size(days, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(days, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(days, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < WX_FORECAST_SLOTS; i++) {
        lv_obj_t *day = lv_obj_create(days);
        lv_obj_remove_style_all(day);
        lv_obj_set_size(day, 120, 148);
        lv_obj_set_style_bg_color(day, THEME_SURFACE_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(day, theme_surface_opa(), LV_PART_MAIN);
        lv_obj_set_style_radius(day, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(day, 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(day, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(day, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(day, 3, LV_PART_MAIN);
        lv_obj_clear_flag(day, LV_OBJ_FLAG_SCROLLABLE);

        s_wx_day_name[i] = lv_label_create(day);
        lv_label_set_text(s_wx_day_name[i], "---");
        lv_obj_set_style_text_color(s_wx_day_name[i], THEME_TEXT_SECONDARY, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_wx_day_name[i], THEME_FONT_LABEL, LV_PART_MAIN);

        s_wx_day_icon[i] = lv_label_create(day);
        lv_label_set_text(s_wx_day_icon[i], WI_NA);
        lv_obj_set_style_text_color(s_wx_day_icon[i], THEME_PRIMARY_COLOR, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_wx_day_icon[i], THEME_FONT_WX_SMALL, LV_PART_MAIN);

        s_wx_day_temp[i] = lv_label_create(day);
        lv_label_set_text(s_wx_day_temp[i], "—/—");
        lv_obj_set_width(s_wx_day_temp[i], LV_PCT(100));
        lv_label_set_long_mode(s_wx_day_temp[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_wx_day_temp[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_wx_day_temp[i], THEME_TEXT_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_wx_day_temp[i], THEME_FONT_BODY, LV_PART_MAIN);

        s_wx_day_cond[i] = lv_label_create(day);
        lv_label_set_text(s_wx_day_cond[i], "");
        lv_obj_set_width(s_wx_day_cond[i], LV_PCT(100));
        lv_label_set_long_mode(s_wx_day_cond[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_wx_day_cond[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_wx_day_cond[i], THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_wx_day_cond[i], THEME_FONT_SMALL, LV_PART_MAIN);

        s_wx_day_pop[i] = lv_label_create(day);
        lv_label_set_text(s_wx_day_pop[i], "");
        lv_obj_set_width(s_wx_day_pop[i], LV_PCT(100));
        lv_label_set_long_mode(s_wx_day_pop[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_wx_day_pop[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_wx_day_pop[i], lv_color_hex(0x4FA8FF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_wx_day_pop[i], THEME_FONT_SMALL, LV_PART_MAIN);
    }

    s_wx_age = lv_label_create(fc_card);
    lv_label_set_text(s_wx_age, "Waiting for first fetch...");
    lv_obj_set_width(s_wx_age, LV_PCT(100));
    lv_obj_set_style_text_align(s_wx_age, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_age, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_age, THEME_TEXT_MUTED, LV_PART_MAIN);
}

static void wx_forecast_graph_slide_create(lv_obj_t *carousel,
                                           wx_forecast_graph_view_t *view,
                                           wx_forecast_graph_t mode,
                                           lv_coord_t slide_h,
                                           lv_coord_t chart_h)
{
    lv_obj_t *slide = lv_obj_create(carousel);
    lv_obj_remove_style_all(slide);
    lv_obj_set_size(slide, LV_PCT(100), slide_h);
    lv_obj_set_flex_flow(slide, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(slide, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slide, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(slide, 6, LV_PART_MAIN);
    lv_obj_clear_flag(slide, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(slide);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 10, LV_PART_MAIN);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, wx_forecast_graph_title(mode));
    lv_obj_set_width(title, 1);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, wx_forecast_graph_color(mode), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_BODY, LV_PART_MAIN);

    view->data_hi[mode] = lv_label_create(top);
    lv_label_set_text(view->data_hi[mode], "High —");
    lv_obj_set_width(view->data_hi[mode], 120);
    lv_label_set_long_mode(view->data_hi[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->data_hi[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->data_hi[mode], THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->data_hi[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    view->data_lo[mode] = lv_label_create(top);
    lv_label_set_text(view->data_lo[mode], "Low —");
    lv_obj_set_width(view->data_lo[mode], 116);
    lv_label_set_long_mode(view->data_lo[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->data_lo[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->data_lo[mode], THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->data_lo[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *graph_row = lv_obj_create(slide);
    lv_obj_remove_style_all(graph_row);
    lv_obj_set_size(graph_row, LV_PCT(100), chart_h);
    lv_obj_set_flex_flow(graph_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(graph_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(graph_row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(graph_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *chart = lv_chart_create(graph_row);
    lv_obj_set_size(chart, 1, chart_h);
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(chart, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(chart, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(chart, 4, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 5, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, WX_FORECAST_GRAPH_POINTS);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    view->series[mode] = lv_chart_add_series(chart, wx_forecast_graph_color(mode), LV_CHART_AXIS_PRIMARY_Y);
    view->chart[mode] = chart;

    lv_obj_t *axis = lv_obj_create(graph_row);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, 44, LV_PCT(100));
    lv_obj_set_flex_flow(axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_SCROLLABLE);

    view->axis_hi[mode] = lv_label_create(axis);
    lv_label_set_text(view->axis_hi[mode], "—");
    lv_obj_set_width(view->axis_hi[mode], LV_PCT(100));
    lv_label_set_long_mode(view->axis_hi[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->axis_hi[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->axis_hi[mode], THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->axis_hi[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    view->axis_lo[mode] = lv_label_create(axis);
    lv_label_set_text(view->axis_lo[mode], "—");
    lv_obj_set_width(view->axis_lo[mode], LV_PCT(100));
    lv_label_set_long_mode(view->axis_lo[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->axis_lo[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->axis_lo[mode], THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->axis_lo[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *time_row = lv_obj_create(slide);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_right(time_row, 52, LV_PART_MAIN);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < WX_GRAPH_TIME_LABELS; i++) {
        view->time_label[mode][i] = lv_label_create(time_row);
        lv_label_set_text(view->time_label[mode][i], "--:--");
        lv_obj_set_width(view->time_label[mode][i], 72);
        lv_label_set_long_mode(view->time_label[mode][i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(view->time_label[mode][i],
                                    i == 0 ? LV_TEXT_ALIGN_LEFT :
                                    i == WX_GRAPH_TIME_LABELS - 1 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(view->time_label[mode][i], THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(view->time_label[mode][i], THEME_FONT_SMALL, LV_PART_MAIN);
    }
}

static lv_obj_t *wx_forecast_graphs_carousel_create(lv_obj_t *parent,
                                                    wx_forecast_graph_view_t *view,
                                                    lv_coord_t carousel_h,
                                                    lv_coord_t chart_h)
{
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));

    lv_obj_t *carousel = lv_obj_create(parent);
    lv_obj_remove_style_all(carousel);
    lv_obj_set_size(carousel, LV_PCT(100), carousel_h);
    lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(carousel, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(carousel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ONE);
    wx_carousel_attach_manual_pause(carousel);
    view->carousel = carousel;

    lv_coord_t slide_h = carousel_h - 2;
    for (int mode = 0; mode < WX_FC_GRAPH_COUNT; mode++) {
        wx_forecast_graph_slide_create(carousel, view, (wx_forecast_graph_t)mode, slide_h, chart_h);
    }
    return carousel;
}

static void wx_forecast_graphs_status_create(lv_obj_t *parent, wx_forecast_graph_view_t *view)
{
    if (!view) return;
    view->status = lv_label_create(parent);
    lv_label_set_text(view->status, "Waiting for hourly forecast");
    lv_obj_set_width(view->status, LV_PCT(100));
    lv_label_set_long_mode(view->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->status, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->status, THEME_FONT_SMALL, LV_PART_MAIN);
}

static void wx_forecast_graphs_card_create(lv_obj_t *parent)
{
    lv_obj_t *graph_card = deep_card(parent);
    lv_obj_set_size(graph_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(graph_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(graph_card, 8, LV_PART_MAIN);
    wx_card_header(graph_card, LV_SYMBOL_LIST, "24-HOUR FORECAST GRAPHS", THEME_PRIMARY_COLOR);
    wx_forecast_graphs_carousel_create(graph_card, &s_wx_fc_view, 230, 162);
    wx_forecast_graphs_status_create(graph_card, &s_wx_fc_view);
}

static void wx_history_graph_slide_create(lv_obj_t *carousel,
                                          wx_history_graph_view_t *view,
                                          wx_graph_mode_t mode,
                                          lv_coord_t slide_h,
                                          lv_coord_t chart_h)
{
    lv_obj_t *slide = lv_obj_create(carousel);
    lv_obj_remove_style_all(slide);
    lv_obj_set_size(slide, LV_PCT(100), slide_h);
    lv_obj_set_flex_flow(slide, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(slide, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slide, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(slide, 6, LV_PART_MAIN);
    lv_obj_clear_flag(slide, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(slide);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 10, LV_PART_MAIN);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, wx_graph_title(mode));
    lv_obj_set_width(title, 1);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, wx_graph_series_color(mode, 0), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_BODY, LV_PART_MAIN);

    view->data_a[mode] = lv_label_create(top);
    lv_label_set_text(view->data_a[mode], "A —");
    lv_obj_set_width(view->data_a[mode], 176);
    lv_label_set_long_mode(view->data_a[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->data_a[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->data_a[mode], wx_graph_series_color(mode, 0), LV_PART_MAIN);
    lv_obj_set_style_text_font(view->data_a[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    view->data_b[mode] = lv_label_create(top);
    lv_label_set_text(view->data_b[mode], "B —");
    lv_obj_set_width(view->data_b[mode], 176);
    lv_label_set_long_mode(view->data_b[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->data_b[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->data_b[mode], wx_graph_series_color(mode, 1), LV_PART_MAIN);
    lv_obj_set_style_text_font(view->data_b[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *graph_row = lv_obj_create(slide);
    lv_obj_remove_style_all(graph_row);
    lv_obj_set_size(graph_row, LV_PCT(100), chart_h);
    lv_obj_set_flex_flow(graph_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(graph_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(graph_row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(graph_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *chart = lv_chart_create(graph_row);
    lv_obj_set_size(chart, 1, chart_h);
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(chart, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_top(chart, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(chart, 4, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 5, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, WX_HISTORY_CHART_POINTS);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    view->fill_series[mode][0] = lv_chart_add_series(chart, THEME_TEXT_MUTED, LV_CHART_AXIS_PRIMARY_Y);
    view->fill_series[mode][1] = lv_chart_add_series(chart, THEME_TEXT_MUTED, LV_CHART_AXIS_PRIMARY_Y);
    view->series[mode][0] = lv_chart_add_series(chart, wx_graph_series_color(mode, 0), LV_CHART_AXIS_PRIMARY_Y);
    view->series[mode][1] = lv_chart_add_series(chart, wx_graph_series_color(mode, 1), LV_CHART_AXIS_PRIMARY_Y);
    view->chart[mode] = chart;

    lv_obj_t *axis = lv_obj_create(graph_row);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, 44, LV_PCT(100));
    lv_obj_set_flex_flow(axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(axis, LV_OBJ_FLAG_SCROLLABLE);

    view->axis_hi[mode] = lv_label_create(axis);
    lv_label_set_text(view->axis_hi[mode], "—");
    lv_obj_set_width(view->axis_hi[mode], LV_PCT(100));
    lv_label_set_long_mode(view->axis_hi[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->axis_hi[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->axis_hi[mode], THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->axis_hi[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    view->axis_lo[mode] = lv_label_create(axis);
    lv_label_set_text(view->axis_lo[mode], "—");
    lv_obj_set_width(view->axis_lo[mode], LV_PCT(100));
    lv_label_set_long_mode(view->axis_lo[mode], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->axis_lo[mode], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(view->axis_lo[mode], THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(view->axis_lo[mode], THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *time_row = lv_obj_create(slide);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_right(time_row, 52, LV_PART_MAIN);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < WX_GRAPH_TIME_LABELS; i++) {
        view->time_label[mode][i] = lv_label_create(time_row);
        lv_label_set_text(view->time_label[mode][i], "--:--");
        lv_obj_set_width(view->time_label[mode][i], 72);
        lv_label_set_long_mode(view->time_label[mode][i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(view->time_label[mode][i],
                                    i == 0 ? LV_TEXT_ALIGN_LEFT :
                                    i == WX_GRAPH_TIME_LABELS - 1 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(view->time_label[mode][i], THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(view->time_label[mode][i], THEME_FONT_SMALL, LV_PART_MAIN);
    }
}

static lv_obj_t *wx_history_graphs_carousel_create(lv_obj_t *parent,
                                                   wx_history_graph_view_t *view,
                                                   lv_coord_t carousel_h,
                                                   lv_coord_t chart_h)
{
    if (!view) return NULL;
    memset(view, 0, sizeof(*view));

    lv_obj_t *carousel = lv_obj_create(parent);
    lv_obj_remove_style_all(carousel);
    lv_obj_set_size(carousel, LV_PCT(100), carousel_h);
    lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(carousel, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(carousel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ONE);
    wx_carousel_attach_manual_pause(carousel);
    view->carousel = carousel;

    lv_coord_t slide_h = carousel_h - 2;
    for (int mode = 0; mode < WX_GRAPH_COUNT; mode++) {
        wx_history_graph_slide_create(carousel, view, (wx_graph_mode_t)mode, slide_h, chart_h);
    }
    return carousel;
}

static void pull_refresh_weather(void)
{
    esp_err_t err = weather_api_request_refresh();
    if (s_wx_age) {
        label_set_text_if_changed(s_wx_age, err == ESP_OK ? "Fetching weather..." : esp_err_to_name(err));
    }
}

static bool weather_activate_needs_fetch(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    weather_state_t w;
    if (weather_state_get(&w) != ESP_OK || !w.valid) return true;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return now_ms - w.fetched_at_ms > (uint32_t)cfg.weather_tab_stale_min * 60u * 1000u;
}

static void weather_activate_status(esp_err_t err)
{
    if (!s_wx_age) return;
    if (err != ESP_OK) {
        label_set_text_if_changed(s_wx_age, esp_err_to_name(err));
        return;
    }

    services_status_t st;
    if (services_status_get(&st) == ESP_OK) {
        if (!st.weather_configured) {
            label_set_text_if_changed(s_wx_age, "Weather not configured");
            return;
        }
        if (!st.wifi_connected) {
            label_set_text_if_changed(s_wx_age, "Waiting for Wi-Fi");
            return;
        }
    }
    label_set_text_if_changed(s_wx_age, "Fetching weather...");
}

void screen_weather_activate(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    weather_refresh(NULL);
    if (!weather_activate_needs_fetch()) return;

    if (!rate_limit_ms(&s_weather_activate_refresh_ms, (uint32_t)cfg.weather_tab_wake_s * 1000u)) {
        weather_activate_status(ESP_OK);
        return;
    }
    weather_activate_status(weather_api_request_refresh());
}

lv_obj_t *screen_weather_create(lv_obj_t *parent)
{
    lv_obj_t *p = page_root(parent);
    s_weather_page_root = p;
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_row(p, 10, LV_PART_MAIN);

    /* ===== Header card: location + observed time ===== */
    lv_obj_t *header = deep_card(p);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(header, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_wx_city = lv_label_create(header);
    lv_label_set_text(s_wx_city, "");
    lv_obj_set_width(s_wx_city, 1);
    lv_obj_set_flex_grow(s_wx_city, 1);
    lv_label_set_long_mode(s_wx_city, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_wx_city, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_city, THEME_FONT_TITLE, LV_PART_MAIN);

    lv_obj_t *header_meta = lv_obj_create(header);
    lv_obj_remove_style_all(header_meta);
    lv_obj_set_size(header_meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header_meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_meta, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header_meta, 10, LV_PART_MAIN);
    lv_obj_clear_flag(header_meta, LV_OBJ_FLAG_SCROLLABLE);

    s_wx_moon_icon = lv_label_create(header_meta);
    lv_label_set_text(s_wx_moon_icon, WI_NA);
    lv_obj_set_style_text_color(s_wx_moon_icon, lv_color_hex(0xD9E2FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_moon_icon, THEME_FONT_WX_MEDIUM, LV_PART_MAIN);

    s_wx_moon_label = lv_label_create(header_meta);
    lv_label_set_text(s_wx_moon_label, "Moon —");
    lv_obj_set_width(s_wx_moon_label, 170);
    lv_label_set_long_mode(s_wx_moon_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_wx_moon_label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_moon_label, THEME_FONT_BODY, LV_PART_MAIN);

    s_wx_observed = lv_label_create(header_meta);
    lv_label_set_text(s_wx_observed, "");
    lv_obj_set_width(s_wx_observed, 104);
    lv_label_set_long_mode(s_wx_observed, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wx_observed, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_observed, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_observed, THEME_FONT_LABEL, LV_PART_MAIN);

    /* ===== Hero card: icon | current temp | wind compass ===== */
    lv_obj_t *hero = deep_card(p);
    lv_obj_set_size(hero, LV_PCT(100), 214);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(hero, 20, LV_PART_MAIN);

    /* Left column: native weather icon + condition text */
    lv_obj_t *icon_col = lv_obj_create(hero);
    lv_obj_remove_style_all(icon_col);
    lv_obj_set_size(icon_col, 190, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icon_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(icon_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(icon_col, 6, LV_PART_MAIN);
    lv_obj_clear_flag(icon_col, LV_OBJ_FLAG_SCROLLABLE);

    s_wx_icon = lv_label_create(icon_col);
    lv_label_set_text(s_wx_icon, WI_NA);
    lv_obj_set_style_text_font(s_wx_icon, THEME_FONT_WX_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_icon, THEME_PRIMARY_COLOR, LV_PART_MAIN);

    s_wx_cond = lv_label_create(icon_col);
    lv_label_set_text(s_wx_cond, "No data");
    lv_obj_set_width(s_wx_cond, 190);
    lv_label_set_long_mode(s_wx_cond, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wx_cond, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_cond, THEME_FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_cond, THEME_TEXT_PRIMARY, LV_PART_MAIN);

    s_wx_desc = lv_label_create(icon_col);
    lv_label_set_text(s_wx_desc, "");
    lv_obj_set_width(s_wx_desc, 190);
    lv_label_set_long_mode(s_wx_desc, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wx_desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_desc, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_desc, THEME_TEXT_MUTED, LV_PART_MAIN);

    /* Right column: giant temp + feels + hi/lo + summary */
    lv_obj_t *temp_col = lv_obj_create(hero);
    lv_obj_remove_style_all(temp_col);
    lv_obj_set_size(temp_col, 226, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temp_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(temp_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(temp_col, 2, LV_PART_MAIN);
    lv_obj_clear_flag(temp_col, LV_OBJ_FLAG_SCROLLABLE);

    s_wx_temp = lv_label_create(temp_col);
    lv_label_set_text(s_wx_temp, "--°");
    lv_obj_set_style_text_font(s_wx_temp, THEME_FONT_WX_TEMP, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_temp, THEME_TEXT_PRIMARY, LV_PART_MAIN);

    s_wx_feels = lv_label_create(temp_col);
    lv_label_set_text(s_wx_feels, "Feels —°");
    lv_obj_set_style_text_font(s_wx_feels, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_feels, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_wx_feels, 28, LV_PART_MAIN);

    s_wx_hilo = lv_label_create(temp_col);
    lv_label_set_text(s_wx_hilo, "Hi —  Lo —");
    lv_obj_set_style_text_font(s_wx_hilo, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_hilo, THEME_TEXT_MUTED, LV_PART_MAIN);

    s_wx_hum = lv_label_create(temp_col);
    lv_label_set_text(s_wx_hum, "Humidity —");
    lv_obj_set_style_text_font(s_wx_hum, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_hum, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    s_wx_wind = lv_label_create(temp_col);
    lv_label_set_text(s_wx_wind, "Wind —");
    lv_obj_set_style_text_font(s_wx_wind, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wx_wind, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    lv_obj_t *wind_col = lv_obj_create(hero);
    lv_obj_remove_style_all(wind_col);
    lv_obj_set_size(wind_col, 170, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wind_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wind_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wind_col, 6, LV_PART_MAIN);
    lv_obj_clear_flag(wind_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *compass = lv_obj_create(wind_col);
    lv_obj_remove_style_all(compass);
    lv_obj_set_size(compass, 124, 124);
    lv_obj_set_style_radius(compass, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(compass, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(compass, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(compass, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(compass, 2, LV_PART_MAIN);
    lv_obj_clear_flag(compass, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *north = lv_label_create(compass);
    lv_label_set_text(north, "N");
    lv_obj_set_style_text_font(north, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(north, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_align(north, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *east = lv_label_create(compass);
    lv_label_set_text(east, "E");
    lv_obj_set_style_text_font(east, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(east, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_align(east, LV_ALIGN_RIGHT_MID, -6, 0);

    lv_obj_t *south = lv_label_create(compass);
    lv_label_set_text(south, "S");
    lv_obj_set_style_text_font(south, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(south, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_align(south, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *west = lv_label_create(compass);
    lv_label_set_text(west, "W");
    lv_obj_set_style_text_font(west, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(west, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_align(west, LV_ALIGN_LEFT_MID, 6, 0);

    s_wx_wind_points[0].x = 62;
    s_wx_wind_points[0].y = 62;
    s_wx_wind_points[1].x = 62;
    s_wx_wind_points[1].y = 14;
    s_wx_wind_needle = lv_line_create(compass);
    lv_obj_set_size(s_wx_wind_needle, 124, 124);
    lv_line_set_points_mutable(s_wx_wind_needle, s_wx_wind_points, 2);
    lv_obj_set_style_line_color(s_wx_wind_needle, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_wx_wind_needle, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_wx_wind_needle, true, LV_PART_MAIN);
    lv_obj_align(s_wx_wind_needle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_wx_wind_head_points[0].x = 54;
    s_wx_wind_head_points[0].y = 29;
    s_wx_wind_head_points[1].x = 62;
    s_wx_wind_head_points[1].y = 14;
    s_wx_wind_head_points[2].x = 70;
    s_wx_wind_head_points[2].y = 29;
    s_wx_wind_arrow_head = lv_line_create(compass);
    lv_obj_set_size(s_wx_wind_arrow_head, 124, 124);
    lv_line_set_points_mutable(s_wx_wind_arrow_head, s_wx_wind_head_points, 3);
    lv_obj_set_style_line_color(s_wx_wind_arrow_head, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_wx_wind_arrow_head, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_wx_wind_arrow_head, true, LV_PART_MAIN);
    lv_obj_align(s_wx_wind_arrow_head, LV_ALIGN_TOP_LEFT, 0, 0);

    s_wx_wind_compass = lv_label_create(wind_col);
    lv_label_set_text(s_wx_wind_compass, "--");
    lv_obj_set_style_text_color(s_wx_wind_compass, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_wind_compass, THEME_FONT_TITLE, LV_PART_MAIN);

    s_wx_wind_degree = lv_label_create(wind_col);
    lv_label_set_text(s_wx_wind_degree, "No wind data");
    lv_obj_set_style_text_color(s_wx_wind_degree, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wx_wind_degree, THEME_FONT_SMALL, LV_PART_MAIN);

    wx_daily_forecast_card_create(p);
    wx_forecast_graphs_card_create(p);

    /* ===== Details card: 3-col tile grid ===== */
    lv_obj_t *details = deep_card(p);
    lv_obj_set_size(details, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(details, 6, LV_PART_MAIN);
    wx_card_header(details, LV_SYMBOL_LIST, "DETAILS", THEME_PRIMARY_COLOR);

    lv_obj_t *grid = lv_obj_create(details);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 8, LV_PART_MAIN);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    s_wx_pressure   = wx_metric_tile(grid, WI_BAROMETER,    "Pressure",   "—", lv_color_hex(0x9D7BFF));
    s_wx_visibility = wx_metric_tile(grid, WI_EYE,          "Visibility", "—", lv_color_hex(0x4FA8FF));
    s_wx_clouds     = wx_metric_tile(grid, WI_CLOUD,        "Clouds",     "—", lv_color_hex(0x9AAACD));
    s_wx_uv         = wx_metric_tile(grid, WI_HOT,          "UV Index",   "—", lv_color_hex(0xFFD23F));
    s_wx_gust       = wx_metric_tile(grid, WI_CLOUD_UP,     "Gust",       "—", lv_color_hex(0x4FA8FF));
    s_wx_precip     = wx_metric_tile(grid, WI_RAINDROPS,    "Precip 1h",  "—", lv_color_hex(0x4FA8FF));
    s_wx_sunrise    = wx_metric_tile(grid, WI_SUNRISE,      "Sunrise",    "—:—", lv_color_hex(0xFFD23F));
    s_wx_sunset     = wx_metric_tile(grid, WI_SUNSET,       "Sunset",     "—:—", lv_color_hex(0xFF8C42));
    s_wx_dewpoint   = wx_metric_tile(grid, WI_HOT,          "Dew point",  "—", lv_color_hex(0x6EE7D8));
    s_wx_daylight   = wx_metric_tile(grid, WI_DAY_SUNNY,    "Daylight",   "—", lv_color_hex(0xFFD23F));

    /* ===== History card ===== */
    lv_obj_t *history_card = deep_card(p);
    lv_obj_set_size(history_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(history_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(history_card, 8, LV_PART_MAIN);
    wx_card_header(history_card, LV_SYMBOL_LIST, "SAVED HISTORY", THEME_PRIMARY_COLOR);
    wx_history_graphs_carousel_create(history_card, &s_wx_history_view, 230, 162);

    s_weather_refresh_timer = lv_timer_create(weather_refresh, weather_page_update_period_ms(), NULL);
    if (!s_wx_fc_cycle_timer) s_wx_fc_cycle_timer = lv_timer_create(weather_graph_cycle_cb, weather_graph_cycle_period_ms(), NULL);
    else weather_graph_cycle_timer_apply();
    weather_refresh(NULL);

    /* Pull-to-refresh: force an immediate weather data fetch */
    pull_refresh_enable(p, pull_refresh_weather);
    return p;
}

/* ============================================================
 * SYSTEM INFO PANEL
 * ============================================================ */

static lv_obj_t *s_info_uptime;
static lv_obj_t *s_info_heap;
static lv_obj_t *s_info_cpu;
static lv_obj_t *s_info_internal_ram;
static lv_obj_t *s_info_psram;
static lv_obj_t *s_info_wifi;
static lv_obj_t *s_info_ip;
static lv_obj_t *s_info_area;
static lv_obj_t *s_info_timezone;
static lv_obj_t *s_info_time;
static lv_obj_t *s_info_rs485;
static lv_obj_t *s_info_wled;
static lv_obj_t *s_info_weather;
static lv_obj_t *s_info_audio;
static lv_obj_t *s_info_panel_root;

#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
#define INFO_CPU_TASK_MAX 48
static TaskStatus_t *s_info_cpu_tasks;
static configRUN_TIME_COUNTER_TYPE s_info_cpu_prev_total;
static configRUN_TIME_COUNTER_TYPE s_info_cpu_prev_idle;
static bool s_info_cpu_have_prev;

static TaskStatus_t *info_cpu_tasks_get(void)
{
    if (s_info_cpu_tasks) return s_info_cpu_tasks;
    s_info_cpu_tasks = heap_caps_calloc(INFO_CPU_TASK_MAX, sizeof(*s_info_cpu_tasks),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_info_cpu_tasks;
}
#endif

static void info_row(lv_obj_t *parent, const char *k, const char *v,
                     lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kl = lv_label_create(row);
    lv_label_set_text(kl, k);
    lv_obj_set_style_text_color(kl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(kl, THEME_FONT_BODY, LV_PART_MAIN);

    lv_obj_t *vl = lv_label_create(row);
    lv_label_set_text(vl, v);
    lv_obj_set_style_text_color(vl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(vl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_width(vl, 330);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    if (out_val) *out_val = vl;
}

static uint8_t heap_used_pct(size_t free_bytes, size_t total_bytes)
{
    if (!total_bytes || free_bytes >= total_bytes) return 0;
    size_t used = total_bytes - free_bytes;
    return (uint8_t)((used * 100u + total_bytes / 2u) / total_bytes);
}

static void format_heap_pct(char *out, size_t out_len, uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps);
    size_t free_bytes = heap_caps_get_free_size(caps);
    if (!out || out_len == 0) return;
    if (!total) {
        snprintf(out, out_len, "n/a");
        return;
    }
    snprintf(out, out_len, "%u%% used  %u/%u KB free",
             heap_used_pct(free_bytes, total),
             (unsigned)(free_bytes / 1024u),
             (unsigned)(total / 1024u));
}

static void info_cpu_refresh(const app_tuning_config_t *cfg)
{
    if (!s_info_cpu) return;
    if (!cfg || !cfg->system_cpu_load_enabled) {
        label_set_text_if_changed(s_info_cpu, "Off");
        return;
    }
#if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
    TaskStatus_t *tasks = info_cpu_tasks_get();
    if (!tasks) {
        label_set_text_if_changed(s_info_cpu, "n/a");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t count = uxTaskGetSystemState(tasks, INFO_CPU_TASK_MAX, &total);
    if (count == 0 || count >= INFO_CPU_TASK_MAX || total == 0) {
        label_set_text_if_changed(s_info_cpu, "n/a");
        return;
    }

    configRUN_TIME_COUNTER_TYPE idle_total = 0;
    for (UBaseType_t i = 0; i < count; i++) {
        const char *name = tasks[i].pcTaskName;
        if (name && strncmp(name, "IDLE", 4) == 0) idle_total += tasks[i].ulRunTimeCounter;
    }

    if (!s_info_cpu_have_prev || total <= s_info_cpu_prev_total || idle_total < s_info_cpu_prev_idle) {
        s_info_cpu_have_prev = true;
        s_info_cpu_prev_total = total;
        s_info_cpu_prev_idle = idle_total;
        label_set_text_if_changed(s_info_cpu, "Sampling");
        return;
    }

    configRUN_TIME_COUNTER_TYPE total_delta = total - s_info_cpu_prev_total;
    configRUN_TIME_COUNTER_TYPE idle_delta = idle_total - s_info_cpu_prev_idle;
    s_info_cpu_prev_total = total;
    s_info_cpu_prev_idle = idle_total;
    if (!total_delta) {
        label_set_text_if_changed(s_info_cpu, "n/a");
        return;
    }
    uint32_t idle_pct = (uint32_t)((idle_delta * 100u + total_delta / 2u) / total_delta);
    if (idle_pct > 100u) idle_pct = 100u;
    char b[20];
    snprintf(b, sizeof(b), "%u%%", (unsigned)(100u - idle_pct));
    label_set_text_if_changed(s_info_cpu, b);
#else
    label_set_text_if_changed(s_info_cpu, "Build disabled");
#endif
}

static void info_refresh(lv_timer_t *t)
{
    if (timer_page_hidden(t, s_info_panel_root)) return;
    if (s_info_uptime) {
        uint64_t us = esp_timer_get_time();
        uint32_t s = us / 1000000ULL;
        char b[24];
        snprintf(b, sizeof(b), "%lud %02lu:%02lu:%02lu",
                 (unsigned long)(s / 86400),
                 (unsigned long)((s / 3600) % 24),
                 (unsigned long)((s / 60) % 60),
                 (unsigned long)(s % 60));
            label_set_text_if_changed(s_info_uptime, b);
    }
    if (s_info_heap) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t free_int   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        char b[40];
        snprintf(b, sizeof(b), "%u KB / %u KB",
                 (unsigned)(free_int / 1024),
                 (unsigned)(free_psram / 1024));
        label_set_text_if_changed(s_info_heap, b);
    }
    if (s_info_internal_ram) {
        char b[48];
        format_heap_pct(b, sizeof(b), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        label_set_text_if_changed(s_info_internal_ram, b);
    }
    if (s_info_psram) {
        char b[48];
        format_heap_pct(b, sizeof(b), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        label_set_text_if_changed(s_info_psram, b);
    }

    app_tuning_config_t cfg = load_tuning_config();
    info_cpu_refresh(&cfg);

    services_status_t st;
    services_status_get(&st);
    if (s_info_wifi) {
        if (st.wifi_connected) label_set_text_if_changed(s_info_wifi, st.wifi_ssid[0] ? st.wifi_ssid : "Connected");
        else if (!st.wifi_supported) label_set_text_if_changed(s_info_wifi, "Driver disabled");
        else if (st.wifi_configured) label_set_text_if_changed(s_info_wifi, "Connecting");
        else label_set_text_if_changed(s_info_wifi, "Not configured");
    }
    label_set_text_if_changed(s_info_ip, st.ip_addr[0] ? st.ip_addr : "-");
    label_set_text_if_changed(s_info_area, st.location_ready ? st.location_area : "Locating");
    label_set_text_if_changed(s_info_timezone, st.location_ready ? st.timezone : "UTC");
    if (s_info_time) {
        if (st.time_synced) label_set_text_if_changed(s_info_time, "Synced");
        else if (st.time_sync_started) label_set_text_if_changed(s_info_time, st.time_detail[0] ? st.time_detail : "Syncing");
        else label_set_text_if_changed(s_info_time, "Not started");
    }
    label_set_text_if_changed(s_info_rs485, st.rs485_ready ? "Ready" : "Offline");
    if (s_info_wled) {
        if (st.wled_online) {
            wled_state_t ws;
            wled_state_get(&ws);
            if (ws.valid && ws.version[0]) {
                char wled_info[48];
                snprintf(wled_info, sizeof(wled_info), "v%s  %u LEDs", ws.version, ws.led_count);
                label_set_text_if_changed(s_info_wled, wled_info);
            } else {
                label_set_text_if_changed(s_info_wled, "Online");
            }
        } else {
            label_set_text_if_changed(s_info_wled, "No response");
        }
    }
    if (s_info_weather) {
        if (st.weather_online) label_set_text_if_changed(s_info_weather, "Fresh");
        else label_set_text_if_changed(s_info_weather, st.weather_detail[0] ? st.weather_detail : "Unavailable");
    }
    if (s_info_audio) {
        if (st.sound_sync_ready) label_set_text_if_changed(s_info_audio, "Streaming");
        else if (st.audio_ready || st.fft_ready) label_set_text_if_changed(s_info_audio, "Preparing");
        else label_set_text_if_changed(s_info_audio, "Disabled");
    }
}

static void info_panel_create(lv_obj_t *parent, lv_obj_t *visibility_root)
{
    s_info_panel_root = visibility_root;

    lv_obj_t *card = deep_card(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);

    const esp_app_desc_t *app = esp_app_get_description();
    char buf[64];

    info_row(card, "Firmware", app ? app->version : "?", NULL);
    info_row(card, "Built", __DATE__ " " __TIME__, NULL);
    snprintf(buf, sizeof(buf), "ESP-IDF %s", IDF_VER);
    info_row(card, "Framework", buf, NULL);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(buf, sizeof(buf), "Unavailable");
    }
    info_row(card, "Device MAC", buf, NULL);

    info_row(card, "WiFi", "Not configured", &s_info_wifi);
    info_row(card, "IP", "-", &s_info_ip);
    info_row(card, "Area", "Locating", &s_info_area);
    info_row(card, "Timezone", "UTC", &s_info_timezone);
    info_row(card, "Time", "Not started", &s_info_time);
    info_row(card, "RS-485", "Offline", &s_info_rs485);
    info_row(card, "WLED", "No response", &s_info_wled);
    info_row(card, "Weather", "Not configured", &s_info_weather);
    info_row(card, "Audio sync", "Disabled", &s_info_audio);

    info_row(card, "CPU Load", "Off", &s_info_cpu);
    info_row(card, "Internal RAM", "-", &s_info_internal_ram);
    info_row(card, "PSRAM", "-", &s_info_psram);
    info_row(card, "Uptime",  "0d 00:00:00",  &s_info_uptime);
    info_row(card, "Free heap (int/psram)", "-",  &s_info_heap);

    lv_timer_create(info_refresh, 1000, NULL);
    info_refresh(NULL);
}

/* ============================================================
 * SETTINGS PAGE  (editable rows with − / + steppers)
 * ============================================================ */

typedef struct {
    int     step;
    int     vmin;
    int     vmax;
    char    suffix[8];
    void  (*apply)(int v);
    int   (*read)(void);
    lv_obj_t *value_label;
} stepper_ctx_t;

#define SETTINGS_STEPPER_MAX 40

static stepper_ctx_t s_steppers[SETTINGS_STEPPER_MAX];
static int           s_stepper_count;

static void format_stepper(char *buf, size_t n, const stepper_ctx_t *c, int v)
{
    snprintf(buf, n, "%d%s", v, c->suffix);
}

static void stepper_update(stepper_ctx_t *c)
{
    char b[24];
    format_stepper(b, sizeof(b), c, c->read());
    lv_label_set_text(c->value_label, b);
}

static void stepper_btn_cb(lv_event_t *e)
{
    stepper_ctx_t *c = (stepper_ctx_t *)lv_event_get_user_data(e);
    /* The button has a flag in its own user_data: +1 or -1 via dsc */
    lv_obj_t *btn = lv_event_get_target(e);
    int dir = (int)(intptr_t)lv_obj_get_user_data(btn);
    int v = c->read() + dir * c->step;
    if (v < c->vmin) v = c->vmin;
    if (v > c->vmax) v = c->vmax;
    c->apply(v);
    stepper_update(c);
}

static void settings_hide_keyboard(void);
static void settings_status_refresh(lv_timer_t *t);
static esp_err_t theme_save_controls(bool refresh_background);
static void idle_bottom_panel_reset(void);

/* Settings-row callbacks – tiny shims so setter signatures match */
static int  rd_kmin(void)   { led_state_t s; led_state_get(&s); return s.kelvin_min; }
static int  rd_kmax(void)   { led_state_t s; led_state_get(&s); return s.kelvin_max; }
static int  rd_to(void)     { led_state_t s; led_state_get(&s); return s.screen_timeout_s; }
static int  rd_disp(void)   { led_state_t s; led_state_get(&s); return s.display_brightness_pct; }
static void ap_kmin(int v)  { led_state_set_kelvin_min((uint16_t)v); }
static void ap_kmax(int v)  { led_state_set_kelvin_max((uint16_t)v); }
static void ap_to(int v)    { led_state_set_screen_timeout((uint16_t)v); }
static void ap_disp(int v)  { led_state_set_display_brightness((uint8_t)v); }

static esp_err_t save_tuning_config(app_tuning_config_t *cfg)
{
    esp_err_t err = app_config_tuning_save(cfg);
    if (err != ESP_OK) toast_show(esp_err_to_name(err));
    else settings_status_refresh(NULL);
    return err;
}

static void tuning_weather_changed(void)
{
    (void)weather_api_request_refresh();
}

static void tuning_weather_ui_changed(void)
{
    weather_refresh_timer_apply();
}

static void tuning_auto_hold_changed(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    (void)backlight_manager_set_manual_hold_minutes(cfg.auto_brightness_hold_min);
}

static void tuning_backlight_changed(void)
{
    (void)backlight_manager_apply_tuning();
}

static void tuning_idle_changed(void)
{
    (void)idle_manager_apply_tuning();
}

static void tuning_status_changed(void)
{
    status_bar_apply_tuning();
}

static void tuning_time_sync_changed(void)
{
    (void)services_time_sync_apply_tuning();
}

#define DEFINE_TUNING_ACCESSORS(name, field, after_save) \
static int rd_##name(void) { app_tuning_config_t cfg = load_tuning_config(); return cfg.field; } \
static void ap_##name(int v) { \
    app_tuning_config_t cfg = load_tuning_config(); \
    cfg.field = v; \
    if (save_tuning_config(&cfg) == ESP_OK) { after_save; } \
}

DEFINE_TUNING_ACCESSORS(wx_refresh, weather_refresh_min, tuning_weather_changed())
DEFINE_TUNING_ACCESSORS(wx_retry, weather_retry_s, tuning_weather_changed())
DEFINE_TUNING_ACCESSORS(wx_wifi_wait, weather_wifi_wait_s, tuning_weather_changed())
DEFINE_TUNING_ACCESSORS(wx_http_timeout, weather_http_timeout_s, tuning_weather_changed())
DEFINE_TUNING_ACCESSORS(wx_forecast_gap, weather_forecast_gap_s, tuning_weather_changed())
DEFINE_TUNING_ACCESSORS(wx_tab_stale, weather_tab_stale_min, tuning_weather_ui_changed())
DEFINE_TUNING_ACCESSORS(wx_tab_wake, weather_tab_wake_s, tuning_weather_ui_changed())
DEFINE_TUNING_ACCESSORS(wx_page_update, weather_page_update_s, tuning_weather_ui_changed())
DEFINE_TUNING_ACCESSORS(wx_graph_cycle, weather_graph_cycle_s, weather_graph_cycle_timer_apply())
DEFINE_TUNING_ACCESSORS(wx_bottom_cycle, weather_bottom_panel_cycle_s, idle_bottom_panel_reset())
DEFINE_TUNING_ACCESSORS(wled_poll, wled_poll_s, (void)0)
DEFINE_TUNING_ACCESSORS(wled_stale, wled_stale_s, (void)0)
DEFINE_TUNING_ACCESSORS(wled_hue_update, wled_hue_update_hz, lights_tuning_refresh())
DEFINE_TUNING_ACCESSORS(light_safety_hours, light_safety_auto_off_hours, led_state_safety_check_now())
DEFINE_TUNING_ACCESSORS(auto_min, auto_brightness_min_pct, tuning_backlight_changed())
DEFINE_TUNING_ACCESSORS(auto_max, auto_brightness_max_pct, tuning_backlight_changed())
DEFINE_TUNING_ACCESSORS(auto_eval, auto_brightness_eval_s, tuning_backlight_changed())
DEFINE_TUNING_ACCESSORS(auto_ramp, auto_brightness_ramp_s, tuning_backlight_changed())
DEFINE_TUNING_ACCESSORS(auto_hold, auto_brightness_hold_min, tuning_auto_hold_changed())
DEFINE_TUNING_ACCESSORS(low_warn, low_brightness_warn_pct, tuning_status_changed())
DEFINE_TUNING_ACCESSORS(idle_check, idle_check_s, tuning_idle_changed())
DEFINE_TUNING_ACCESSORS(idle_wake_timer_min, idle_dismiss_lights_timer_min, tuning_idle_changed())
DEFINE_TUNING_ACCESSORS(idle_swipe_dismiss_min, idle_swipe_dismiss_min, tuning_idle_changed())
DEFINE_TUNING_ACCESSORS(status_update, status_bar_update_s, tuning_status_changed())
DEFINE_TUNING_ACCESSORS(toast_duration, toast_duration_ms, (void)0)
DEFINE_TUNING_ACCESSORS(time_sync_interval, time_sync_interval_min, tuning_time_sync_changed())
DEFINE_TUNING_ACCESSORS(time_sync_hour, time_sync_hour, tuning_time_sync_changed())

static void idle_dismiss_lights_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_tuning_config_t cfg = load_tuning_config();
    cfg.idle_dismiss_lights_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (save_tuning_config(&cfg) == ESP_OK) tuning_idle_changed();
}

static void light_safety_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_tuning_config_t cfg = load_tuning_config();
    cfg.light_safety_auto_off_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (save_tuning_config(&cfg) == ESP_OK) led_state_safety_check_now();
}

static void idle_wake_timer_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_tuning_config_t cfg = load_tuning_config();
    cfg.idle_dismiss_lights_timer_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (save_tuning_config(&cfg) == ESP_OK) tuning_idle_changed();
}

static void idle_swipe_wake_lights_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_tuning_config_t cfg = load_tuning_config();
    cfg.idle_swipe_wake_lights_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (save_tuning_config(&cfg) == ESP_OK) tuning_idle_changed();
}

static void weather_bottom_mode_changed(lv_event_t *e)
{
    lv_obj_t *dropdown = lv_event_get_target(e);
    app_tuning_config_t cfg = load_tuning_config();
    cfg.weather_bottom_panel_mode = (uint8_t)lv_dropdown_get_selected(dropdown);
    if (save_tuning_config(&cfg) == ESP_OK) idle_bottom_panel_reset();
}

static void auto_brightness_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t err = backlight_manager_set_enabled(enabled);
    toast_show(err == ESP_OK ? (enabled ? "Auto-brightness ON" : "Auto-brightness OFF") : esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static stepper_ctx_t *add_stepper_row(lv_obj_t *parent, const char *icon,
                                      const char *label,
                                      int step, int vmin, int vmax,
                                      const char *suffix,
                                      void (*apply)(int), int (*read)(void))
{
    if (s_stepper_count >= SETTINGS_STEPPER_MAX) return NULL;
    stepper_ctx_t *c = &s_steppers[s_stepper_count++];
    c->step = step; c->vmin = vmin; c->vmax = vmax;
    strncpy(c->suffix, suffix ? suffix : "", sizeof(c->suffix) - 1);
    c->apply = apply; c->read = read;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(ic, THEME_FONT_LARGE, LV_PART_MAIN);

    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_color(lb, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_flex_grow(lb, 1);

    /* − button */
    lv_obj_t *minus = lv_button_create(row);
    lv_obj_set_size(minus, 48, 48);
    lv_obj_set_style_radius(minus, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(minus, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(minus, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(minus, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(minus, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(minus, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(minus, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(minus, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(minus, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(minus, 0, LV_PART_MAIN);
    lv_obj_set_user_data(minus, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(minus, stepper_btn_cb, LV_EVENT_CLICKED, c);
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(ml, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(ml, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(ml);

    /* value */
    c->value_label = lv_label_create(row);
    lv_label_set_text(c->value_label, "");
    lv_obj_set_width(c->value_label, 130);
    lv_obj_set_style_text_align(c->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(c->value_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(c->value_label, THEME_FONT_TITLE, LV_PART_MAIN);

    /* + button */
    lv_obj_t *plus = lv_button_create(row);
    lv_obj_set_size(plus, 48, 48);
    lv_obj_set_style_radius(plus, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(plus, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(plus, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(plus, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(plus, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(plus, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(plus, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(plus, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(plus, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(plus, 0, LV_PART_MAIN);
    lv_obj_set_user_data(plus, (void *)(intptr_t)+1);
    lv_obj_add_event_cb(plus, stepper_btn_cb, LV_EVENT_CLICKED, c);
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(pl, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(pl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(pl);

    stepper_update(c);
    return c;
}

static lv_obj_t *s_settings_keyboard;
static lv_obj_t *s_settings_root;
static lv_obj_t *s_settings_home;
static lv_obj_t *s_settings_floating_header;
static lv_obj_t *s_settings_floating_title;
static lv_obj_t *s_settings_lights_page;
static lv_obj_t *s_settings_wifi_page;
static lv_obj_t *s_settings_wled_page;
static lv_obj_t *s_settings_weather_page;
static lv_obj_t *s_settings_theme_page;
static lv_obj_t *s_settings_sd_page;
static lv_obj_t *s_settings_display_page;
static lv_obj_t *s_settings_idle_page;
static lv_obj_t *s_settings_audio_page;
static lv_obj_t *s_settings_timer_page;
static lv_obj_t *s_settings_system_page;
static lv_obj_t *s_settings_about_page;
static lv_obj_t *s_settings_display_status;
static lv_obj_t *s_light_safety_switch;
static lv_obj_t *s_auto_brightness_switch;
static lv_obj_t *s_settings_idle_status;
static lv_obj_t *s_home_wifi_summary;
static lv_obj_t *s_home_wled_summary;
static lv_obj_t *s_home_weather_summary;
static lv_obj_t *s_home_theme_summary;
static lv_obj_t *s_home_sd_summary;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_sd_list;
static lv_obj_t *s_sd_path_label;
static lv_obj_t *s_wifi_password_overlay;
static lv_obj_t *s_wifi_password_ta;
static lv_obj_t *s_wifi_password_keyboard;
static lv_obj_t *s_weather_key_ta;
static lv_obj_t *s_weather_location_label;
static lv_obj_t *s_weather_bottom_mode_dropdown;
static lv_obj_t *s_system_cpu_load_switch;
static lv_obj_t *s_idle_dismiss_lights_switch;
static lv_obj_t *s_idle_swipe_wake_lights_switch;
static lv_obj_t *s_idle_wake_timer_switch;
static lv_obj_t *s_theme_bg_dropdown;
static lv_obj_t *s_theme_background_enabled_switch;
static lv_obj_t *s_theme_idle_only_switch;
static lv_obj_t *s_theme_opacity_slider;
static lv_obj_t *s_theme_shadows_sw;
static lv_obj_t *s_theme_dim_slider;
static lv_obj_t *s_theme_slideshow_slider;
static lv_obj_t *s_theme_slideshow_play_label;
static lv_obj_t *s_theme_opacity_label;
static lv_obj_t *s_theme_dim_label;
static lv_obj_t *s_theme_slideshow_label;
static lv_obj_t *s_theme_download_row;
static lv_obj_t *s_theme_download_btn;
static lv_obj_t *s_theme_progress_panel;
static lv_obj_t *s_theme_progress_label;
static lv_obj_t *s_theme_progress_bar;
static lv_obj_t *s_settings_wifi_status;
static lv_obj_t *s_settings_weather_status;
static lv_obj_t *s_settings_wled_status;
static lv_obj_t *s_settings_theme_status;
static lv_obj_t *s_settings_sd_status;
static lv_obj_t *s_settings_audio_status;
static lv_obj_t *s_wifi_scan_status;
static lv_obj_t *s_wifi_scan_btn;
static TaskHandle_t s_wifi_scan_task;
static bool s_wifi_scan_in_progress;
static bool s_wifi_pull_armed;
static char s_wifi_selected_ssid[33];
static bool s_wifi_selected_secure;

#define THEME_COLOR_CHOICES 6
#define THEME_COLOR_WHEEL_SIZE 156
#define THEME_COLOR_WHEEL_RADIUS ((THEME_COLOR_WHEEL_SIZE / 2) - 3)
#define THEME_COLOR_WHEEL_CENTER (THEME_COLOR_WHEEL_SIZE / 2)
#define THEME_COLOR_WHEEL_BUF_SIZE LV_CANVAS_BUF_SIZE(THEME_COLOR_WHEEL_SIZE, THEME_COLOR_WHEEL_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN)
#define THEME_COLOR_MEMORY_COUNT 4
#define THEME_COLOR_MEMORY_DIR BSP_SD_MOUNT_POINT "/theme"
#define THEME_COLOR_MEMORY_PATH THEME_COLOR_MEMORY_DIR "/recent_colors.txt"
#define THEME_CONTROL_PANEL_HEX 0x0B1020
#define THEME_CONTROL_SURFACE_HEX 0x111A2E
#define THEME_CONTROL_BORDER_HEX 0x33405F
#define THEME_CONTROL_PRIMARY_HEX 0x2F8CFF
#define THEME_CONTROL_TEXT_HEX 0xFFFFFF
#define THEME_CONTROL_MUTED_HEX 0xA9B5D6
#define THEME_READABLE_PRIMARY_DARK_HEX 0x10131A
#define THEME_READABLE_SECONDARY_DARK_HEX 0x3E475A
#define THEME_READABLE_MUTED_DARK_HEX 0x697386
#define THEME_DEG_TO_RAD 0.01745329252f
#define THEME_RAD_TO_DEG 57.295779513f

typedef enum {
    THEME_COLOR_BACKGROUND,
    THEME_COLOR_SURFACE,
    THEME_COLOR_CARD,
    THEME_COLOR_BORDER,
    THEME_COLOR_PRIMARY,
    THEME_COLOR_ACCENT,
    THEME_COLOR_COUNT,
} theme_color_role_t;

typedef struct {
    const char *label;
    uint32_t choices[THEME_COLOR_CHOICES];
} theme_color_role_def_t;

static const theme_color_role_def_t s_theme_color_roles[THEME_COLOR_COUNT] = {
    [THEME_COLOR_BACKGROUND] = {"Background", {0x05070D, 0x071015, 0x120B18, 0x08120B, 0x130F08, 0x0D0D10}},
    [THEME_COLOR_SURFACE]    = {"Fields",     {0x111625, 0x12231F, 0x221426, 0x1E1B12, 0x121E2A, 0x191B1E}},
    [THEME_COLOR_CARD]       = {"Boxes",      {0x171D31, 0x14251F, 0x27172B, 0x28200F, 0x102333, 0x202124}},
    [THEME_COLOR_BORDER]     = {"Outline",    {0x303A59, 0x2D6A5F, 0x7B4FA3, 0x8E6A32, 0x4C79A6, 0x5D6678}},
    [THEME_COLOR_PRIMARY]    = {"Primary",    {0x1E86FF, 0x00BFA6, 0xFFB020, 0xE85D75, 0xA855F7, 0x7DD3FC}},
    [THEME_COLOR_ACCENT]     = {"Accent",     {0xFF4B00, 0x16C784, 0xF43F5E, 0xFACC15, 0x38BDF8, 0xC084FC}},
};

static lv_obj_t *s_theme_swatches[THEME_COLOR_COUNT][THEME_COLOR_CHOICES];
static lv_obj_t *s_theme_custom_swatches[THEME_COLOR_COUNT];
static lv_obj_t *s_theme_custom_labels[THEME_COLOR_COUNT];
static lv_obj_t *s_theme_color_labels[THEME_COLOR_COUNT];
static lv_obj_t *s_theme_palette_card;
static lv_obj_t *s_theme_palette_title;
static uint8_t s_theme_selected[THEME_COLOR_COUNT];
static uint32_t s_theme_custom_hex[THEME_COLOR_COUNT];
static bool s_theme_custom_set[THEME_COLOR_COUNT];
static bool s_theme_using_custom[THEME_COLOR_COUNT];

static lv_obj_t *s_theme_picker_overlay;
static lv_obj_t *s_theme_picker_canvas;
static lv_obj_t *s_theme_picker_marker;
static lv_obj_t *s_theme_picker_preview;
static lv_obj_t *s_theme_picker_hex_label;
static lv_obj_t *s_theme_picker_value_slider;
static lv_obj_t *s_theme_picker_value_label;
static lv_obj_t *s_theme_picker_memory_swatches[THEME_COLOR_MEMORY_COUNT];
static lv_obj_t *s_theme_picker_memory_labels[THEME_COLOR_MEMORY_COUNT];
static uint8_t *s_theme_picker_buf;
static theme_color_role_t s_theme_picker_role;
static uint8_t s_theme_picker_restore_selected;
static uint32_t s_theme_picker_restore_custom_hex;
static bool s_theme_picker_restore_custom_set;
static bool s_theme_picker_restore_using_custom;
static uint16_t s_theme_picker_hue;
static uint8_t s_theme_picker_sat;
static uint8_t s_theme_picker_value;
static uint32_t s_theme_color_memory[THEME_COLOR_MEMORY_COUNT];
static bool s_theme_color_memory_valid[THEME_COLOR_MEMORY_COUNT];
static bool s_theme_color_memory_loaded;
static bool s_theme_color_memory_dirty;

/* Preset table is now in ui_background.c; accessed via bg_preset_*() */

static void system_cpu_load_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_tuning_config_t cfg = load_tuning_config();
    cfg.system_cpu_load_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (save_tuning_config(&cfg) == ESP_OK) info_refresh(NULL);
}

#define SETTINGS_WIFI_SCAN_MAX 12
static wifi_scan_result_t s_wifi_scan_results[SETTINGS_WIFI_SCAN_MAX];
static size_t s_wifi_scan_count;

static void settings_show_keyboard(lv_obj_t *ta)
{
    if (!s_settings_keyboard || !ta) return;
    lv_keyboard_mode_t mode = (lv_keyboard_mode_t)(intptr_t)lv_obj_get_user_data(ta);
    lv_keyboard_set_textarea(s_settings_keyboard, ta);
    lv_keyboard_set_mode(s_settings_keyboard, mode);
    lv_obj_clear_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_keyboard);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void settings_hide_keyboard(void)
{
    if (!s_settings_keyboard) return;
    lv_keyboard_set_textarea(s_settings_keyboard, NULL);
    lv_obj_add_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void settings_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED) {
        settings_show_keyboard(ta);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        settings_hide_keyboard();
        lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    }
}

static lv_obj_t *settings_panel_create(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 16, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void settings_show_panel(lv_obj_t *panel)
{
    settings_hide_keyboard();
    if (s_wifi_password_overlay) {
        lv_obj_delete(s_wifi_password_overlay);
        s_wifi_password_overlay = NULL;
        s_wifi_password_ta = NULL;
    }

    lv_obj_t *panels[] = {
        s_settings_home,
        s_settings_lights_page,
        s_settings_wifi_page,
        s_settings_wled_page,
        s_settings_weather_page,
        s_settings_theme_page,
        s_settings_sd_page,
        s_settings_display_page,
        s_settings_idle_page,
        s_settings_audio_page,
        s_settings_timer_page,
        s_settings_system_page,
        s_settings_about_page,
    };
    for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (!panels[i]) continue;
        if (panels[i] == panel) lv_obj_clear_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_settings_floating_header) {
        if (panel && panel != s_settings_home) {
            const char *title = (const char *)lv_obj_get_user_data(panel);
            label_set_text_if_changed(s_settings_floating_title, title && title[0] ? title : "Settings");
            lv_obj_clear_flag(s_settings_floating_header, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_settings_floating_header);
        } else {
            lv_obj_add_flag(s_settings_floating_header, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_settings_root) lv_obj_scroll_to_y(s_settings_root, 0, LV_ANIM_OFF);
}

static void settings_back_clicked(lv_event_t *e)
{
    (void)e;
    settings_show_panel(s_settings_home);
}

void screen_settings_show_home(void)
{
    settings_show_panel(s_settings_home);
}

static void settings_floating_header_create(lv_obj_t *parent)
{
    s_settings_floating_header = lv_obj_create(parent);
    lv_obj_remove_style_all(s_settings_floating_header);
    theme_style_glass_panel(s_settings_floating_header, 18);
    lv_obj_set_size(s_settings_floating_header, LV_PCT(100), 72);
    lv_obj_set_style_pad_hor(s_settings_floating_header, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_settings_floating_header, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_settings_floating_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_settings_floating_header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_settings_floating_header, 12, LV_PART_MAIN);
    lv_obj_add_flag(s_settings_floating_header, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_settings_floating_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_settings_floating_header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *back = lv_button_create(s_settings_floating_header);
    lv_obj_set_size(back, 54, 54);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(back, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(back, settings_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(arrow, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(arrow);

    s_settings_floating_title = lv_label_create(s_settings_floating_header);
    lv_label_set_text(s_settings_floating_title, "Settings");
    lv_obj_set_width(s_settings_floating_title, 1);
    lv_obj_set_flex_grow(s_settings_floating_title, 1);
    lv_label_set_long_mode(s_settings_floating_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_settings_floating_title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_settings_floating_title, THEME_FONT_TITLE, LV_PART_MAIN);
}

static lv_obj_t *settings_header(lv_obj_t *parent, const char *title)
{
    lv_obj_set_user_data(parent, (void *)title);
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, LV_PCT(100), 74);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return spacer;
}

static lv_obj_t *settings_section_title(lv_obj_t *parent, const char *title)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(label, 8, LV_PART_MAIN);
    return label;
}

static lv_obj_t *settings_status_label(lv_obj_t *parent)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "-");
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_LABEL, LV_PART_MAIN);
    return label;
}

static lv_obj_t *settings_text_input(lv_obj_t *parent, const char *label,
                                     const char *placeholder, uint32_t max_len,
                                     bool password, const char *accepted_chars,
                                     lv_keyboard_mode_t kb_mode)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lb = lv_label_create(col);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_color(lb, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(lb, THEME_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *ta = lv_textarea_create(col);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder ? placeholder : "");
    lv_textarea_set_password_mode(ta, password);
    if (accepted_chars) lv_textarea_set_accepted_chars(ta, accepted_chars);
    lv_obj_set_size(ta, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(ta, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(ta, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(ta, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ta, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ta, 10, LV_PART_MAIN);
    lv_obj_set_user_data(ta, (void *)(intptr_t)kb_mode);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, settings_ta_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return ta;
}

static lv_obj_t *settings_button(lv_obj_t *parent, const char *label,
                                 bool primary, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 54);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, primary ? 0 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, primary ? THEME_PRIMARY_COLOR : THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, label);
    lv_obj_set_style_text_font(txt, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, primary ? lv_color_black() : THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(txt);
    return btn;
}

static lv_obj_t *settings_button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 4, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *settings_menu_row(lv_obj_t *parent, const char *icon, const char *title,
                                   const char *subtitle, lv_obj_t **out_subtitle,
                                   lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 86);
    lv_obj_set_style_radius(row, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_width(ic, 36);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(ic, &ui_icons_28, LV_PART_MAIN);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(col);
    lv_label_set_text(ttl, title);
    lv_obj_set_style_text_color(ttl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(ttl, THEME_FONT_BODY, LV_PART_MAIN);

    lv_obj_t *sub = lv_label_create(col);
    lv_label_set_text(sub, subtitle ? subtitle : "");
    lv_obj_set_width(sub, LV_PCT(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(sub, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, THEME_FONT_SMALL, LV_PART_MAIN);
    if (out_subtitle) *out_subtitle = sub;

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, ">");
    lv_obj_set_style_text_color(arrow, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(arrow, THEME_FONT_TITLE, LV_PART_MAIN);
    return row;
}

static void settings_panel_clicked(lv_event_t *e)
{
    lv_obj_t *panel = (lv_obj_t *)lv_event_get_user_data(e);
    settings_show_panel(panel);
}

#define SD_EXPLORER_MAX_ENTRIES 48
#define SD_EXPLORER_PATH_MAX    160
#define SD_EXPLORER_NAME_MAX    64

typedef struct {
    char name[SD_EXPLORER_NAME_MAX];
    char path[SD_EXPLORER_PATH_MAX];
    bool is_dir;
    uint64_t size_bytes;
} sd_explorer_entry_t;

static sd_explorer_entry_t *s_sd_entries;
static uint8_t s_sd_entry_count;
static char s_sd_current_path[SD_EXPLORER_PATH_MAX] = BSP_SD_MOUNT_POINT;

static bool sd_explorer_entries_ensure(void)
{
    if (s_sd_entries) return true;
    s_sd_entries = heap_caps_calloc(SD_EXPLORER_MAX_ENTRIES, sizeof(s_sd_entries[0]),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_sd_entries != NULL;
}

static bool sd_explorer_is_root(void)
{
    return strcmp(s_sd_current_path, BSP_SD_MOUNT_POINT) == 0;
}

static const char *sd_explorer_display_path(void)
{
    size_t root_len = strlen(BSP_SD_MOUNT_POINT);
    if (strncmp(s_sd_current_path, BSP_SD_MOUNT_POINT, root_len) != 0) return s_sd_current_path;
    if (s_sd_current_path[root_len] == '\0') return "/";
    return s_sd_current_path + root_len;
}

static void sd_explorer_set_status(const char *text)
{
    const char *next = text && text[0] ? text : "Browse /sdcard";
    label_set_text_if_changed(s_settings_sd_status, next);
    label_set_text_if_changed(s_home_sd_summary, next);
}

static void sd_explorer_message(const char *text)
{
    if (!s_sd_list) return;
    lv_obj_clean(s_sd_list);
    lv_obj_t *label = lv_label_create(s_sd_list);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN);
}

static void sd_explorer_format_size(char *out, size_t out_len, uint64_t bytes)
{
    if (!out || out_len == 0) return;
    if (bytes < 1024ULL) {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        uint64_t tenths = (bytes * 10ULL + 512ULL) / 1024ULL;
        snprintf(out, out_len, "%llu.%llu KB",
                 (unsigned long long)(tenths / 10ULL),
                 (unsigned long long)(tenths % 10ULL));
    } else {
        uint64_t tenths = (bytes * 10ULL + (512ULL * 1024ULL)) / (1024ULL * 1024ULL);
        snprintf(out, out_len, "%llu.%llu MB",
                 (unsigned long long)(tenths / 10ULL),
                 (unsigned long long)(tenths % 10ULL));
    }
}

static int sd_explorer_entry_compare(const void *a, const void *b)
{
    const sd_explorer_entry_t *ea = (const sd_explorer_entry_t *)a;
    const sd_explorer_entry_t *eb = (const sd_explorer_entry_t *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcmp(ea->name, eb->name);
}

static bool sd_explorer_join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    if (!out || !dir || !name || !name[0]) return false;
    int written;
    if (strcmp(dir, "/") == 0) written = snprintf(out, out_len, "/%s", name);
    else written = snprintf(out, out_len, "%s/%s", dir, name);
    return written > 0 && (size_t)written < out_len;
}

static void sd_explorer_refresh(void);

/* ── Context menu overlay (long-press) ── */
static lv_obj_t *s_sd_context_overlay;
static sd_explorer_entry_t s_sd_context_entry;  /* snapshot of entry at long-press time */
static bool s_sd_long_press_fired;              /* suppress click after long-press */
static lv_obj_t *s_sd_rename_ta;               /* rename text area in context overlay */
static lv_obj_t *s_sd_rename_keyboard;         /* keyboard for rename */
static lv_obj_t *s_sd_newfolder_ta;            /* new folder text area */
static lv_obj_t *s_sd_newfolder_overlay;       /* new folder overlay */

static void sd_context_close(void)
{
    if (s_sd_context_overlay) {
        lv_obj_delete(s_sd_context_overlay);
        s_sd_context_overlay = NULL;
    }
    s_sd_rename_ta = NULL;
    s_sd_rename_keyboard = NULL;
    memset(&s_sd_context_entry, 0, sizeof(s_sd_context_entry));
}

static void sd_newfolder_close(void)
{
    if (s_sd_newfolder_overlay) {
        lv_obj_delete(s_sd_newfolder_overlay);
        s_sd_newfolder_overlay = NULL;
    }
    s_sd_newfolder_ta = NULL;
}

static int sd_recursive_delete(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return remove(path);

    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *ent;
    char child[SD_EXPLORER_PATH_MAX];
    int ret = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (!ent->d_name[0] || strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (sd_recursive_delete(child) != 0) ret = -1;
    }
    closedir(dir);
    if (rmdir(path) != 0) ret = -1;
    return ret;
}

/* ── Delete action ── */
static void sd_context_delete_confirmed(lv_event_t *e)
{
    (void)e;
    sd_explorer_entry_t saved = s_sd_context_entry;
    sd_context_close();
    if (!saved.path[0]) return;

    int result = sd_recursive_delete(saved.path);
    if (result == 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Deleted %s", saved.name);
        toast_show(msg);
    } else {
        char msg[80];
        snprintf(msg, sizeof(msg), "Delete failed: %s", saved.name);
        toast_show(msg);
    }
    sd_explorer_refresh();
}

static void sd_context_cancel(lv_event_t *e)
{
    (void)e;
    sd_context_close();
}

static void sd_context_delete_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_sd_context_entry.path[0]) { sd_context_close(); return; }

    /* Replace context menu contents with confirmation */
    if (!s_sd_context_overlay) return;
    lv_obj_clean(s_sd_context_overlay);

    lv_obj_t *card = lv_obj_create(s_sd_context_overlay);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 20);
    lv_obj_set_width(card, 580);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Confirm Delete");
    lv_obj_set_style_text_color(title, THEME_ERROR_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    char msg[180];
    if (s_sd_context_entry.is_dir) {
        snprintf(msg, sizeof(msg),
                 "Delete folder \"%s\" and all its contents?\nThis cannot be undone.",
                 s_sd_context_entry.name);
    } else {
        char sz[24];
        sd_explorer_format_size(sz, sizeof(sz), s_sd_context_entry.size_bytes);
        snprintf(msg, sizeof(msg),
                 "Delete file \"%s\" (%s)?\nThis cannot be undone.",
                 s_sd_context_entry.name, sz);
    }
    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, msg);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(body, THEME_FONT_BODY, LV_PART_MAIN);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, LV_PART_MAIN);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_button_create(btns);
    lv_obj_set_size(cancel_btn, 120, 44);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, sd_context_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    lv_obj_t *del_btn = lv_button_create(btns);
    lv_obj_set_size(del_btn, 120, 44);
    lv_obj_set_style_radius(del_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(del_btn, THEME_ERROR_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(del_btn, sd_context_delete_confirmed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, "Delete");
    lv_obj_set_style_text_color(del_lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(del_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(del_lbl);
}

/* ── Rename action ── */
static void sd_context_rename_done(lv_event_t *e)
{
    (void)e;
    if (!s_sd_rename_ta || !s_sd_context_entry.path[0]) { sd_context_close(); return; }
    const char *new_name = lv_textarea_get_text(s_sd_rename_ta);
    if (!new_name || !new_name[0]) { toast_show("Name cannot be empty"); return; }
    if (strchr(new_name, '/')) { toast_show("Name cannot contain /"); return; }

    /* Build new path: parent dir + new name */
    char new_path[SD_EXPLORER_PATH_MAX];
    char *slash = strrchr(s_sd_context_entry.path, '/');
    if (!slash) { sd_context_close(); return; }
    size_t dir_len = (size_t)(slash - s_sd_context_entry.path);
    snprintf(new_path, sizeof(new_path), "%.*s/%s", (int)dir_len, s_sd_context_entry.path, new_name);

    /* Check if destination already exists */
    struct stat st;
    if (stat(new_path, &st) == 0) {
        toast_show("A file with that name already exists");
        return;
    }

    int result = rename(s_sd_context_entry.path, new_path);
    sd_context_close();
    if (result == 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Renamed to %s", new_name);
        toast_show(msg);
    } else {
        toast_show("Rename failed");
    }
    sd_explorer_refresh();
}

static void sd_context_rename_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        sd_context_rename_done(e);
    } else if (code == LV_EVENT_CANCEL) {
        sd_context_close();
    }
}

static void sd_context_rename_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_sd_context_entry.path[0]) { sd_context_close(); return; }
    if (!s_sd_context_overlay) return;
    lv_obj_clean(s_sd_context_overlay);

    lv_obj_t *card = lv_obj_create(s_sd_context_overlay);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 20);
    lv_obj_set_width(card, 620);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Rename");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    s_sd_rename_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_sd_rename_ta, true);
    lv_textarea_set_max_length(s_sd_rename_ta, SD_EXPLORER_NAME_MAX - 1);
    lv_textarea_set_text(s_sd_rename_ta, s_sd_context_entry.name);
    lv_obj_set_size(s_sd_rename_ta, LV_PCT(100), 54);
    lv_obj_set_style_bg_color(s_sd_rename_ta, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sd_rename_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_sd_rename_ta, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(s_sd_rename_ta, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sd_rename_ta, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_sd_rename_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sd_rename_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_sd_rename_ta, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_sd_rename_ta, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_sd_rename_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_sd_rename_ta, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_sd_rename_ta, sd_context_rename_ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_sd_rename_ta, sd_context_rename_ta_event, LV_EVENT_CANCEL, NULL);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, LV_PART_MAIN);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_button_create(btns);
    lv_obj_set_size(cancel_btn, 110, 42);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, sd_context_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancel_btn);
    lv_label_set_text(clbl, "Cancel");
    lv_obj_set_style_text_color(clbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(clbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(clbl);

    lv_obj_t *save_btn = lv_button_create(btns);
    lv_obj_set_size(save_btn, 110, 42);
    lv_obj_set_style_radius(save_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(save_btn, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(save_btn, sd_context_rename_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t *slbl = lv_label_create(save_btn);
    lv_label_set_text(slbl, "Rename");
    lv_obj_set_style_text_color(slbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(slbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(slbl);

    /* Keyboard at bottom */
    s_sd_rename_keyboard = lv_keyboard_create(s_sd_context_overlay);
    lv_obj_set_size(s_sd_rename_keyboard, LV_PCT(100), 260);
    lv_obj_align(s_sd_rename_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_sd_rename_keyboard, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sd_rename_keyboard, 0, LV_PART_MAIN);
    lv_keyboard_set_textarea(s_sd_rename_keyboard, s_sd_rename_ta);
}

/* ── Info action ── */
static void sd_context_info_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_sd_context_entry.path[0]) { sd_context_close(); return; }
    if (!s_sd_context_overlay) return;
    lv_obj_clean(s_sd_context_overlay);

    lv_obj_t *card = lv_obj_create(s_sd_context_overlay);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 20);
    lv_obj_set_width(card, 620);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "File Info");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    /* Info rows */
    char buf[SD_EXPLORER_PATH_MAX + 32];

    snprintf(buf, sizeof(buf), "Name:  %s", s_sd_context_entry.name);
    lv_obj_t *l1 = lv_label_create(card);
    lv_label_set_text(l1, buf);
    lv_obj_set_width(l1, LV_PCT(100));
    lv_label_set_long_mode(l1, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l1, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(l1, THEME_FONT_BODY, LV_PART_MAIN);

    snprintf(buf, sizeof(buf), "Type:  %s", s_sd_context_entry.is_dir ? "Folder" : "File");
    lv_obj_t *l2 = lv_label_create(card);
    lv_label_set_text(l2, buf);
    lv_obj_set_style_text_color(l2, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(l2, THEME_FONT_BODY, LV_PART_MAIN);

    if (!s_sd_context_entry.is_dir) {
        char sz[24];
        sd_explorer_format_size(sz, sizeof(sz), s_sd_context_entry.size_bytes);
        snprintf(buf, sizeof(buf), "Size:  %s  (%llu bytes)", sz,
                 (unsigned long long)s_sd_context_entry.size_bytes);
        lv_obj_t *l3 = lv_label_create(card);
        lv_label_set_text(l3, buf);
        lv_obj_set_style_text_color(l3, THEME_TEXT_SECONDARY, LV_PART_MAIN);
        lv_obj_set_style_text_font(l3, THEME_FONT_BODY, LV_PART_MAIN);
    }

    /* Show relative path from mount point */
    size_t root_len = strlen(BSP_SD_MOUNT_POINT);
    const char *rel = s_sd_context_entry.path;
    if (strncmp(rel, BSP_SD_MOUNT_POINT, root_len) == 0) rel += root_len;
    snprintf(buf, sizeof(buf), "Path:  %s", rel);
    lv_obj_t *l4 = lv_label_create(card);
    lv_label_set_text(l4, buf);
    lv_obj_set_width(l4, LV_PCT(100));
    lv_label_set_long_mode(l4, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l4, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(l4, THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *close_btn = lv_button_create(card);
    lv_obj_set_size(close_btn, LV_PCT(100), 44);
    lv_obj_set_style_radius(close_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, sd_context_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(close_btn);
    lv_label_set_text(clbl, "Close");
    lv_obj_set_style_text_color(clbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(clbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(clbl);
}

/* ── Open folder (from context menu) ── */
static void sd_context_open_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_sd_context_entry.is_dir || !s_sd_context_entry.path[0]) { sd_context_close(); return; }
    snprintf(s_sd_current_path, sizeof(s_sd_current_path), "%s", s_sd_context_entry.path);
    sd_context_close();
    sd_explorer_refresh();
}

static void sd_context_overlay_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_sd_context_overlay) {
        sd_context_close();
    }
}

/* ── Helper: context menu action row button ── */
static lv_obj_t *sd_context_action_btn(lv_obj_t *parent, const char *icon_text,
                                        const char *label_text, lv_color_t color,
                                        lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_PCT(100), 50);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(row);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon_text);
    lv_obj_set_style_text_color(ic, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(ic, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_width(ic, 28);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, THEME_FONT_BODY, LV_PART_MAIN);

    return btn;
}

static void sd_explorer_long_pressed(lv_event_t *e)
{
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_sd_entry_count) return;
    if (!s_sd_entries) return;

    s_sd_long_press_fired = true;
    sd_context_close();
    /* Snapshot the entry so it survives any refresh */
    memcpy(&s_sd_context_entry, &s_sd_entries[index], sizeof(sd_explorer_entry_t));

    /* Create dark overlay on lv_layer_top */
    s_sd_context_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sd_context_overlay);
    lv_obj_set_size(s_sd_context_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sd_context_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sd_context_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_sd_context_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sd_context_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sd_context_overlay, sd_context_overlay_clicked, LV_EVENT_CLICKED, NULL);

    /* Card with entry info + action buttons */
    lv_obj_t *card = lv_obj_create(s_sd_context_overlay);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 20);
    lv_obj_set_width(card, 580);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title row: icon + name */
    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(hdr, 6, LV_PART_MAIN);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(hdr);
    lv_label_set_text(icon, s_sd_context_entry.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(icon, s_sd_context_entry.is_dir ? THEME_PRIMARY_COLOR : THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, THEME_FONT_TITLE, LV_PART_MAIN);

    lv_obj_t *name_col = lv_obj_create(hdr);
    lv_obj_remove_style_all(name_col);
    lv_obj_set_flex_grow(name_col, 1);
    lv_obj_set_height(name_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(name_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(name_col, 2, LV_PART_MAIN);
    lv_obj_clear_flag(name_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(name_col);
    lv_label_set_text(name, s_sd_context_entry.name);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(name, THEME_FONT_BODY, LV_PART_MAIN);

    char detail[64];
    if (s_sd_context_entry.is_dir) {
        snprintf(detail, sizeof(detail), "Folder");
    } else {
        char sz[24];
        sd_explorer_format_size(sz, sizeof(sz), s_sd_context_entry.size_bytes);
        snprintf(detail, sizeof(detail), "%s", sz);
    }
    lv_obj_t *meta = lv_label_create(name_col);
    lv_label_set_text(meta, detail);
    lv_obj_set_style_text_color(meta, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(meta, THEME_FONT_SMALL, LV_PART_MAIN);

    /* ── Action buttons ── */
    if (s_sd_context_entry.is_dir) {
        sd_context_action_btn(card, LV_SYMBOL_DIRECTORY, "Open",
                              THEME_PRIMARY_COLOR, sd_context_open_clicked);
    }
    sd_context_action_btn(card, LV_SYMBOL_EDIT, "Rename",
                          THEME_TEXT_SECONDARY, sd_context_rename_clicked);
    sd_context_action_btn(card, LV_SYMBOL_LIST, "Info",
                          THEME_TEXT_SECONDARY, sd_context_info_clicked);
    /* Delete button — styled differently */
    lv_obj_t *del_btn = sd_context_action_btn(card, LV_SYMBOL_TRASH, "Delete",
                                               THEME_ERROR_COLOR, sd_context_delete_clicked);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x2A1015), LV_PART_MAIN);
    lv_obj_set_style_border_width(del_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(del_btn, THEME_ERROR_COLOR, LV_PART_MAIN);

    /* Cancel at bottom */
    lv_obj_t *cancel_btn = lv_button_create(card);
    lv_obj_set_size(cancel_btn, LV_PCT(100), 42);
    lv_obj_set_style_radius(cancel_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, sd_context_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
}

/* ── New Folder action ── */
static void sd_newfolder_done(lv_event_t *e)
{
    (void)e;
    if (!s_sd_newfolder_ta) { sd_newfolder_close(); return; }
    const char *name = lv_textarea_get_text(s_sd_newfolder_ta);
    if (!name || !name[0]) { toast_show("Name cannot be empty"); return; }
    if (strchr(name, '/')) { toast_show("Name cannot contain /"); return; }

    char new_path[SD_EXPLORER_PATH_MAX];
    snprintf(new_path, sizeof(new_path), "%s/%s", s_sd_current_path, name);

    struct stat st;
    if (stat(new_path, &st) == 0) {
        toast_show("Already exists");
        return;
    }

    int result = mkdir(new_path, 0775);
    sd_newfolder_close();
    if (result == 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Created %s", name);
        toast_show(msg);
    } else {
        toast_show("Create folder failed");
    }
    sd_explorer_refresh();
}

static void sd_newfolder_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) sd_newfolder_done(e);
    else if (code == LV_EVENT_CANCEL) sd_newfolder_close();
}

static void sd_newfolder_cancel(lv_event_t *e)
{
    (void)e;
    sd_newfolder_close();
}

static void sd_explorer_newfolder_clicked(lv_event_t *e)
{
    (void)e;
    sd_newfolder_close();

    s_sd_newfolder_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sd_newfolder_overlay);
    lv_obj_set_size(s_sd_newfolder_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sd_newfolder_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sd_newfolder_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_sd_newfolder_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sd_newfolder_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_sd_newfolder_overlay);
    lv_obj_remove_style_all(card);
    theme_style_glass_panel(card, 20);
    lv_obj_set_width(card, 620);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "New Folder");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    s_sd_newfolder_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_sd_newfolder_ta, true);
    lv_textarea_set_max_length(s_sd_newfolder_ta, SD_EXPLORER_NAME_MAX - 1);
    lv_textarea_set_placeholder_text(s_sd_newfolder_ta, "Folder name");
    lv_obj_set_size(s_sd_newfolder_ta, LV_PCT(100), 54);
    lv_obj_set_style_bg_color(s_sd_newfolder_ta, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sd_newfolder_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_sd_newfolder_ta, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(s_sd_newfolder_ta, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sd_newfolder_ta, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_sd_newfolder_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sd_newfolder_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_sd_newfolder_ta, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_sd_newfolder_ta, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_sd_newfolder_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_sd_newfolder_ta, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_sd_newfolder_ta, sd_newfolder_ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_sd_newfolder_ta, sd_newfolder_ta_event, LV_EVENT_CANCEL, NULL);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, LV_PART_MAIN);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel_btn = lv_button_create(btns);
    lv_obj_set_size(cancel_btn, 110, 42);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, sd_newfolder_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancel_btn);
    lv_label_set_text(clbl, "Cancel");
    lv_obj_set_style_text_color(clbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(clbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(clbl);

    lv_obj_t *create_btn = lv_button_create(btns);
    lv_obj_set_size(create_btn, 110, 42);
    lv_obj_set_style_radius(create_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(create_btn, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(create_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(create_btn, sd_newfolder_done, LV_EVENT_CLICKED, NULL);
    lv_obj_t *crlbl = lv_label_create(create_btn);
    lv_label_set_text(crlbl, "Create");
    lv_obj_set_style_text_color(crlbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(crlbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_center(crlbl);

    /* Keyboard at bottom */
    lv_obj_t *kb = lv_keyboard_create(s_sd_newfolder_overlay);
    lv_obj_set_size(kb, LV_PCT(100), 260);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN);
    lv_keyboard_set_textarea(kb, s_sd_newfolder_ta);
}

static void sd_explorer_entry_clicked(lv_event_t *e)
{
    /* Suppress click that fires after a long-press */
    if (s_sd_long_press_fired) {
        s_sd_long_press_fired = false;
        return;
    }
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_sd_entry_count) return;
    if (!s_sd_entries) return;
    sd_explorer_entry_t *entry = &s_sd_entries[index];
    if (entry->is_dir) {
        snprintf(s_sd_current_path, sizeof(s_sd_current_path), "%s", entry->path);
        sd_explorer_refresh();
        return;
    }

    char size[24];
    sd_explorer_format_size(size, sizeof(size), entry->size_bytes);
    char status[120];
    snprintf(status, sizeof(status), "%s  %s", entry->name, size);
    sd_explorer_set_status(status);
    toast_show(status);
}

static void sd_explorer_add_row(uint8_t index)
{
    if (!s_sd_list || index >= s_sd_entry_count) return;
    sd_explorer_entry_t *entry = &s_sd_entries[index];

    lv_obj_t *row = lv_button_create(s_sd_list);
    lv_obj_set_size(row, LV_PCT(100), 74);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, sd_explorer_entry_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    lv_obj_add_event_cb(row, sd_explorer_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, entry->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
    lv_obj_set_width(icon, 34);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, entry->is_dir ? THEME_PRIMARY_COLOR : THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, THEME_FONT_LARGE, LV_PART_MAIN);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, entry->name);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(name, THEME_FONT_BODY, LV_PART_MAIN);

    char detail[64];
    if (entry->is_dir) {
        snprintf(detail, sizeof(detail), "Folder");
    } else {
        char size[24];
        sd_explorer_format_size(size, sizeof(size), entry->size_bytes);
        snprintf(detail, sizeof(detail), "%s", size);
    }
    lv_obj_t *meta = lv_label_create(col);
    lv_label_set_text(meta, detail);
    lv_obj_set_width(meta, LV_PCT(100));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(meta, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(meta, THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, entry->is_dir ? ">" : "");
    lv_obj_set_style_text_color(arrow, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(arrow, THEME_FONT_TITLE, LV_PART_MAIN);
}

static void sd_explorer_add_storage_info(void)
{
    if (!s_sd_list) return;
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) return;
    uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;

    lv_obj_t *info = lv_obj_create(s_sd_list);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(info, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(info, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_radius(info, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(info, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 8, LV_PART_MAIN);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    /* Title row */
    lv_obj_t *title_row = lv_obj_create(info);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disk_icon = lv_label_create(title_row);
    lv_label_set_text(disk_icon, LV_SYMBOL_SAVE);
    lv_obj_set_style_text_color(disk_icon, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(disk_icon, THEME_FONT_TITLE, LV_PART_MAIN);

    lv_obj_t *disk_title = lv_label_create(title_row);
    lv_label_set_text(disk_title, "SD Card Storage");
    lv_obj_set_style_text_color(disk_title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(disk_title, THEME_FONT_BODY, LV_PART_MAIN);

    /* Usage bar */
    lv_obj_t *bar = lv_bar_create(info);
    lv_obj_set_size(bar, LV_PCT(100), 14);
    lv_obj_set_style_radius(bar, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    uint8_t used_pct = total_bytes > 0 ? (uint8_t)((used_bytes * 100ULL) / total_bytes) : 0;
    lv_color_t bar_color = used_pct > 90 ? THEME_ERROR_COLOR :
                           used_pct > 70 ? lv_color_hex(0xFFB347) : THEME_PRIMARY_COLOR;
    lv_obj_set_style_bg_color(bar, bar_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, used_pct, LV_ANIM_OFF);

    /* Stats row */
    char total_str[16], used_str[16], free_str[16];
    sd_explorer_format_size(total_str, sizeof(total_str), total_bytes);
    sd_explorer_format_size(used_str, sizeof(used_str), used_bytes);
    sd_explorer_format_size(free_str, sizeof(free_str), free_bytes);

    lv_obj_t *stats = lv_obj_create(info);
    lv_obj_remove_style_all(stats);
    lv_obj_set_size(stats, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    char stat_buf[48];
    snprintf(stat_buf, sizeof(stat_buf), "Used: %s", used_str);
    lv_obj_t *used_lbl = lv_label_create(stats);
    lv_label_set_text(used_lbl, stat_buf);
    lv_obj_set_style_text_color(used_lbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(used_lbl, THEME_FONT_SMALL, LV_PART_MAIN);

    snprintf(stat_buf, sizeof(stat_buf), "Free: %s", free_str);
    lv_obj_t *free_lbl = lv_label_create(stats);
    lv_label_set_text(free_lbl, stat_buf);
    lv_obj_set_style_text_color(free_lbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(free_lbl, THEME_FONT_SMALL, LV_PART_MAIN);

    snprintf(stat_buf, sizeof(stat_buf), "Total: %s", total_str);
    lv_obj_t *total_lbl = lv_label_create(stats);
    lv_label_set_text(total_lbl, stat_buf);
    lv_obj_set_style_text_color(total_lbl, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(total_lbl, THEME_FONT_SMALL, LV_PART_MAIN);
}

static void sd_explorer_refresh(void)
{
    if (!s_sd_list) return;
    settings_hide_keyboard();
    lv_obj_clean(s_sd_list);
    s_sd_entry_count = 0;
    if (!sd_explorer_entries_ensure()) {
        sd_explorer_set_status("Out of memory");
        sd_explorer_message("Unable to allocate folder list");
        return;
    }

    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) {
        sd_explorer_set_status(sd_storage_last_error());
        sd_explorer_message(sd_storage_last_error());
        return;
    }

    /* Show storage info card when at root */
    if (sd_explorer_is_root()) {
        sd_explorer_add_storage_info();
    }

    DIR *dir = opendir(s_sd_current_path);
    if (!dir) {
        char status[96];
        snprintf(status, sizeof(status), "Open failed: errno %d", errno);
        sd_explorer_set_status(status);
        sd_explorer_message("Unable to open this folder");
        return;
    }

    uint16_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        total++;
        if (s_sd_entry_count >= SD_EXPLORER_MAX_ENTRIES) continue;

        sd_explorer_entry_t *entry = &s_sd_entries[s_sd_entry_count];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        if (!sd_explorer_join_path(entry->path, sizeof(entry->path), s_sd_current_path, name)) continue;

        struct stat st;
        if (stat(entry->path, &st) == 0) {
            entry->is_dir = S_ISDIR(st.st_mode);
            entry->size_bytes = entry->is_dir ? 0 : (uint64_t)st.st_size;
        } else {
            entry->is_dir = false;
            entry->size_bytes = 0;
        }
        s_sd_entry_count++;
    }
    closedir(dir);

    qsort(s_sd_entries, s_sd_entry_count, sizeof(s_sd_entries[0]), sd_explorer_entry_compare);

    char path_text[96];
    snprintf(path_text, sizeof(path_text), "%s", sd_explorer_display_path());
    label_set_text_if_changed(s_sd_path_label, path_text);

    char status[96];
    if (total > s_sd_entry_count) {
        snprintf(status, sizeof(status), "%u items  showing %u", (unsigned)total, (unsigned)s_sd_entry_count);
    } else {
        snprintf(status, sizeof(status), "%u item%s", (unsigned)total, total == 1 ? "" : "s");
    }
    sd_explorer_set_status(status);

    if (s_sd_entry_count == 0 && !sd_explorer_is_root()) {
        sd_explorer_message("Folder is empty");
        return;
    }

    for (uint8_t i = 0; i < s_sd_entry_count; i++) {
        sd_explorer_add_row(i);
    }
}

static void sd_explorer_refresh_clicked(lv_event_t *e)
{
    (void)e;
    sd_explorer_refresh();
}

static void sd_explorer_root_clicked(lv_event_t *e)
{
    (void)e;
    snprintf(s_sd_current_path, sizeof(s_sd_current_path), "%s", BSP_SD_MOUNT_POINT);
    sd_explorer_refresh();
}

static void sd_explorer_up_clicked(lv_event_t *e)
{
    (void)e;
    if (sd_explorer_is_root()) {
        toast_show("Already at SD root");
        return;
    }

    char *slash = strrchr(s_sd_current_path, '/');
    if (!slash || slash <= s_sd_current_path + strlen(BSP_SD_MOUNT_POINT)) {
        snprintf(s_sd_current_path, sizeof(s_sd_current_path), "%s", BSP_SD_MOUNT_POINT);
    } else {
        *slash = '\0';
    }
    sd_explorer_refresh();
}

static void settings_sd_open(lv_event_t *e)
{
    (void)e;
    settings_show_panel(s_settings_sd_page);
    sd_explorer_refresh();
}

static void wifi_scan_set_status(const char *text)
{
    if (s_wifi_scan_status) lv_label_set_text(s_wifi_scan_status, text ? text : "");
}

static void wifi_scan_list_message(const char *text)
{
    if (!s_wifi_list) return;
    lv_obj_clean(s_wifi_list);
    lv_obj_t *label = lv_label_create(s_wifi_list);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN);
}

static void wifi_password_close(void)
{
    settings_hide_keyboard();
    if (s_wifi_password_overlay) {
        lv_obj_delete(s_wifi_password_overlay);
        s_wifi_password_overlay = NULL;
        s_wifi_password_ta = NULL;
        s_wifi_password_keyboard = NULL;
    }
}

static void wifi_password_cancel_clicked(lv_event_t *e)
{
    (void)e;
    wifi_password_close();
}

static void wifi_forget_saved_network(void)
{
    settings_hide_keyboard();
    esp_err_t err = app_config_wifi_clear();
    if (err == ESP_OK) {
        (void)wifi_sta_init();
        s_wifi_scan_count = 0;
        wifi_scan_set_status("Saved network forgotten");
        wifi_scan_list_message("No saved network");
        toast_show("Wi-Fi forgotten");
    } else {
        toast_show(esp_err_to_name(err));
    }
    settings_status_refresh(NULL);
}

static void wifi_saved_forget_clicked(lv_event_t *e)
{
    (void)e;
    wifi_password_close();
    wifi_forget_saved_network();
}

static void wifi_saved_reconnect_clicked(lv_event_t *e)
{
    (void)e;
    wifi_password_close();
    esp_err_t err = wifi_sta_init();
    if (err == ESP_OK) {
        wifi_scan_set_status("Connecting");
        toast_show("Connecting Wi-Fi");
    } else {
        wifi_scan_set_status(esp_err_to_name(err));
        toast_show(esp_err_to_name(err));
    }
    settings_status_refresh(NULL);
}

static void wifi_password_show_keyboard(void)
{
    if (!s_wifi_password_keyboard || !s_wifi_password_ta) return;
    lv_keyboard_set_textarea(s_wifi_password_keyboard, s_wifi_password_ta);
    lv_keyboard_set_mode(s_wifi_password_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(s_wifi_password_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_password_keyboard);
    lv_obj_add_state(s_wifi_password_ta, LV_STATE_FOCUSED);
}

static void wifi_password_submit(void)
{
    const char *psk = s_wifi_password_ta ? lv_textarea_get_text(s_wifi_password_ta) : "";
    if (s_wifi_selected_secure && (!psk || !psk[0])) {
        toast_show("Password required");
        wifi_scan_set_status("Password required");
        return;
    }
    esp_err_t err = wifi_sta_connect(s_wifi_selected_ssid, psk ? psk : "");
    wifi_password_close();

    if (err == ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to %s", s_wifi_selected_ssid);
        wifi_scan_set_status(msg);
        (void)provision_wifi_update();
        toast_show("Connecting Wi-Fi");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        wifi_scan_set_status("Wi-Fi driver disabled");
        toast_show("Wi-Fi unavailable");
    } else if (err == ESP_ERR_INVALID_ARG) {
        wifi_scan_set_status("SSID or password invalid");
        toast_show("Wi-Fi details invalid");
    } else {
        wifi_scan_set_status(esp_err_to_name(err));
        toast_show(esp_err_to_name(err));
    }
    settings_status_refresh(NULL);
}

static void wifi_password_connect_clicked(lv_event_t *e)
{
    (void)e;
    wifi_password_submit();
}

static void wifi_password_ta_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSED) {
        wifi_password_show_keyboard();
    } else if (code == LV_EVENT_READY) {
        wifi_password_submit();
    } else if (code == LV_EVENT_CANCEL) {
        wifi_password_close();
    }
}

static void wifi_password_open(const char *ssid, bool secure)
{
    if (!ssid || !ssid[0]) return;
    snprintf(s_wifi_selected_ssid, sizeof(s_wifi_selected_ssid), "%s", ssid);
    s_wifi_selected_secure = secure;

    wifi_password_close();

    s_wifi_password_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wifi_password_overlay);
    lv_obj_set_size(s_wifi_password_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_wifi_password_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wifi_password_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_wifi_password_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wifi_password_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_wifi_password_overlay);

    lv_obj_t *card = deep_card(s_wifi_password_overlay);
    lv_obj_set_width(card, 620);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, ssid);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, secure ? "Password required" : "Open network; password optional");
    lv_obj_set_style_text_color(hint, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, THEME_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *input_col = lv_obj_create(card);
    lv_obj_remove_style_all(input_col);
    lv_obj_set_size(input_col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(input_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(input_col, 8, LV_PART_MAIN);
    lv_obj_clear_flag(input_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *input_label = lv_label_create(input_col);
    lv_label_set_text(input_label, secure ? "Password" : "Password (optional)");
    lv_obj_set_style_text_color(input_label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(input_label, THEME_FONT_LABEL, LV_PART_MAIN);

    s_wifi_password_ta = lv_textarea_create(input_col);
    lv_textarea_set_one_line(s_wifi_password_ta, true);
    lv_textarea_set_max_length(s_wifi_password_ta, 64);
    lv_textarea_set_placeholder_text(s_wifi_password_ta, "Network password");
    lv_textarea_set_password_mode(s_wifi_password_ta, true);
    lv_obj_set_size(s_wifi_password_ta, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(s_wifi_password_ta, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wifi_password_ta, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wifi_password_ta, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(s_wifi_password_ta, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_password_ta, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_wifi_password_ta, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wifi_password_ta, 16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_password_ta, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wifi_password_ta, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_wifi_password_ta, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_wifi_password_ta, 10, LV_PART_MAIN);
    lv_obj_add_flag(s_wifi_password_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(s_wifi_password_ta, wifi_password_ta_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wifi_password_ta, wifi_password_ta_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_wifi_password_ta, wifi_password_ta_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_wifi_password_ta, wifi_password_ta_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_wifi_password_ta, wifi_password_ta_event, LV_EVENT_CANCEL, NULL);

    app_wifi_config_t saved;
    if (app_config_wifi_load(&saved) == ESP_OK && saved.configured && strcmp(saved.ssid, ssid) == 0) {
        lv_textarea_set_text(s_wifi_password_ta, saved.psk);
    }

    lv_obj_t *buttons = settings_button_row(card);
    settings_button(buttons, "Cancel", false, wifi_password_cancel_clicked);
    settings_button(buttons, "Connect", true, wifi_password_connect_clicked);

    s_wifi_password_keyboard = lv_keyboard_create(s_wifi_password_overlay);
    lv_obj_set_size(s_wifi_password_keyboard, LV_PCT(100), 250);
    lv_obj_align(s_wifi_password_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_password_keyboard, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_password_keyboard, 0, LV_PART_MAIN);

    wifi_password_show_keyboard();
}

static void wifi_saved_info_open(const wifi_scan_result_t *network)
{
    if (!network || !network->ssid[0]) return;

    wifi_password_close();

    services_status_t st;
    services_status_get(&st);
    bool connected = st.wifi_connected && strcmp(st.wifi_ssid, network->ssid) == 0;

    s_wifi_password_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wifi_password_overlay);
    lv_obj_set_size(s_wifi_password_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_wifi_password_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wifi_password_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_wifi_password_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wifi_password_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = deep_card(s_wifi_password_overlay);
    lv_obj_set_width(card, 620);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, network->ssid);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    char detail[96];
    snprintf(detail, sizeof(detail), "%s  %d dBm  Ch %u  %s",
             connected ? "Connected" : "Saved",
             network->rssi,
             network->channel,
             network->secure ? "Secured" : "Open");
    lv_obj_t *meta = lv_label_create(card);
    lv_label_set_text(meta, detail);
    lv_obj_set_width(meta, LV_PCT(100));
    lv_label_set_long_mode(meta, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(meta, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(meta, THEME_FONT_BODY, LV_PART_MAIN);

    if (connected) {
        lv_obj_t *ip = lv_label_create(card);
        lv_label_set_text(ip, st.ip_addr[0] ? st.ip_addr : "-");
        lv_obj_set_style_text_color(ip, THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(ip, THEME_FONT_LABEL, LV_PART_MAIN);
    }

    lv_obj_t *buttons = settings_button_row(card);
    settings_button(buttons, "Close", false, wifi_password_cancel_clicked);
    if (!connected) settings_button(buttons, "Connect", true, wifi_saved_reconnect_clicked);
    settings_button(buttons, "Forget", false, wifi_saved_forget_clicked);
}

static void wifi_network_clicked(lv_event_t *e)
{
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index < 0 || (size_t)index >= s_wifi_scan_count) return;

    app_wifi_config_t saved;
    if (app_config_wifi_load(&saved) == ESP_OK && saved.configured &&
        strcmp(saved.ssid, s_wifi_scan_results[index].ssid) == 0) {
        wifi_saved_info_open(&s_wifi_scan_results[index]);
        return;
    }

    wifi_password_open(s_wifi_scan_results[index].ssid, s_wifi_scan_results[index].secure);
}

static void wifi_scan_render_list(void)
{
    if (!s_wifi_list) return;
    lv_obj_clean(s_wifi_list);

    app_wifi_config_t saved = {0};
    bool has_saved = app_config_wifi_load(&saved) == ESP_OK && saved.configured;
    services_status_t st;
    services_status_get(&st);

    if (s_wifi_scan_count == 0) {
        lv_obj_t *empty = lv_label_create(s_wifi_list);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(empty, THEME_FONT_BODY, LV_PART_MAIN);
        return;
    }

    for (size_t i = 0; i < s_wifi_scan_count; i++) {
        bool is_saved = has_saved && strcmp(saved.ssid, s_wifi_scan_results[i].ssid) == 0;
        bool is_current = st.wifi_connected && strcmp(st.wifi_ssid, s_wifi_scan_results[i].ssid) == 0;

        lv_obj_t *row = lv_button_create(s_wifi_list);
        lv_obj_set_size(row, LV_PCT(100), 70);
        lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, is_current ? THEME_PRIMARY_COLOR : THEME_BORDER_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, THEME_SURFACE_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 18, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
        lv_obj_add_event_cb(row, wifi_network_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(icon, THEME_PRIMARY_COLOR, LV_PART_MAIN);
        lv_obj_set_style_text_font(icon, THEME_FONT_LARGE, LV_PART_MAIN);

        lv_obj_t *text_col = lv_obj_create(row);
        lv_obj_remove_style_all(text_col);
        lv_obj_remove_flag(text_col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_size(text_col, 1, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text_col, 1);
        lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text_col, 4, LV_PART_MAIN);
        lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ssid = lv_label_create(text_col);
        lv_label_set_text(ssid, s_wifi_scan_results[i].ssid);
        lv_obj_set_width(ssid, LV_PCT(100));
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(ssid, THEME_TEXT_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_text_font(ssid, THEME_FONT_BODY, LV_PART_MAIN);

        char detail[80];
        snprintf(detail, sizeof(detail), "%s%d dBm  Ch %u  %s",
                 is_current ? "Connected  " : (is_saved ? "Saved  " : ""),
                 s_wifi_scan_results[i].rssi,
                 s_wifi_scan_results[i].channel,
                 s_wifi_scan_results[i].secure ? "Secured" : "Open");
        lv_obj_t *meta = lv_label_create(text_col);
        lv_label_set_text(meta, detail);
        lv_obj_set_style_text_color(meta, THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(meta, THEME_FONT_SMALL, LV_PART_MAIN);

        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, ">");
        lv_obj_set_style_text_color(arrow, THEME_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_style_text_font(arrow, THEME_FONT_TITLE, LV_PART_MAIN);
    }
}

static void wifi_scan_complete_async(void *user_data)
{
    esp_err_t err = (esp_err_t)(intptr_t)user_data;
    s_wifi_scan_task = NULL;
    s_wifi_scan_in_progress = false;
    if (s_wifi_scan_btn) lv_obj_clear_state(s_wifi_scan_btn, LV_STATE_DISABLED);

    if (err == ESP_OK) {
        wifi_scan_render_list();
        if (s_wifi_scan_count > 0) {
            char msg[40];
            snprintf(msg, sizeof(msg), "Found %u networks", (unsigned)s_wifi_scan_count);
            wifi_scan_set_status(msg);
            toast_show(msg);
        } else {
            wifi_scan_set_status("No networks found");
            toast_show("No networks found");
        }
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        wifi_scan_set_status("Wi-Fi scan unavailable");
        toast_show("Wi-Fi scan unavailable");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Scan failed: %s", esp_err_to_name(err));
        wifi_scan_set_status(msg);
        toast_show(esp_err_to_name(err));
    }
}

static void wifi_scan_worker(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_scan_networks(s_wifi_scan_results, SETTINGS_WIFI_SCAN_MAX, &s_wifi_scan_count);

    bool posted = false;
    if (bsp_display_lock(2000)) {
        posted = lv_async_call(wifi_scan_complete_async, (void *)(intptr_t)err) == LV_RESULT_OK;
        bsp_display_unlock();
    }
    if (!posted) {
        ESP_LOGE(TAG, "Unable to post Wi-Fi scan result to UI: %s", esp_err_to_name(err));
        s_wifi_scan_task = NULL;
        s_wifi_scan_in_progress = false;
    }

    vTaskDelete(NULL);
}

static void wifi_scan_start(void)
{
    if (s_wifi_scan_in_progress) {
        toast_show("Scan already running");
        return;
    }

    ESP_LOGI(TAG, "Wi-Fi scan UI requested");
    settings_hide_keyboard();
    s_wifi_scan_count = 0;
    s_wifi_scan_in_progress = true;
    if (s_wifi_scan_btn) lv_obj_add_state(s_wifi_scan_btn, LV_STATE_DISABLED);
    if (s_wifi_list) {
        wifi_scan_list_message("Scanning...");
    }
    wifi_scan_set_status("Scanning...");
    toast_show("Scanning Wi-Fi");

    BaseType_t ok = xTaskCreateWithCaps(wifi_scan_worker, "wifi_scan", 6144,
                                        NULL, 5, &s_wifi_scan_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(wifi_scan_worker, "wifi_scan", 6144,
                                       NULL, 5, &s_wifi_scan_task);
    if (ok != pdPASS) {
        s_wifi_scan_task = NULL;
        s_wifi_scan_in_progress = false;
        if (s_wifi_scan_btn) lv_obj_clear_state(s_wifi_scan_btn, LV_STATE_DISABLED);
        wifi_scan_set_status("Unable to start scan");
        toast_show("Unable to start scan");
    }
}

static void wifi_list_scroll_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *list = lv_event_get_target(e);
    if (code == LV_EVENT_SCROLL) {
        if (!s_wifi_scan_in_progress && lv_obj_get_scroll_y(list) < -48) {
            s_wifi_pull_armed = true;
        }
    } else if (code == LV_EVENT_SCROLL_END) {
        if (s_wifi_pull_armed && !s_wifi_scan_in_progress) {
            wifi_scan_start();
        }
        s_wifi_pull_armed = false;
    }
}

static void settings_wifi_open(lv_event_t *e)
{
    (void)e;
    settings_show_panel(s_settings_wifi_page);
    settings_status_refresh(NULL);
    wifi_scan_start();
}

static void format_pct_label(lv_obj_t *label, const char *name, int value)
{
    if (!label) return;
    char text[40];
    snprintf(text, sizeof(text), "%s %d%%", name, value);
    lv_label_set_text(label, text);
}

static void format_seconds_label(lv_obj_t *label, const char *name, int value)
{
    if (!label) return;
    char text[40];
    snprintf(text, sizeof(text), "%s %ds", name, value);
    lv_label_set_text(label, text);
}

static void theme_slider_event(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    if (slider == s_theme_opacity_slider) format_pct_label(s_theme_opacity_label, "Panels", value);
    else if (slider == s_theme_dim_slider) format_pct_label(s_theme_dim_label, "Dim", value);
    else if (slider == s_theme_slideshow_slider) format_seconds_label(s_theme_slideshow_label, "Slide", value);
}

static lv_obj_t *theme_slider_row(lv_obj_t *parent, const char *label_text,
                                  int min_value, int max_value, int initial,
                                  lv_obj_t **out_value_label)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label_text);
    lv_obj_set_style_text_color(name, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(name, THEME_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, "-");
    lv_obj_set_style_text_color(value_label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(value_label, THEME_FONT_LABEL, LV_PART_MAIN);
    if (out_value_label) *out_value_label = value_label;

    lv_obj_t *slider = lv_slider_create(col);
    lv_obj_set_size(slider, LV_PCT(100), 18);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x070B14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, THEME_TEXT_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, theme_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

static uint8_t theme_clamp_bg_preset(uint8_t preset)
{
    uint8_t count = bg_preset_count();
    return (count && preset < count) ? preset : 0;
}

static const char *theme_background_url(uint8_t preset, uint8_t slot)
{
    preset = theme_clamp_bg_preset(preset);
    uint8_t url_count = bg_preset_url_count(preset);
    slot %= url_count;
    const bg_preset_t *p = bg_preset_get(preset);
    const char *url = p->urls[slot];
    return (url && url[0]) ? url : p->urls[0];
}

static bool theme_choice_for_hex(theme_color_role_t role, uint32_t hex, uint8_t *out_choice)
{
    if (role >= THEME_COLOR_COUNT) return false;
    hex &= 0xFFFFFFu;
    for (uint8_t i = 0; i < THEME_COLOR_CHOICES; i++) {
        if ((s_theme_color_roles[role].choices[i] & 0xFFFFFFu) == hex) {
            if (out_choice) *out_choice = i;
            return true;
        }
    }
    return false;
}

static void theme_load_role_hex(theme_color_role_t role, uint32_t hex)
{
    if (role >= THEME_COLOR_COUNT) return;
    hex &= 0xFFFFFFu;
    uint8_t choice = 0;
    if (theme_choice_for_hex(role, hex, &choice)) {
        s_theme_selected[role] = choice;
        s_theme_custom_hex[role] = hex;
        s_theme_custom_set[role] = false;
        s_theme_using_custom[role] = false;
        return;
    }
    s_theme_selected[role] = 0;
    s_theme_custom_hex[role] = hex;
    s_theme_custom_set[role] = true;
    s_theme_using_custom[role] = true;
}

static uint32_t theme_selected_hex(theme_color_role_t role)
{
    if (role >= THEME_COLOR_COUNT) return 0;
    if (s_theme_using_custom[role]) return s_theme_custom_hex[role] & 0xFFFFFFu;
    uint8_t index = s_theme_selected[role];
    if (index >= THEME_COLOR_CHOICES) index = 0;
    return s_theme_color_roles[role].choices[index] & 0xFFFFFFu;
}

static uint32_t theme_custom_preview_hex(theme_color_role_t role)
{
    if (role >= THEME_COLOR_COUNT) return 0;
    if (s_theme_custom_set[role]) return s_theme_custom_hex[role] & 0xFFFFFFu;
    return theme_selected_hex(role);
}

static lv_color_t theme_contrast_text_color(uint32_t hex)
{
    uint8_t r = (uint8_t)((hex >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((hex >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(hex & 0xFFu);
    uint16_t luma = (uint16_t)((r * 77u + g * 150u + b * 29u) >> 8);
    return luma > 150 ? lv_color_black() : lv_color_white();
}

static bool theme_hex_is_light(uint32_t hex)
{
    uint8_t r = (uint8_t)((hex >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((hex >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(hex & 0xFFu);
    uint16_t luma = (uint16_t)((r * 77u + g * 150u + b * 29u) >> 8);
    return luma > 150;
}

static bool theme_hex_matches(uint32_t hex, uint32_t expected)
{
    return (hex & 0xFFFFFFu) == (expected & 0xFFFFFFu);
}

static void theme_palette_editor_apply_chrome(void);
static void theme_update_all_swatches(void);

static void settings_text_contrast_one(lv_obj_t *obj, bool light_panels)
{
    static const lv_style_selector_t selectors[] = {
        LV_PART_MAIN,
        LV_PART_MAIN | LV_STATE_CHECKED,
        LV_PART_MAIN | LV_STATE_PRESSED,
        LV_PART_MAIN | LV_STATE_FOCUSED,
        LV_PART_ITEMS,
        LV_PART_SELECTED,
    };

    for (size_t i = 0; i < sizeof(selectors) / sizeof(selectors[0]); i++) {
        lv_style_value_t value;
        if (lv_obj_get_local_style_prop(obj, LV_STYLE_TEXT_COLOR, &value, selectors[i]) != LV_STYLE_RES_FOUND) continue;
        uint32_t hex = lv_color_to_u32(value.color) & 0xFFFFFFu;
        uint32_t mapped = hex;
        if (light_panels) {
            if (theme_hex_matches(hex, 0xFFFFFFu)) mapped = THEME_READABLE_PRIMARY_DARK_HEX;
            else if (theme_hex_matches(hex, 0xB8C1E6u)) mapped = THEME_READABLE_SECONDARY_DARK_HEX;
            else if (theme_hex_matches(hex, 0x7883ABu)) mapped = THEME_READABLE_MUTED_DARK_HEX;
        } else {
            if (theme_hex_matches(hex, THEME_READABLE_PRIMARY_DARK_HEX)) mapped = 0xFFFFFFu;
            else if (theme_hex_matches(hex, THEME_READABLE_SECONDARY_DARK_HEX)) mapped = 0xB8C1E6u;
            else if (theme_hex_matches(hex, THEME_READABLE_MUTED_DARK_HEX)) mapped = 0x7883ABu;
        }
        if (mapped != hex) {
            value.color = lv_color_hex(mapped);
            lv_obj_set_local_style_prop(obj, LV_STYLE_TEXT_COLOR, value, selectors[i]);
        }
    }
}

static void settings_apply_text_contrast_tree(lv_obj_t *root, bool light_panels)
{
    if (!root) return;
    settings_text_contrast_one(root, light_panels);
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < child_count; i++) {
        settings_apply_text_contrast_tree(lv_obj_get_child(root, i), light_panels);
    }
}

static void settings_apply_theme_readability(void)
{
    if (!s_settings_root) return;
    bool light_panels = theme_hex_is_light(theme_selected_hex(THEME_COLOR_CARD)) ||
                        theme_hex_is_light(theme_selected_hex(THEME_COLOR_SURFACE));
    settings_apply_text_contrast_tree(s_settings_root, light_panels);
    theme_palette_editor_apply_chrome();
    theme_update_all_swatches();
}

static void theme_palette_editor_apply_chrome(void)
{
    if (s_theme_palette_card) {
        lv_obj_set_style_bg_color(s_theme_palette_card, lv_color_hex(THEME_CONTROL_PANEL_HEX), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_theme_palette_card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_theme_palette_card, lv_color_hex(THEME_CONTROL_BORDER_HEX), LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_theme_palette_card, LV_OPA_90, LV_PART_MAIN);
    }
    if (s_theme_palette_title) {
        lv_obj_set_style_text_color(s_theme_palette_title, lv_color_hex(THEME_CONTROL_MUTED_HEX), LV_PART_MAIN);
    }
    for (theme_color_role_t role = 0; role < THEME_COLOR_COUNT; role++) {
        if (s_theme_color_labels[role]) {
            lv_obj_set_style_text_color(s_theme_color_labels[role], lv_color_hex(THEME_CONTROL_MUTED_HEX), LV_PART_MAIN);
        }
    }
}

static void theme_update_swatches(theme_color_role_t role)
{
    if (role >= THEME_COLOR_COUNT) return;
    for (uint8_t i = 0; i < THEME_COLOR_CHOICES; i++) {
        lv_obj_t *swatch = s_theme_swatches[role][i];
        if (!swatch) continue;
        bool selected = !s_theme_using_custom[role] && i == s_theme_selected[role];
        lv_obj_set_style_bg_color(swatch, lv_color_hex(s_theme_color_roles[role].choices[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(swatch, selected ? 4 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(swatch,
                                      selected ? theme_contrast_text_color(s_theme_color_roles[role].choices[i])
                                               : lv_color_hex(THEME_CONTROL_BORDER_HEX),
                                      LV_PART_MAIN);
    }

    lv_obj_t *custom = s_theme_custom_swatches[role];
    if (!custom) return;
    uint32_t custom_hex = theme_custom_preview_hex(role);
    lv_obj_set_style_bg_color(custom, lv_color_hex(custom_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(custom, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(custom, s_theme_using_custom[role] ? 4 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(custom,
                                  s_theme_using_custom[role] ? theme_contrast_text_color(custom_hex)
                                                             : lv_color_hex(THEME_CONTROL_BORDER_HEX),
                                  LV_PART_MAIN);
    if (s_theme_custom_labels[role]) {
        lv_obj_set_style_text_color(s_theme_custom_labels[role], theme_contrast_text_color(custom_hex), LV_PART_MAIN);
    }
}

static void theme_update_all_swatches(void)
{
    for (theme_color_role_t role = 0; role < THEME_COLOR_COUNT; role++) {
        theme_update_swatches(role);
    }
}

static void theme_update_slideshow_button(bool enabled)
{
    if (s_theme_slideshow_play_label) {
        lv_label_set_text(s_theme_slideshow_play_label, enabled ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void theme_collect_controls(app_theme_config_t *cfg)
{
    if (!cfg) return;
    if (app_config_theme_load(cfg) != ESP_OK) app_config_theme_defaults(cfg);

    if (s_theme_opacity_slider) cfg->surface_opacity_pct = (uint8_t)lv_slider_get_value(s_theme_opacity_slider);
    if (s_theme_shadows_sw) cfg->shadows_enabled = lv_obj_has_state(s_theme_shadows_sw, LV_STATE_CHECKED);
    if (s_theme_dim_slider) cfg->background_dim_pct = (uint8_t)lv_slider_get_value(s_theme_dim_slider);
    if (s_theme_slideshow_slider) cfg->slideshow_seconds = (uint16_t)lv_slider_get_value(s_theme_slideshow_slider);
    if (s_theme_background_enabled_switch) cfg->background_enabled = lv_obj_has_state(s_theme_background_enabled_switch, LV_STATE_CHECKED);
    if (s_theme_idle_only_switch) cfg->background_idle_only = lv_obj_has_state(s_theme_idle_only_switch, LV_STATE_CHECKED);
    if (s_theme_bg_dropdown) cfg->background_preset = theme_clamp_bg_preset((uint8_t)lv_dropdown_get_selected(s_theme_bg_dropdown));
    else cfg->background_preset = theme_clamp_bg_preset(cfg->background_preset);

    cfg->bg_color_hex = theme_selected_hex(THEME_COLOR_BACKGROUND);
    cfg->surface_color_hex = theme_selected_hex(THEME_COLOR_SURFACE);
    cfg->card_color_hex = theme_selected_hex(THEME_COLOR_CARD);
    cfg->border_color_hex = theme_selected_hex(THEME_COLOR_BORDER);
    cfg->primary_color_hex = theme_selected_hex(THEME_COLOR_PRIMARY);
    cfg->accent_color_hex = theme_selected_hex(THEME_COLOR_ACCENT);

    const char *url = theme_background_url(cfg->background_preset, cfg->next_slot);
    snprintf(cfg->background_url, sizeof(cfg->background_url), "%s", url ? url : "");
}

/* Re-sync the cached idle lv_screen to the current theme/opacity. It is never
 * the active screen while Settings is open, so the lv_screen_active() refresh
 * in these handlers misses it. Defined with the idle screen further down. */
static void idle_apply_theme(void);

static void theme_preview_apply(void)
{
    app_theme_config_t cfg;
    theme_collect_controls(&cfg);
    theme_apply_config(&cfg);
    theme_recolor_tree(lv_screen_active());
    theme_apply_surface_opacity(lv_screen_active());
    idle_apply_theme();
    theme_apply_shadows(lv_screen_active());
    settings_apply_theme_readability();
}

static void theme_swatch_clicked(lv_event_t *e)
{
    intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    theme_color_role_t role = (theme_color_role_t)((packed >> 8) & 0xFF);
    uint8_t choice = (uint8_t)(packed & 0xFF);
    if (role >= THEME_COLOR_COUNT || choice >= THEME_COLOR_CHOICES) return;
    s_theme_selected[role] = choice;
    s_theme_using_custom[role] = false;
    if (!s_theme_custom_set[role]) {
        s_theme_custom_hex[role] = s_theme_color_roles[role].choices[choice] & 0xFFFFFFu;
    }
    theme_update_swatches(role);
    theme_preview_apply();
}

static uint32_t theme_picker_current_hex(void)
{
    lv_color_t color = lv_color_hsv_to_rgb(s_theme_picker_hue,
                                           s_theme_picker_sat,
                                           s_theme_picker_value);
    return lv_color_to_u32(color) & 0xFFFFFFu;
}

static void theme_picker_update_marker(void)
{
    if (!s_theme_picker_marker) return;
    int32_t distance = (THEME_COLOR_WHEEL_RADIUS * (int32_t)s_theme_picker_sat + 50) / 100;
    float angle = (float)s_theme_picker_hue * THEME_DEG_TO_RAD;
    int32_t x = THEME_COLOR_WHEEL_CENTER + (int32_t)(cosf(angle) * (float)distance);
    int32_t y = THEME_COLOR_WHEEL_CENTER + (int32_t)(sinf(angle) * (float)distance);
    lv_obj_set_pos(s_theme_picker_marker, x - 8, y - 8);
}

static void theme_picker_update_preview(void)
{
    uint32_t hex = theme_picker_current_hex();
    lv_color_t color = lv_color_hex(hex);
    if (s_theme_picker_preview) {
        lv_obj_set_style_bg_color(s_theme_picker_preview, color, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_theme_picker_preview, theme_contrast_text_color(hex), LV_PART_MAIN);
    }
    if (s_theme_picker_hex_label) {
        char text[16];
        snprintf(text, sizeof(text), "#%06lX", (unsigned long)hex);
        lv_label_set_text(s_theme_picker_hex_label, text);
    }
    if (s_theme_picker_value_slider) {
        lv_obj_set_style_bg_color(s_theme_picker_value_slider, color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_theme_picker_value_slider, color, LV_PART_KNOB);
    }
    if (s_theme_picker_value_label) {
        char text[24];
        snprintf(text, sizeof(text), "Brightness %u%%", s_theme_picker_value);
        lv_label_set_text(s_theme_picker_value_label, text);
    }
    theme_picker_update_marker();
}

static void theme_picker_apply_live(void)
{
    if (s_theme_picker_role >= THEME_COLOR_COUNT) return;
    s_theme_custom_hex[s_theme_picker_role] = theme_picker_current_hex();
    s_theme_custom_set[s_theme_picker_role] = true;
    s_theme_using_custom[s_theme_picker_role] = true;
    theme_picker_update_preview();
    theme_update_swatches(s_theme_picker_role);
    theme_preview_apply();
}

static void theme_color_memory_update_ui(void)
{
    for (uint8_t i = 0; i < THEME_COLOR_MEMORY_COUNT; i++) {
        lv_obj_t *swatch = s_theme_picker_memory_swatches[i];
        if (!swatch) continue;
        bool valid = s_theme_color_memory_valid[i];
        uint32_t hex = valid ? (s_theme_color_memory[i] & 0xFFFFFFu) : 0x111625u;
        lv_obj_set_style_bg_color(swatch, lv_color_hex(hex), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(swatch, valid ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_color(swatch,
                                      valid ? theme_contrast_text_color(hex)
                                            : lv_color_hex(THEME_CONTROL_BORDER_HEX),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(swatch, valid ? 2 : 1, LV_PART_MAIN);
        if (s_theme_picker_memory_labels[i]) {
            lv_label_set_text(s_theme_picker_memory_labels[i], valid ? "" : LV_SYMBOL_PLUS);
            lv_obj_set_style_text_color(s_theme_picker_memory_labels[i], lv_color_hex(THEME_CONTROL_MUTED_HEX), LV_PART_MAIN);
        }
    }
}

static uint8_t theme_color_channel_delta(uint32_t a, uint32_t b, uint8_t shift)
{
    uint8_t av = (uint8_t)((a >> shift) & 0xFFu);
    uint8_t bv = (uint8_t)((b >> shift) & 0xFFu);
    return av > bv ? (uint8_t)(av - bv) : (uint8_t)(bv - av);
}

static bool theme_color_memory_same(uint32_t a, uint32_t b)
{
    a &= 0xFFFFFFu;
    b &= 0xFFFFFFu;
    if (a == b) return true;
    if (lv_color_to_u16(lv_color_hex(a)) == lv_color_to_u16(lv_color_hex(b))) return true;
    return theme_color_channel_delta(a, b, 16) <= 3 &&
           theme_color_channel_delta(a, b, 8) <= 3 &&
           theme_color_channel_delta(a, b, 0) <= 3;
}

static bool theme_color_memory_compact(void)
{
    uint32_t compact[THEME_COLOR_MEMORY_COUNT] = {0};
    bool valid[THEME_COLOR_MEMORY_COUNT] = {0};
    uint8_t count = 0;

    for (uint8_t i = 0; i < THEME_COLOR_MEMORY_COUNT; i++) {
        if (!s_theme_color_memory_valid[i]) continue;
        uint32_t hex = s_theme_color_memory[i] & 0xFFFFFFu;
        bool duplicate = false;
        for (uint8_t j = 0; j < count; j++) {
            if (theme_color_memory_same(compact[j], hex)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && count < THEME_COLOR_MEMORY_COUNT) {
            compact[count] = hex;
            valid[count] = true;
            count++;
        }
    }

    bool changed = false;
    for (uint8_t i = 0; i < THEME_COLOR_MEMORY_COUNT; i++) {
        uint32_t old_hex = s_theme_color_memory[i] & 0xFFFFFFu;
        if (s_theme_color_memory_valid[i] != valid[i] || (valid[i] && old_hex != compact[i])) {
            changed = true;
        }
        s_theme_color_memory[i] = compact[i];
        s_theme_color_memory_valid[i] = valid[i];
    }
    return changed;
}

static esp_err_t theme_color_memory_load(void)
{
    if (s_theme_color_memory_loaded) return ESP_OK;

    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE) s_theme_color_memory_loaded = true;
        return err;
    }

    memset(s_theme_color_memory, 0, sizeof(s_theme_color_memory));
    memset(s_theme_color_memory_valid, 0, sizeof(s_theme_color_memory_valid));
    s_theme_color_memory_dirty = false;

    FILE *file = fopen(THEME_COLOR_MEMORY_PATH, "r");
    if (!file) {
        if (errno == ENOENT) s_theme_color_memory_loaded = true;
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    char line[32];
    uint8_t count = 0;
    while (count < THEME_COLOR_MEMORY_COUNT && fgets(line, sizeof(line), file)) {
        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '#') cursor++;
        char *end = NULL;
        unsigned long value = strtoul(cursor, &end, 16);
        if (end == cursor) continue;
        s_theme_color_memory[count] = (uint32_t)value & 0xFFFFFFu;
        s_theme_color_memory_valid[count] = true;
        count++;
    }
    fclose(file);
    s_theme_color_memory_loaded = true;
    if (theme_color_memory_compact()) s_theme_color_memory_dirty = true;
    return ESP_OK;
}

static esp_err_t theme_color_memory_save(void)
{
    if (theme_color_memory_compact()) s_theme_color_memory_dirty = true;
    esp_err_t err = sd_storage_ensure_dir(THEME_COLOR_MEMORY_DIR);
    if (err != ESP_OK) return err;

    char data[THEME_COLOR_MEMORY_COUNT * 10 + 1];
    size_t len = 0;
    for (uint8_t i = 0; i < THEME_COLOR_MEMORY_COUNT; i++) {
        if (!s_theme_color_memory_valid[i]) continue;
        int written = snprintf(data + len, sizeof(data) - len, "#%06lX\n",
                               (unsigned long)(s_theme_color_memory[i] & 0xFFFFFFu));
        if (written < 0 || (size_t)written >= sizeof(data) - len) return ESP_ERR_INVALID_SIZE;
        len += (size_t)written;
    }

    err = sd_storage_write_text_atomic(THEME_COLOR_MEMORY_PATH, data, len);
    if (err == ESP_OK) s_theme_color_memory_dirty = false;
    return err;
}

static esp_err_t theme_color_memory_add(uint32_t hex)
{
    esp_err_t err = theme_color_memory_load();
    if (err != ESP_OK) return err;
    hex &= 0xFFFFFFu;
    if (theme_color_memory_compact()) s_theme_color_memory_dirty = true;

    for (uint8_t i = 0; i < THEME_COLOR_MEMORY_COUNT; i++) {
        if (s_theme_color_memory_valid[i] && theme_color_memory_same(s_theme_color_memory[i], hex)) {
            theme_color_memory_update_ui();
            return s_theme_color_memory_dirty ? theme_color_memory_save() : ESP_OK;
        }
    }

    for (int8_t i = (int8_t)(THEME_COLOR_MEMORY_COUNT - 1); i > 0; i--) {
        s_theme_color_memory[i] = s_theme_color_memory[i - 1];
        s_theme_color_memory_valid[i] = s_theme_color_memory_valid[i - 1];
    }
    s_theme_color_memory[0] = hex;
    s_theme_color_memory_valid[0] = true;
    s_theme_color_memory_loaded = true;
    s_theme_color_memory_dirty = true;
    theme_color_memory_update_ui();
    return theme_color_memory_save();
}

static void theme_color_memory_clicked(lv_event_t *e)
{
    uint8_t index = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (index >= THEME_COLOR_MEMORY_COUNT || !s_theme_color_memory_valid[index]) return;
    uint32_t hex = s_theme_color_memory[index] & 0xFFFFFFu;
    lv_color_hsv_t hsv = lv_color_rgb_to_hsv((uint8_t)((hex >> 16) & 0xFFu),
                                             (uint8_t)((hex >> 8) & 0xFFu),
                                             (uint8_t)(hex & 0xFFu));
    s_theme_picker_hue = hsv.h;
    s_theme_picker_sat = hsv.s;
    s_theme_picker_value = hsv.v;
    if (s_theme_picker_value_slider) lv_slider_set_value(s_theme_picker_value_slider, s_theme_picker_value, LV_ANIM_OFF);
    theme_picker_apply_live();
}

static void theme_picker_draw_wheel(void)
{
    if (!s_theme_picker_canvas) return;
    lv_color_t outside = lv_color_hex(THEME_CONTROL_SURFACE_HEX);
    const int32_t center = THEME_COLOR_WHEEL_CENTER;
    const int32_t radius = THEME_COLOR_WHEEL_RADIUS;
    const int32_t radius_sq = radius * radius;
    for (int32_t y = 0; y < THEME_COLOR_WHEEL_SIZE; y++) {
        for (int32_t x = 0; x < THEME_COLOR_WHEEL_SIZE; x++) {
            int32_t dx = x - center;
            int32_t dy = y - center;
            int32_t dist_sq = dx * dx + dy * dy;
            if (dist_sq > radius_sq) {
                lv_canvas_set_px(s_theme_picker_canvas, x, y, outside, LV_OPA_COVER);
                continue;
            }
            float dist = sqrtf((float)dist_sq);
            uint8_t sat = (uint8_t)((dist * 100.0f) / (float)radius);
            if (sat > 100) sat = 100;
            float angle = atan2f((float)dy, (float)dx) * THEME_RAD_TO_DEG;
            if (angle < 0.0f) angle += 360.0f;
            lv_color_t color = lv_color_hsv_to_rgb((uint16_t)angle, sat, 100);
            lv_canvas_set_px(s_theme_picker_canvas, x, y, color, LV_OPA_COVER);
        }
    }
}

static void theme_picker_close(bool keep)
{
    if (s_theme_picker_role < THEME_COLOR_COUNT && !keep) {
        s_theme_selected[s_theme_picker_role] = s_theme_picker_restore_selected;
        s_theme_custom_hex[s_theme_picker_role] = s_theme_picker_restore_custom_hex;
        s_theme_custom_set[s_theme_picker_role] = s_theme_picker_restore_custom_set;
        s_theme_using_custom[s_theme_picker_role] = s_theme_picker_restore_using_custom;
        theme_update_swatches(s_theme_picker_role);
        theme_preview_apply();
    }
    if (s_theme_picker_overlay) lv_obj_delete(s_theme_picker_overlay);
    s_theme_picker_overlay = NULL;
    s_theme_picker_canvas = NULL;
    s_theme_picker_marker = NULL;
    s_theme_picker_preview = NULL;
    s_theme_picker_hex_label = NULL;
    s_theme_picker_value_slider = NULL;
    s_theme_picker_value_label = NULL;
    memset(s_theme_picker_memory_swatches, 0, sizeof(s_theme_picker_memory_swatches));
    memset(s_theme_picker_memory_labels, 0, sizeof(s_theme_picker_memory_labels));
    free(s_theme_picker_buf);
    s_theme_picker_buf = NULL;
}

static void theme_picker_cancel_clicked(lv_event_t *e)
{
    (void)e;
    theme_picker_close(false);
}

static void theme_picker_use_clicked(lv_event_t *e)
{
    (void)e;
    esp_err_t err = theme_color_memory_add(theme_picker_current_hex());
    theme_picker_close(true);
    if (err != ESP_OK) toast_show("Color memory not saved");
}

static void theme_picker_overlay_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_theme_picker_overlay) theme_picker_close(false);
}

static void theme_picker_wheel_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !s_theme_picker_canvas) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t coords;
    lv_obj_get_coords(s_theme_picker_canvas, &coords);
    float dx = (float)(point.x - coords.x1 - THEME_COLOR_WHEEL_CENTER);
    float dy = (float)(point.y - coords.y1 - THEME_COLOR_WHEEL_CENTER);
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > (float)THEME_COLOR_WHEEL_RADIUS) dist = (float)THEME_COLOR_WHEEL_RADIUS;

    if (dist > 1.0f) {
        float angle = atan2f(dy, dx) * THEME_RAD_TO_DEG;
        if (angle < 0.0f) angle += 360.0f;
        if (angle >= 360.0f) angle -= 360.0f;
        s_theme_picker_hue = (uint16_t)angle;
    }
    s_theme_picker_sat = (uint8_t)((dist * 100.0f) / (float)THEME_COLOR_WHEEL_RADIUS);
    if (s_theme_picker_sat > 100) s_theme_picker_sat = 100;
    theme_picker_apply_live();
}

static void theme_picker_value_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    s_theme_picker_value = (uint8_t)value;
    theme_picker_apply_live();
}

static lv_obj_t *theme_picker_button_label(lv_obj_t *button, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_center(label);
    return label;
}

static void theme_custom_swatch_clicked(lv_event_t *e)
{
    theme_color_role_t role = (theme_color_role_t)(intptr_t)lv_event_get_user_data(e);
    if (role >= THEME_COLOR_COUNT) return;

    theme_picker_close(true);
    s_theme_picker_role = role;
    s_theme_picker_restore_selected = s_theme_selected[role];
    s_theme_picker_restore_custom_hex = s_theme_custom_hex[role];
    s_theme_picker_restore_custom_set = s_theme_custom_set[role];
    s_theme_picker_restore_using_custom = s_theme_using_custom[role];

    uint32_t start_hex = s_theme_custom_set[role] ? theme_custom_preview_hex(role) : theme_selected_hex(role);
    lv_color_hsv_t hsv = lv_color_rgb_to_hsv((uint8_t)((start_hex >> 16) & 0xFFu),
                                             (uint8_t)((start_hex >> 8) & 0xFFu),
                                             (uint8_t)(start_hex & 0xFFu));
    s_theme_picker_hue = hsv.h;
    s_theme_picker_sat = hsv.s;
    s_theme_picker_value = hsv.v;
    s_theme_custom_hex[role] = start_hex & 0xFFFFFFu;
    s_theme_custom_set[role] = true;
    s_theme_using_custom[role] = true;

    s_theme_picker_buf = heap_caps_malloc(THEME_COLOR_WHEEL_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_theme_picker_buf) s_theme_picker_buf = heap_caps_malloc(THEME_COLOR_WHEEL_BUF_SIZE, MALLOC_CAP_8BIT);
    if (!s_theme_picker_buf) {
        s_theme_selected[role] = s_theme_picker_restore_selected;
        s_theme_custom_hex[role] = s_theme_picker_restore_custom_hex;
        s_theme_custom_set[role] = s_theme_picker_restore_custom_set;
        s_theme_using_custom[role] = s_theme_picker_restore_using_custom;
        toast_show("No memory for picker");
        return;
    }

    s_theme_picker_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_theme_picker_overlay);
    lv_obj_set_size(s_theme_picker_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_theme_picker_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_theme_picker_overlay, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_flag(s_theme_picker_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_theme_picker_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_theme_picker_overlay, theme_picker_overlay_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(s_theme_picker_overlay);

    lv_obj_t *card = deep_card(s_theme_picker_overlay);
    lv_obj_set_width(card, 620);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(THEME_CONTROL_PANEL_HEX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(THEME_CONTROL_BORDER_HEX), LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

    char title_text[64];
    snprintf(title_text, sizeof(title_text), "%s color", s_theme_color_roles[role].label);
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_CONTROL_TEXT_HEX), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, THEME_FONT_TITLE, LV_PART_MAIN);

    lv_obj_t *body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body, 18, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *wheel_box = lv_obj_create(body);
    lv_obj_remove_style_all(wheel_box);
    lv_obj_set_size(wheel_box, THEME_COLOR_WHEEL_SIZE, THEME_COLOR_WHEEL_SIZE);
    lv_obj_clear_flag(wheel_box, LV_OBJ_FLAG_SCROLLABLE);

    s_theme_picker_canvas = lv_canvas_create(wheel_box);
    lv_canvas_set_buffer(s_theme_picker_canvas, s_theme_picker_buf,
                         THEME_COLOR_WHEEL_SIZE, THEME_COLOR_WHEEL_SIZE,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_theme_picker_canvas, THEME_COLOR_WHEEL_SIZE, THEME_COLOR_WHEEL_SIZE);
    lv_obj_add_flag(s_theme_picker_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_theme_picker_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_theme_picker_canvas, theme_picker_wheel_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_theme_picker_canvas, theme_picker_wheel_event, LV_EVENT_PRESSING, NULL);
    theme_picker_draw_wheel();

    s_theme_picker_marker = lv_obj_create(wheel_box);
    lv_obj_remove_style_all(s_theme_picker_marker);
    lv_obj_set_size(s_theme_picker_marker, 16, 16);
    lv_obj_set_style_radius(s_theme_picker_marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_theme_picker_marker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_theme_picker_marker, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_theme_picker_marker, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_outline_width(s_theme_picker_marker, 1, LV_PART_MAIN);
    lv_obj_set_style_outline_color(s_theme_picker_marker, lv_color_black(), LV_PART_MAIN);
    lv_obj_clear_flag(s_theme_picker_marker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *side = lv_obj_create(body);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, 300, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 10, LV_PART_MAIN);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    s_theme_picker_preview = lv_obj_create(side);
    lv_obj_remove_style_all(s_theme_picker_preview);
    lv_obj_set_size(s_theme_picker_preview, LV_PCT(100), 58);
    lv_obj_set_style_radius(s_theme_picker_preview, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_theme_picker_preview, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_theme_picker_preview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_theme_picker_preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_theme_picker_hex_label = lv_label_create(side);
    lv_obj_set_style_text_color(s_theme_picker_hex_label, lv_color_hex(THEME_CONTROL_MUTED_HEX), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_theme_picker_hex_label, THEME_FONT_BODY, LV_PART_MAIN);

    s_theme_picker_value_label = lv_label_create(side);
    lv_obj_set_style_text_color(s_theme_picker_value_label, lv_color_hex(THEME_CONTROL_MUTED_HEX), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_theme_picker_value_label, THEME_FONT_LABEL, LV_PART_MAIN);

    theme_color_memory_load();

    s_theme_picker_value_slider = lv_slider_create(side);
    lv_obj_set_size(s_theme_picker_value_slider, LV_PCT(100), 28);
    lv_slider_set_range(s_theme_picker_value_slider, 0, 100);
    lv_slider_set_value(s_theme_picker_value_slider, s_theme_picker_value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_theme_picker_value_slider, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_theme_picker_value_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_theme_picker_value_slider, lv_color_hex(THEME_CONTROL_BORDER_HEX), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_theme_picker_value_slider, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_theme_picker_value_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_theme_picker_value_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_theme_picker_value_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_theme_picker_value_slider, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(s_theme_picker_value_slider, theme_picker_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *memory_row = lv_obj_create(side);
    lv_obj_remove_style_all(memory_row);
    lv_obj_set_size(memory_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(memory_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(memory_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(memory_row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(memory_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    for (uint8_t i = 0; i < THEME_COLOR_MEMORY_COUNT; i++) {
        lv_obj_t *slot = lv_button_create(memory_row);
        lv_obj_set_size(slot, 62, 38);
        lv_obj_set_style_radius(slot, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(slot, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(slot, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(slot, lv_color_hex(THEME_CONTROL_BORDER_HEX), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slot, lv_color_hex(THEME_CONTROL_SURFACE_HEX), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slot, LV_OPA_40, LV_PART_MAIN);
        lv_obj_add_event_cb(slot, theme_color_memory_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(slot);
        lv_label_set_text(label, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_font(label, THEME_FONT_SMALL, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(THEME_CONTROL_MUTED_HEX), LV_PART_MAIN);
        lv_obj_center(label);
        s_theme_picker_memory_swatches[i] = slot;
        s_theme_picker_memory_labels[i] = label;
    }
    theme_color_memory_update_ui();

    lv_obj_t *buttons = settings_button_row(card);
    lv_obj_t *cancel = lv_button_create(buttons);
    lv_obj_set_height(cancel, 54);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_style_radius(cancel, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel, lv_color_hex(THEME_CONTROL_BORDER_HEX), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(THEME_CONTROL_SURFACE_HEX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, theme_picker_cancel_clicked, LV_EVENT_CLICKED, NULL);
    theme_picker_button_label(cancel, "Cancel", lv_color_hex(THEME_CONTROL_TEXT_HEX));

    lv_obj_t *use = lv_button_create(buttons);
    lv_obj_set_height(use, 54);
    lv_obj_set_flex_grow(use, 1);
    lv_obj_set_style_radius(use, 16, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(use, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(use, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(use, lv_color_hex(THEME_CONTROL_PRIMARY_HEX), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(use, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(use, theme_picker_use_clicked, LV_EVENT_CLICKED, NULL);
    theme_picker_button_label(use, "Use", lv_color_black());

    theme_picker_update_preview();
    theme_update_swatches(role);
    theme_preview_apply();
}

static void theme_dropdown_changed(lv_event_t *e)
{
    (void)e;
    app_theme_config_t cfg;
    theme_collect_controls(&cfg);
    const char *url = theme_background_url(cfg.background_preset, cfg.next_slot);
    snprintf(cfg.background_url, sizeof(cfg.background_url), "%s", url ? url : "");
    (void)app_config_theme_save(&cfg);
    settings_status_refresh(NULL);
}

static lv_obj_t *theme_icon_button(lv_obj_t *parent, const char *symbol,
                                   lv_event_cb_t cb, void *user_data,
                                   lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 62, 50);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(label);
    if (out_label) *out_label = label;
    return btn;
}

static void theme_bg_step_clicked(lv_event_t *e)
{
    settings_hide_keyboard();
    intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    esp_err_t err = theme_save_controls(false);
    if (err == ESP_OK) err = ui_background_step(dir < 0 ? -1 : 1);
    if (err == ESP_OK) toast_show(dir < 0 ? "Previous background" : "Next background");
    else toast_show(err == ESP_ERR_NOT_FOUND ? "No background images" : esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static void theme_slideshow_toggle_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();
    app_theme_config_t cfg;
    theme_collect_controls(&cfg);
    cfg.slideshow_enabled = !cfg.slideshow_enabled;
    esp_err_t err = app_config_theme_save(&cfg);
    if (err == ESP_OK) {
        theme_update_slideshow_button(cfg.slideshow_enabled);
        ui_background_refresh();
    }
    toast_show(err == ESP_OK ? (cfg.slideshow_enabled ? "Slideshow playing" : "Slideshow paused") : esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static void theme_download_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();

    services_status_t st;
    services_status_get(&st);
    if (!st.wifi_connected) {
        toast_show("Connect Wi-Fi first");
        return;
    }

    app_theme_config_t cfg;
    theme_collect_controls(&cfg);
    cfg.background_preset = theme_clamp_bg_preset(cfg.background_preset);
    esp_err_t err = app_config_theme_save(&cfg);
    if (err != ESP_OK) {
        toast_show(esp_err_to_name(err));
        return;
    }

    const bg_preset_t *preset = bg_preset_get(cfg.background_preset);
    err = ui_background_download_collection_start(preset->urls,
                                                  bg_preset_url_count(cfg.background_preset),
                                                  cfg.background_preset);
    if (err == ESP_OK) {
        toast_show("Downloading backgrounds");
    } else if (err == ESP_ERR_INVALID_STATE && ui_background_is_busy()) {
        toast_show("Download already running");
    } else {
        toast_show(esp_err_to_name(err));
    }
    settings_status_refresh(NULL);
}

static lv_obj_t *theme_background_picker(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *label = lv_label_create(col);
    lv_label_set_text(label, "Background image");
    lv_obj_set_style_text_color(label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_LABEL, LV_PART_MAIN);

    lv_obj_t *dropdown = lv_dropdown_create(col);
    lv_obj_set_size(dropdown, LV_PCT(100), 58);
    lv_dropdown_set_options(dropdown, bg_preset_dropdown_options());
    lv_dropdown_set_symbol(dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(dropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_bg_color(dropdown, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dropdown, 16, LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(dropdown, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(dropdown, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(dropdown, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(dropdown, theme_dropdown_changed, LV_EVENT_VALUE_CHANGED, NULL);
    return dropdown;
}

static void theme_switch_event(lv_event_t *e)
{
    (void)e;
    esp_err_t err = theme_save_controls(true);
    if (err != ESP_OK) toast_show(esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static void settings_switch_set_checked(lv_obj_t *sw, bool checked)
{
    if (!sw) return;
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(sw, LV_STATE_CHECKED);
}

static lv_obj_t *settings_switch_row(lv_obj_t *parent, const char *icon, const char *label_text,
                                     lv_event_cb_t event_cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(ic, THEME_FONT_LARGE, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_flex_grow(label, 1);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 88, 44);
    lv_obj_set_style_bg_color(sw, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, THEME_PRIMARY_COLOR, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, THEME_TEXT_PRIMARY, LV_PART_KNOB);
    if (event_cb) lv_obj_add_event_cb(sw, event_cb, LV_EVENT_VALUE_CHANGED, user_data);
    return sw;
}

static lv_obj_t *settings_dropdown_row(lv_obj_t *parent, const char *icon, const char *label_text,
                                       const char *options, lv_event_cb_t event_cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(ic, THEME_FONT_LARGE, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_flex_grow(label, 1);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_obj_set_size(dropdown, 180, 52);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_symbol(dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(dropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_bg_color(dropdown, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dropdown, 14, LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(dropdown, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(dropdown, 8, LV_PART_MAIN);
    if (event_cb) lv_obj_add_event_cb(dropdown, event_cb, LV_EVENT_VALUE_CHANGED, user_data);
    return dropdown;
}

static lv_obj_t *theme_switch_row(lv_obj_t *parent, const char *icon, const char *label_text)
{
    return settings_switch_row(parent, icon, label_text, theme_switch_event, NULL);
}

static lv_obj_t *theme_color_row(lv_obj_t *parent, theme_color_role_t role)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, s_theme_color_roles[role].label);
    lv_obj_set_width(label, 118);
    lv_obj_set_style_text_color(label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, THEME_FONT_LABEL, LV_PART_MAIN);
    s_theme_color_labels[role] = label;

    for (uint8_t i = 0; i < THEME_COLOR_CHOICES; i++) {
        lv_obj_t *swatch = lv_button_create(row);
        lv_obj_set_size(swatch, 52, 42);
        lv_obj_set_style_radius(swatch, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(s_theme_color_roles[role].choices[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(swatch, THEME_BORDER_COLOR, LV_PART_MAIN);
        lv_obj_set_style_border_width(swatch, 1, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(swatch, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(swatch, theme_swatch_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(((uint8_t)role << 8) | i));
        s_theme_swatches[role][i] = swatch;
    }

    lv_obj_t *custom = lv_button_create(row);
    lv_obj_set_size(custom, 52, 42);
    lv_obj_set_style_radius(custom, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(custom, lv_color_hex(theme_custom_preview_hex(role)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(custom, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(custom, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(custom, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(custom, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(custom, theme_custom_swatch_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)role);
    lv_obj_t *icon = lv_label_create(custom);
    lv_label_set_text(icon, LV_SYMBOL_TINT);
    lv_obj_set_style_text_font(icon, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_center(icon);
    s_theme_custom_swatches[role] = custom;
    s_theme_custom_labels[role] = icon;

    theme_update_swatches(role);
    return row;
}

static void theme_settings_load_fields(void)
{
    app_theme_config_t cfg;
    if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);
    if (s_theme_bg_dropdown) lv_dropdown_set_selected(s_theme_bg_dropdown, theme_clamp_bg_preset(cfg.background_preset));
    settings_switch_set_checked(s_theme_background_enabled_switch, cfg.background_enabled);
    settings_switch_set_checked(s_theme_idle_only_switch, cfg.background_idle_only);
    if (s_theme_opacity_slider) lv_slider_set_value(s_theme_opacity_slider, cfg.surface_opacity_pct, LV_ANIM_OFF);
    if (s_theme_shadows_sw) {
        if (cfg.shadows_enabled) lv_obj_add_state(s_theme_shadows_sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_theme_shadows_sw, LV_STATE_CHECKED);
    }
    if (s_theme_dim_slider) lv_slider_set_value(s_theme_dim_slider, cfg.background_dim_pct, LV_ANIM_OFF);
    if (s_theme_slideshow_slider) lv_slider_set_value(s_theme_slideshow_slider, cfg.slideshow_seconds, LV_ANIM_OFF);
    theme_update_slideshow_button(cfg.slideshow_enabled);
    theme_load_role_hex(THEME_COLOR_BACKGROUND, cfg.bg_color_hex);
    theme_load_role_hex(THEME_COLOR_SURFACE, cfg.surface_color_hex);
    theme_load_role_hex(THEME_COLOR_CARD, cfg.card_color_hex);
    theme_load_role_hex(THEME_COLOR_BORDER, cfg.border_color_hex);
    theme_load_role_hex(THEME_COLOR_PRIMARY, cfg.primary_color_hex);
    theme_load_role_hex(THEME_COLOR_ACCENT, cfg.accent_color_hex);
    settings_apply_theme_readability();
    format_pct_label(s_theme_opacity_label, "Panels", cfg.surface_opacity_pct);
    format_pct_label(s_theme_dim_label, "Dim", cfg.background_dim_pct);
    format_seconds_label(s_theme_slideshow_label, "Slide", cfg.slideshow_seconds);
}

static esp_err_t theme_save_controls(bool refresh_background)
{
    app_theme_config_t cfg;
    theme_collect_controls(&cfg);
    esp_err_t err = app_config_theme_save(&cfg);
    if (err == ESP_OK) {
        theme_apply_config(&cfg);
        theme_recolor_tree(lv_screen_active());
        theme_apply_surface_opacity(lv_screen_active());
        idle_apply_theme();
        theme_apply_shadows(lv_screen_active());
        settings_apply_theme_readability();
        if (refresh_background) ui_background_refresh();
    }
    return err;
}

static void theme_save_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();
    esp_err_t err = theme_save_controls(true);
    toast_show(err == ESP_OK ? "Theme saved" : esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static void theme_clear_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();
    esp_err_t err = ui_background_clear_images();
    theme_settings_load_fields();
    app_theme_config_t cfg;
    if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);
    theme_apply_config(&cfg);
    theme_recolor_tree(lv_screen_active());
    theme_apply_surface_opacity(lv_screen_active());
    idle_apply_theme();
    theme_apply_shadows(lv_screen_active());
    settings_apply_theme_readability();
    toast_show(err == ESP_OK ? "Theme cleared" : esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static void format_coord_x1e6(char *out, size_t out_len, int32_t value)
{
    bool neg = value < 0;
    int32_t abs_value = neg ? -value : value;
    snprintf(out, out_len, "%s%ld.%06ld", neg ? "-" : "",
             (long)(abs_value / 1000000), (long)(abs_value % 1000000));
}

static bool weather_status_is_inflight(const char *detail)
{
    return detail && (!strncmp(detail, "Fetching", 8) || !strcmp(detail, "Weather queued"));
}

static void idle_settings_load_fields(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    settings_switch_set_checked(s_idle_dismiss_lights_switch, cfg.idle_dismiss_lights_on);
    settings_switch_set_checked(s_idle_swipe_wake_lights_switch, cfg.idle_swipe_wake_lights_on);
    settings_switch_set_checked(s_idle_wake_timer_switch, cfg.idle_dismiss_lights_timer_on);
}

static void lights_settings_load_fields(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    settings_switch_set_checked(s_light_safety_switch, cfg.light_safety_auto_off_enabled);
}

static void display_settings_load_fields(void)
{
    settings_switch_set_checked(s_auto_brightness_switch, backlight_manager_is_enabled());
}

static void weather_settings_load_fields(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    if (s_weather_bottom_mode_dropdown) lv_dropdown_set_selected(s_weather_bottom_mode_dropdown, cfg.weather_bottom_panel_mode);
}

static void system_settings_load_fields(void)
{
    app_tuning_config_t cfg = load_tuning_config();
    settings_switch_set_checked(s_system_cpu_load_switch, cfg.system_cpu_load_enabled);
}

static void settings_load_fields(void)
{
    app_weather_config_t weather;
    if (app_config_weather_load(&weather) == ESP_OK && weather.configured) {
        if (s_weather_key_ta) lv_textarea_set_text(s_weather_key_ta, weather.api_key);
    }
    lights_settings_load_fields();
    weather_settings_load_fields();
    theme_settings_load_fields();
    display_settings_load_fields();
    idle_settings_load_fields();
    system_settings_load_fields();
    settings_apply_theme_readability();
}

static void settings_status_refresh(lv_timer_t *t)
{
    if (timer_page_hidden(t, s_settings_root)) return;
    services_status_t st;
    services_status_get(&st);

    char wifi_text[96];
    if (st.wifi_connected) {
        snprintf(wifi_text, sizeof(wifi_text), "Connected%s%s",
                 st.ip_addr[0] && strcmp(st.ip_addr, "-") ? "  " : "",
                 st.ip_addr[0] && strcmp(st.ip_addr, "-") ? st.ip_addr : "");
    } else if (!st.wifi_supported) {
        snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi driver disabled");
    } else if (st.wifi_configured) {
        snprintf(wifi_text, sizeof(wifi_text), "%s", st.wifi_detail[0] ? st.wifi_detail : "Connecting");
    } else {
        snprintf(wifi_text, sizeof(wifi_text), "Not configured");
    }

    label_set_text_if_changed(s_settings_wifi_status, wifi_text);
    if (s_home_wifi_summary) {
        char summary[100];
        snprintf(summary, sizeof(summary), "%s%s%s",
                 st.wifi_ssid[0] ? st.wifi_ssid : "No saved network",
                 st.wifi_configured ? "  " : "",
                 st.wifi_configured ? wifi_text : "");
        label_set_text_if_changed(s_home_wifi_summary, summary);
    }

    char wled_buf[96];
    const char *wled_text = "Waiting for WLED response";
    if (s_settings_wled_status) {
        if (!st.rs485_ready) {
            wled_text = "RS-485 offline";
        } else if (st.wled_online) {
            wled_state_t ws;
            wled_state_get(&ws);
            if (ws.valid && ws.version[0]) {
                snprintf(wled_buf, sizeof(wled_buf), "WLED %s  %u LEDs",
                         ws.version, ws.led_count);
                wled_text = wled_buf;
            } else {
                wled_text = "WLED online";
            }
        }
        label_set_text_if_changed(s_settings_wled_status, wled_text);
    }
    label_set_text_if_changed(s_home_wled_summary, wled_text);

    const char *weather_text = "Not configured";
    if (s_settings_weather_status) {
        if (st.weather_online) weather_text = "Fresh";
        else if (st.weather_configured && weather_status_is_inflight(st.weather_detail) && st.weather_status_ms &&
                 (uint32_t)((esp_timer_get_time() / 1000ULL) - st.weather_status_ms) > 35000u) {
            weather_text = "Weather timed out";
        } else weather_text = st.weather_detail[0] ? st.weather_detail : "Not configured";
        label_set_text_if_changed(s_settings_weather_status, weather_text);
    }
    label_set_text_if_changed(s_home_weather_summary, weather_text);
    if (s_weather_location_label) {
        char location_text[160];
        if (st.location_has_coords) {
            char lat[20];
            char lon[20];
            format_coord_x1e6(lat, sizeof(lat), st.location_lat_x1e6);
            format_coord_x1e6(lon, sizeof(lon), st.location_lon_x1e6);
            snprintf(location_text, sizeof(location_text), "Using IP location: %s\n%s  %s, %s",
                     st.location_area[0] ? st.location_area : "Unknown",
                     st.timezone[0] ? st.timezone : "UTC",
                     lat, lon);
        } else if (st.location_ready) {
            snprintf(location_text, sizeof(location_text), "Using IP location: %s\nWaiting for coordinates",
                     st.location_area[0] ? st.location_area : "Unknown");
        } else if (st.wifi_connected) {
            snprintf(location_text, sizeof(location_text), "Checking IP location");
        } else {
            snprintf(location_text, sizeof(location_text), "Connect Wi-Fi to detect location");
        }
        label_set_text_if_changed(s_weather_location_label, location_text);
    }

    if (s_settings_theme_status || s_home_theme_summary) {
        app_theme_config_t theme_cfg;
        app_config_theme_load(&theme_cfg);
        char theme_text[96];
        uint8_t preset = theme_clamp_bg_preset(theme_cfg.background_preset);
        if (s_theme_bg_dropdown) preset = theme_clamp_bg_preset((uint8_t)lv_dropdown_get_selected(s_theme_bg_dropdown));
        const char *preset_label = bg_preset_get(preset)->label;
        uint8_t present = 0;
        uint8_t total = 0;
        bool preset_ready = ui_background_preset_images_present(preset, &present, &total);
        ui_background_download_state_t bg_state;
        ui_background_download_state_get(&bg_state);

        if (bg_state.busy) {
            snprintf(theme_text, sizeof(theme_text), "%s", bg_state.status);
        } else if (!theme_cfg.background_enabled) {
            snprintf(theme_text, sizeof(theme_text), "%s  images off  %u%% panels",
                     preset_label, theme_cfg.surface_opacity_pct);
        } else if (!preset_ready) {
            snprintf(theme_text, sizeof(theme_text), "%s  %u/%u images ready",
                     preset_label, (unsigned)present, (unsigned)total);
        } else if (theme_cfg.background_enabled && theme_cfg.image_count > 0) {
            snprintf(theme_text, sizeof(theme_text), "%s  %u image%s  %s  %s  %u%% panels",
                     preset_label,
                     theme_cfg.image_count,
                     theme_cfg.image_count == 1 ? "" : "s",
                     theme_cfg.slideshow_enabled ? "playing" : "paused",
                     theme_cfg.background_idle_only ? "idle-only" : "all screens",
                     theme_cfg.surface_opacity_pct);
        } else {
            snprintf(theme_text, sizeof(theme_text), "%s  solid  %u%% panels",
                     preset_label, theme_cfg.surface_opacity_pct);
        }
        label_set_text_if_changed(s_settings_theme_status, theme_text);
        label_set_text_if_changed(s_home_theme_summary, theme_text);
        theme_update_slideshow_button(theme_cfg.slideshow_enabled);

        if (s_theme_download_row) {
            if (!bg_state.busy && !preset_ready) lv_obj_clear_flag(s_theme_download_row, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_theme_download_row, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_theme_download_btn) {
            if (bg_state.busy) lv_obj_add_state(s_theme_download_btn, LV_STATE_DISABLED);
            else lv_obj_clear_state(s_theme_download_btn, LV_STATE_DISABLED);
        }
        if (s_theme_progress_panel) {
            if (bg_state.busy) {
                lv_obj_clear_flag(s_theme_progress_panel, LV_OBJ_FLAG_HIDDEN);
                label_set_text_if_changed(s_theme_progress_label, bg_state.status);
                if (s_theme_progress_bar) {
                    lv_bar_set_value(s_theme_progress_bar,
                                     bg_state.has_progress ? bg_state.progress_pct : 0,
                                     LV_ANIM_ON);
                }
            } else {
                lv_obj_add_flag(s_theme_progress_panel, LV_OBJ_FLAG_HIDDEN);
                if (s_theme_progress_bar) lv_bar_set_value(s_theme_progress_bar, 0, LV_ANIM_OFF);
            }
        }
    }

    if (s_home_sd_summary) {
        const char *sd_text = s_settings_sd_status ? lv_label_get_text(s_settings_sd_status) : NULL;
        label_set_text_if_changed(s_home_sd_summary, sd_text && sd_text[0] && strcmp(sd_text, "-") != 0 ?
                                  sd_text : "Browse /sdcard");
    }

    if (s_settings_display_status) {
        bool auto_enabled = backlight_manager_is_enabled();
        const char *auto_text = auto_enabled ? "Auto-brightness ON  daylight curve active" : "Auto-brightness OFF  manual backlight";
        label_set_text_if_changed(s_settings_display_status, auto_text);
        lv_obj_set_style_text_color(s_settings_display_status,
                                    auto_enabled ? THEME_PRIMARY_COLOR : THEME_TEXT_MUTED,
                                    LV_PART_MAIN);
    }
    settings_switch_set_checked(s_auto_brightness_switch, backlight_manager_is_enabled());

    if (s_settings_audio_status) {
        char audio_text[128];
        snprintf(audio_text, sizeof(audio_text), "Mic: %s  FFT: %s  Sync: %s",
                 st.audio_ready ? "OK" : "off",
                 st.fft_ready ? "OK" : "off",
                 st.sound_sync_ready ? "TX" : "off");
        label_set_text_if_changed(s_settings_audio_status, audio_text);
    }

    if (s_info_panel_root && !lv_obj_has_flag(s_info_panel_root, LV_OBJ_FLAG_HIDDEN)) info_refresh(NULL);
    settings_apply_theme_readability();
}

static void wled_reboot_clicked(lv_event_t *e)
{
    (void)e;
    esp_err_t err = cmd_tx_send_json("{\"rb\":true}");
    toast_show(err == ESP_OK ? "Rebooting WLED" : "RS-485 not ready");
}

static void wled_provision_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();
    app_wifi_config_t wifi;
    if (app_config_wifi_load(&wifi) != ESP_OK || !wifi.configured) {
        toast_show("Save Wi-Fi first");
        return;
    }
    esp_err_t err = provision_init();
    toast_show(err == ESP_OK ? "Provisioning WLED" : esp_err_to_name(err));
    settings_status_refresh(NULL);
}

static void system_reboot_clicked(lv_event_t *e)
{
    (void)e;
    toast_show("Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void weather_save_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();
    const char *key = s_weather_key_ta ? lv_textarea_get_text(s_weather_key_ta) : "";
    if (!key || !key[0]) {
        toast_show("Weather key required");
        return;
    }

    esp_err_t err = app_config_weather_save(key);
    if (err != ESP_OK) {
        toast_show(esp_err_to_name(err));
        return;
    }

    services_status_t st;
    services_status_get(&st);
    if (st.wifi_connected && !st.location_has_coords) (void)services_location_request_refresh();
    const char *detail = st.wifi_connected ?
                         (st.location_has_coords ? "Weather queued" : "Waiting for IP location") :
                         "Waiting for Wi-Fi";
    services_status_note_weather(true, false, detail);
    err = weather_api_request_refresh();
    if (err != ESP_OK) {
        services_status_note_weather(true, false, "Weather task failed");
        toast_show(esp_err_to_name(err));
        settings_status_refresh(NULL);
        return;
    }
    toast_show(st.location_has_coords ? "Weather saved; queued" : "Weather saved; locating");
    settings_status_refresh(NULL);
}

static void weather_clear_clicked(lv_event_t *e)
{
    (void)e;
    settings_hide_keyboard();
    esp_err_t err = app_config_weather_clear();
    if (err != ESP_OK) {
        toast_show(esp_err_to_name(err));
        return;
    }
    if (s_weather_key_ta) lv_textarea_set_text(s_weather_key_ta, "");
    (void)weather_state_clear();
    services_status_note_weather(false, false, "Not configured");
    (void)weather_api_request_refresh();
    toast_show("Weather cleared");
    settings_status_refresh(NULL);
}

lv_obj_t *screen_settings_create(lv_obj_t *parent)
{
    lv_obj_t *p = page_root(parent);
    s_settings_root = p;
    settings_floating_header_create(p);

    s_settings_home = settings_panel_create(p);
    s_settings_lights_page = settings_panel_create(p);
    s_settings_wifi_page = settings_panel_create(p);
    s_settings_wled_page = settings_panel_create(p);
    s_settings_weather_page = settings_panel_create(p);
    s_settings_theme_page = settings_panel_create(p);
    s_settings_sd_page = settings_panel_create(p);
    s_settings_display_page = settings_panel_create(p);
    s_settings_idle_page = settings_panel_create(p);
    s_settings_audio_page = settings_panel_create(p);
    s_settings_timer_page = settings_panel_create(p);
    s_settings_system_page = settings_panel_create(p);
    s_settings_about_page = settings_panel_create(p);

    settings_section_title(s_settings_home, "Settings");
    settings_menu_row(s_settings_home, UI_ICON_WIFI, "Wi-Fi", "Scan and connect",
                      &s_home_wifi_summary, settings_wifi_open, NULL);
    settings_menu_row(s_settings_home, UI_ICON_LIGHTS, "Lights", "Kelvin range, safety",
                      NULL, settings_panel_clicked, s_settings_lights_page);
    settings_menu_row(s_settings_home, UI_ICON_DISPLAY, "Display", "Backlight, timeout, auto-brightness",
                      NULL, settings_panel_clicked, s_settings_display_page);
    settings_menu_row(s_settings_home, UI_ICON_IDLE, "Idle Screen", "Weather clock, gestures",
                      NULL, settings_panel_clicked, s_settings_idle_page);
    settings_menu_row(s_settings_home, UI_ICON_THEME, "Theme", "Backgrounds and glass",
                      &s_home_theme_summary, settings_panel_clicked, s_settings_theme_page);
    settings_menu_row(s_settings_home, UI_ICON_SD, "SD Card", "Browse files",
                      &s_home_sd_summary, settings_sd_open, NULL);
    settings_menu_row(s_settings_home, UI_ICON_WLED, "WLED", "RS-485 provisioning",
                      &s_home_wled_summary, settings_panel_clicked, s_settings_wled_page);
    settings_menu_row(s_settings_home, UI_ICON_WEATHER, "Weather", "API key, IP location, idle display",
                      &s_home_weather_summary, settings_panel_clicked, s_settings_weather_page);
    settings_menu_row(s_settings_home, UI_ICON_AUDIO, "Audio", "Mic, FFT, Sound Sync",
                      NULL, settings_panel_clicked, s_settings_audio_page);
    settings_menu_row(s_settings_home, UI_ICON_TIMER, "Timer", "Alarms, repeat, presets",
                      NULL, settings_panel_clicked, s_settings_timer_page);
    settings_menu_row(s_settings_home, UI_ICON_SETTINGS, "System", "Diagnostics and reboot",
                      NULL, settings_panel_clicked, s_settings_system_page);
    settings_menu_row(s_settings_home, UI_ICON_INFO, "About", "Firmware and licenses",
                      NULL, settings_panel_clicked, s_settings_about_page);

    settings_header(s_settings_lights_page, "Lights");
    lv_obj_t *lights_card = deep_card(s_settings_lights_page);
    lv_obj_set_size(lights_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lights_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lights_card, 0, LV_PART_MAIN);
    s_stepper_count = 0;
    add_stepper_row(lights_card, LV_SYMBOL_EYE_OPEN, "Min Kelvin",
                    100, 1500, 5000, " K", ap_kmin, rd_kmin);
    add_stepper_row(lights_card, LV_SYMBOL_EYE_OPEN, "Max Kelvin",
                    100, 3000, 10000, " K", ap_kmax, rd_kmax);
    s_light_safety_switch = settings_switch_row(lights_card, LV_SYMBOL_WARNING, "Safety Auto-Off",
                                                light_safety_switch_event, NULL);
    add_stepper_row(lights_card, LV_SYMBOL_REFRESH, "Max On Time",
                    1, 1, 24, " h", ap_light_safety_hours, rd_light_safety_hours);

    settings_header(s_settings_wifi_page, "Wi-Fi");
    lv_obj_t *net = deep_card(s_settings_wifi_page);
    lv_obj_set_size(net, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(net, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(net, 14, LV_PART_MAIN);
    s_settings_wifi_status = settings_status_label(net);
    s_wifi_scan_btn = NULL;

    s_wifi_scan_status = settings_status_label(net);
    lv_label_set_text(s_wifi_scan_status, "Opening Wi-Fi");

    s_wifi_list = lv_obj_create(net);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_size(s_wifi_list, LV_PCT(100), 300);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 10, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(s_wifi_list, wifi_list_scroll_event, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_wifi_list, wifi_list_scroll_event, LV_EVENT_SCROLL_END, NULL);

    lv_obj_t *initial = lv_label_create(s_wifi_list);
    lv_label_set_text(initial, "Opening Wi-Fi");
    lv_obj_set_style_text_color(initial, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(initial, THEME_FONT_BODY, LV_PART_MAIN);

    settings_header(s_settings_wled_page, "WLED");
    lv_obj_t *wled_qr_card = deep_card(s_settings_wled_page);
    lv_obj_set_size(wled_qr_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wled_qr_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wled_qr_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wled_qr_card, 18, LV_PART_MAIN);

#if LV_USE_QRCODE
    lv_obj_t *qr = wled_qr_grid_create(wled_qr_card, WLED_WEB_URL);
    if (!qr) qr = wled_qr_placeholder_create(wled_qr_card);
#else
    lv_obj_t *qr = wled_qr_placeholder_create(wled_qr_card);
#endif
    (void)qr;

    lv_obj_t *qr_text = lv_obj_create(wled_qr_card);
    lv_obj_remove_style_all(qr_text);
    lv_obj_set_flex_grow(qr_text, 1);
    lv_obj_set_width(qr_text, 0);
    lv_obj_set_flex_flow(qr_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(qr_text, 6, LV_PART_MAIN);
    lv_obj_clear_flag(qr_text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *qr_title = lv_label_create(qr_text);
    lv_label_set_text(qr_title, "Open WLED");
    lv_obj_set_style_text_color(qr_title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(qr_title, THEME_FONT_BODY, LV_PART_MAIN);

    lv_obj_t *qr_url = lv_label_create(qr_text);
    lv_label_set_text(qr_url, WLED_WEB_URL);
    lv_obj_set_width(qr_url, LV_PCT(100));
    lv_label_set_long_mode(qr_url, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(qr_url, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(qr_url, THEME_FONT_SMALL, LV_PART_MAIN);

    lv_obj_t *wled = deep_card(s_settings_wled_page);
    lv_obj_set_size(wled, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wled, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wled, 14, LV_PART_MAIN);
    s_settings_wled_status = settings_status_label(wled);
    lv_obj_t *wled_btns = settings_button_row(wled);
    settings_button(wled_btns, "Provision", true, wled_provision_clicked);
    settings_button(wled_btns, "Reboot S3", false, wled_reboot_clicked);

    lv_obj_t *wled_timing = deep_card(s_settings_wled_page);
    lv_obj_set_size(wled_timing, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wled_timing, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wled_timing, 0, LV_PART_MAIN);
    lv_obj_t *wled_timing_title = lv_label_create(wled_timing);
    lv_label_set_text(wled_timing_title, "Link Timing");
    lv_obj_set_style_text_color(wled_timing_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(wled_timing_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wled_timing_title, 8, LV_PART_MAIN);
    add_stepper_row(wled_timing, LV_SYMBOL_REFRESH, "Health Poll",
                    1, 2, 30, " s", ap_wled_poll, rd_wled_poll);
    add_stepper_row(wled_timing, LV_SYMBOL_WIFI, "Offline After",
                    5, 10, 120, " s", ap_wled_stale, rd_wled_stale);
    add_stepper_row(wled_timing, LV_SYMBOL_REFRESH, "Hue Updates",
                    1, 1, 5, " /s", ap_wled_hue_update, rd_wled_hue_update);

    settings_header(s_settings_theme_page, "Theme");
    lv_obj_t *theme_card = deep_card(s_settings_theme_page);
    lv_obj_set_size(theme_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(theme_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(theme_card, 16, LV_PART_MAIN);
    s_settings_theme_status = settings_status_label(theme_card);
    s_theme_bg_dropdown = theme_background_picker(theme_card);
    s_theme_background_enabled_switch = theme_switch_row(theme_card, LV_SYMBOL_IMAGE, "Show background images");
    s_theme_idle_only_switch = theme_switch_row(theme_card, LV_SYMBOL_HOME, "Idle weather only");

    s_theme_download_row = settings_button_row(theme_card);
    s_theme_download_btn = settings_button(s_theme_download_row, LV_SYMBOL_DOWNLOAD " Download backgrounds",
                                           true, theme_download_clicked);
    lv_obj_add_flag(s_theme_download_row, LV_OBJ_FLAG_HIDDEN);

    s_theme_progress_panel = lv_obj_create(theme_card);
    lv_obj_remove_style_all(s_theme_progress_panel);
    lv_obj_set_size(s_theme_progress_panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_theme_progress_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_theme_progress_panel, 8, LV_PART_MAIN);
    lv_obj_clear_flag(s_theme_progress_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(s_theme_progress_panel, LV_OBJ_FLAG_HIDDEN);

    s_theme_progress_label = lv_label_create(s_theme_progress_panel);
    lv_label_set_text(s_theme_progress_label, "Downloading backgrounds");
    lv_obj_set_width(s_theme_progress_label, LV_PCT(100));
    lv_label_set_long_mode(s_theme_progress_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_theme_progress_label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_theme_progress_label, THEME_FONT_LABEL, LV_PART_MAIN);

    s_theme_progress_bar = lv_bar_create(s_theme_progress_panel);
    lv_obj_set_size(s_theme_progress_bar, LV_PCT(100), 16);
    lv_bar_set_range(s_theme_progress_bar, 0, 100);
    lv_bar_set_value(s_theme_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_theme_progress_bar, lv_color_hex(0x070B14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_theme_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_theme_progress_bar, THEME_PRIMARY_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_theme_progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_theme_progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    s_theme_opacity_slider = theme_slider_row(theme_card, "Panel opacity", 35, 100, 100,
                                              &s_theme_opacity_label);
    s_theme_shadows_sw = theme_switch_row(theme_card, LV_SYMBOL_EYE_OPEN, "Panel shadows");
    s_theme_dim_slider = theme_slider_row(theme_card, "Background dim", 0, 95, 58,
                                          &s_theme_dim_label);
    s_theme_slideshow_slider = theme_slider_row(theme_card, "Slideshow seconds", 5, 180, 25,
                                                &s_theme_slideshow_label);

    lv_obj_t *bg_controls = lv_obj_create(theme_card);
    lv_obj_remove_style_all(bg_controls);
    lv_obj_set_size(bg_controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bg_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bg_controls, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bg_controls, 10, LV_PART_MAIN);
    lv_obj_clear_flag(bg_controls, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    theme_icon_button(bg_controls, LV_SYMBOL_PREV, theme_bg_step_clicked, (void *)(intptr_t)-1, NULL);
    theme_icon_button(bg_controls, LV_SYMBOL_PAUSE, theme_slideshow_toggle_clicked, NULL, &s_theme_slideshow_play_label);
    theme_icon_button(bg_controls, LV_SYMBOL_NEXT, theme_bg_step_clicked, (void *)(intptr_t)1, NULL);

    lv_obj_t *theme_btns = settings_button_row(theme_card);
    settings_button(theme_btns, "Save", true, theme_save_clicked);
    settings_button(theme_btns, "Reset", false, theme_clear_clicked);

    lv_obj_t *palette_card = deep_card(s_settings_theme_page);
    s_theme_palette_card = palette_card;
    lv_obj_set_size(palette_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(palette_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(palette_card, 12, LV_PART_MAIN);
    lv_obj_t *palette_title = lv_label_create(palette_card);
    s_theme_palette_title = palette_title;
    lv_label_set_text(palette_title, "Palette");
    lv_obj_set_style_text_color(palette_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(palette_title, THEME_FONT_LABEL, LV_PART_MAIN);
    for (theme_color_role_t role = 0; role < THEME_COLOR_COUNT; role++) {
        theme_color_row(palette_card, role);
    }
    theme_palette_editor_apply_chrome();

    settings_header(s_settings_sd_page, "SD Card");
    lv_obj_t *sd_card = deep_card(s_settings_sd_page);
    lv_obj_set_size(sd_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sd_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sd_card, 12, LV_PART_MAIN);
    s_settings_sd_status = settings_status_label(sd_card);
    lv_label_set_text(s_settings_sd_status, "Browse /sdcard");

    /* Toolbar: back | home | path | new | refresh */
    lv_obj_t *sd_toolbar = lv_obj_create(sd_card);
    lv_obj_remove_style_all(sd_toolbar);
    lv_obj_set_size(sd_toolbar, LV_PCT(100), 46);
    lv_obj_set_flex_flow(sd_toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sd_toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sd_toolbar, 6, LV_PART_MAIN);
    lv_obj_clear_flag(sd_toolbar, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button (←) */
    lv_obj_t *back_btn = lv_button_create(sd_toolbar);
    lv_obj_set_size(back_btn, 46, 42);
    lv_obj_set_style_radius(back_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, sd_explorer_up_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(back_lbl);

    /* Home/root button */
    lv_obj_t *home_btn = lv_button_create(sd_toolbar);
    lv_obj_set_size(home_btn, 46, 42);
    lv_obj_set_style_radius(home_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(home_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(home_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(home_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(home_btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_add_event_cb(home_btn, sd_explorer_root_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_lbl = lv_label_create(home_btn);
    lv_label_set_text(home_lbl, LV_SYMBOL_HOME);
    lv_obj_set_style_text_font(home_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(home_lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(home_lbl);

    /* Path breadcrumb (fills remaining space) */
    s_sd_path_label = lv_label_create(sd_toolbar);
    lv_label_set_text(s_sd_path_label, "/");
    lv_obj_set_flex_grow(s_sd_path_label, 1);
    lv_label_set_long_mode(s_sd_path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_sd_path_label, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_sd_path_label, THEME_FONT_LABEL, LV_PART_MAIN);

    /* New Folder button */
    lv_obj_t *new_btn = lv_button_create(sd_toolbar);
    lv_obj_set_size(new_btn, 46, 42);
    lv_obj_set_style_radius(new_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(new_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(new_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(new_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(new_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(new_btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_add_event_cb(new_btn, sd_explorer_newfolder_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *new_lbl = lv_label_create(new_btn);
    lv_label_set_text(new_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(new_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(new_lbl, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_center(new_lbl);

    /* Refresh button */
    lv_obj_t *refresh_btn = lv_button_create(sd_toolbar);
    lv_obj_set_size(refresh_btn, 46, 42);
    lv_obj_set_style_radius(refresh_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(refresh_btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(refresh_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(refresh_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(refresh_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(refresh_btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_add_event_cb(refresh_btn, sd_explorer_refresh_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(refresh_lbl, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(refresh_lbl, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_center(refresh_lbl);

    s_sd_list = lv_obj_create(sd_card);
    lv_obj_remove_style_all(s_sd_list);
    lv_obj_set_size(s_sd_list, LV_PCT(100), 390);
    lv_obj_set_flex_flow(s_sd_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sd_list, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_sd_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_sd_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_sd_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_event_cb(s_sd_list, pull_refresh_scroll_end, LV_EVENT_SCROLL_END, (void *)sd_explorer_refresh);
    sd_explorer_message("Open SD Card to browse files");

    settings_header(s_settings_weather_page, "Weather");
    lv_obj_t *wx = deep_card(s_settings_weather_page);
    lv_obj_set_size(wx, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wx, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wx, 14, LV_PART_MAIN);
    s_settings_weather_status = settings_status_label(wx);
    s_weather_key_ta = settings_text_input(wx, "OpenWeather key", "API key", 95, true, NULL,
                                           LV_KEYBOARD_MODE_TEXT_LOWER);
    s_weather_location_label = settings_status_label(wx);
    lv_label_set_long_mode(s_weather_location_label, LV_LABEL_LONG_WRAP);
    lv_obj_t *wx_btns = settings_button_row(wx);
    settings_button(wx_btns, "Save", true, weather_save_clicked);
    settings_button(wx_btns, "Clear", false, weather_clear_clicked);

    lv_obj_t *wx_timing = deep_card(s_settings_weather_page);
    lv_obj_set_size(wx_timing, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wx_timing, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wx_timing, 0, LV_PART_MAIN);
    lv_obj_t *wx_timing_title = lv_label_create(wx_timing);
    lv_label_set_text(wx_timing_title, "Weather Timing");
    lv_obj_set_style_text_color(wx_timing_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(wx_timing_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wx_timing_title, 8, LV_PART_MAIN);
    add_stepper_row(wx_timing, LV_SYMBOL_REFRESH, "Data Refresh",
                    5, 5, 120, " min", ap_wx_refresh, rd_wx_refresh);
    add_stepper_row(wx_timing, LV_SYMBOL_REFRESH, "Retry Delay",
                    15, 15, 300, " s", ap_wx_retry, rd_wx_retry);
    add_stepper_row(wx_timing, LV_SYMBOL_WIFI, "Wi-Fi Wait",
                    5, 10, 120, " s", ap_wx_wifi_wait, rd_wx_wifi_wait);
    add_stepper_row(wx_timing, LV_SYMBOL_DOWNLOAD, "HTTP Timeout",
                    1, 5, 30, " s", ap_wx_http_timeout, rd_wx_http_timeout);
    add_stepper_row(wx_timing, LV_SYMBOL_REFRESH, "Forecast Gap",
                    1, 1, 30, " s", ap_wx_forecast_gap, rd_wx_forecast_gap);
    add_stepper_row(wx_timing, LV_SYMBOL_EYE_OPEN, "Tab Stale",
                    1, 1, 60, " min", ap_wx_tab_stale, rd_wx_tab_stale);
    add_stepper_row(wx_timing, LV_SYMBOL_REFRESH, "Tab Wake",
                    5, 5, 120, " s", ap_wx_tab_wake, rd_wx_tab_wake);
    add_stepper_row(wx_timing, LV_SYMBOL_REFRESH, "Page Update",
                    1, 1, 10, " s", ap_wx_page_update, rd_wx_page_update);
    add_stepper_row(wx_timing, LV_SYMBOL_REFRESH, "Graph Auto-Scroll",
                    5, 5, 300, " s", ap_wx_graph_cycle, rd_wx_graph_cycle);

    lv_obj_t *idle_wx = deep_card(s_settings_weather_page);
    lv_obj_set_size(idle_wx, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(idle_wx, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(idle_wx, 0, LV_PART_MAIN);
    lv_obj_t *idle_title = lv_label_create(idle_wx);
    lv_label_set_text(idle_title, "Idle Weather Display");
    lv_obj_set_style_text_color(idle_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(idle_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(idle_title, 8, LV_PART_MAIN);
    add_stepper_row(idle_wx, LV_SYMBOL_REFRESH, "Screen Timeout",
                    15, 10, 600, " s", ap_to, rd_to);
    add_stepper_row(idle_wx, LV_SYMBOL_CHARGE, "Display Brightness",
                    5, 5, 100, "%", ap_disp, rd_disp);
    s_weather_bottom_mode_dropdown = settings_dropdown_row(idle_wx, LV_SYMBOL_LIST, "Bottom Panel",
                                                           "Data\nGraphs\nCycle",
                                                           weather_bottom_mode_changed, NULL);
    add_stepper_row(idle_wx, LV_SYMBOL_REFRESH, "Panel Cycle",
                    5, 5, 300, " s", ap_wx_bottom_cycle, rd_wx_bottom_cycle);

    /* ---- Display page ---- */
    settings_header(s_settings_display_page, "Display");
    lv_obj_t *disp_card = deep_card(s_settings_display_page);
    lv_obj_set_size(disp_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(disp_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(disp_card, 0, LV_PART_MAIN);
    s_settings_display_status = settings_status_label(disp_card);
    add_stepper_row(disp_card, LV_SYMBOL_CHARGE, "Brightness",
                    5, 5, 100, "%", ap_disp, rd_disp);
    add_stepper_row(disp_card, LV_SYMBOL_REFRESH, "Timeout",
                    15, 10, 600, " s", ap_to, rd_to);
    add_stepper_row(disp_card, LV_SYMBOL_REFRESH, "Auto Hold",
                    1, 1, 60, " min", ap_auto_hold, rd_auto_hold);
    add_stepper_row(disp_card, LV_SYMBOL_CHARGE, "Low Warn",
                    5, 5, 50, "%", ap_low_warn, rd_low_warn);
    s_auto_brightness_switch = settings_switch_row(disp_card, LV_SYMBOL_EYE_OPEN, "Auto-Brightness",
                                                   auto_brightness_switch_event, NULL);

    lv_obj_t *auto_card = deep_card(s_settings_display_page);
    lv_obj_set_size(auto_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(auto_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(auto_card, 0, LV_PART_MAIN);
    lv_obj_t *auto_title = lv_label_create(auto_card);
    lv_label_set_text(auto_title, "Auto-Brightness Limits");
    lv_obj_set_style_text_color(auto_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(auto_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(auto_title, 8, LV_PART_MAIN);
    add_stepper_row(auto_card, LV_SYMBOL_CHARGE, "Auto Min",
                    1, 1, 60, "%", ap_auto_min, rd_auto_min);
    add_stepper_row(auto_card, LV_SYMBOL_CHARGE, "Auto Max",
                    5, 20, 100, "%", ap_auto_max, rd_auto_max);
    add_stepper_row(auto_card, LV_SYMBOL_REFRESH, "Check Every",
                    5, 5, 300, " s", ap_auto_eval, rd_auto_eval);
    add_stepper_row(auto_card, LV_SYMBOL_REFRESH, "Ramp Time",
                    1, 1, 30, " s", ap_auto_ramp, rd_auto_ramp);

    /* ---- Idle Screen page ---- */
    settings_header(s_settings_idle_page, "Idle Screen");
    lv_obj_t *idle_card = deep_card(s_settings_idle_page);
    lv_obj_set_size(idle_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(idle_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(idle_card, 0, LV_PART_MAIN);
    s_settings_idle_status = settings_status_label(idle_card);
    lv_label_set_text(s_settings_idle_status, "Weather clock activates after timeout");
    add_stepper_row(idle_card, LV_SYMBOL_REFRESH, "Idle Timeout",
                    15, 10, 600, " s", ap_to, rd_to);
    add_stepper_row(idle_card, LV_SYMBOL_CHARGE, "Idle Brightness",
                    5, 5, 100, "%", ap_disp, rd_disp);
    add_stepper_row(idle_card, LV_SYMBOL_REFRESH, "Idle Check",
                    1, 1, 10, " s", ap_idle_check, rd_idle_check);
    add_stepper_row(idle_card, LV_SYMBOL_UP, "Swipe Dismiss",
                    5, 1, 240, " min", ap_idle_swipe_dismiss_min, rd_idle_swipe_dismiss_min);
    s_idle_dismiss_lights_switch = settings_switch_row(idle_card, LV_SYMBOL_EYE_OPEN, "Wake Lights",
                                                       idle_dismiss_lights_switch_event, NULL);
    s_idle_swipe_wake_lights_switch = settings_switch_row(idle_card, LV_SYMBOL_UP, "Swipe Wakes Lights",
                                                          idle_swipe_wake_lights_switch_event, NULL);
    s_idle_wake_timer_switch = settings_switch_row(idle_card, LV_SYMBOL_REFRESH, "Wake Timer",
                                                   idle_wake_timer_switch_event, NULL);
    add_stepper_row(idle_card, LV_SYMBOL_REFRESH, "Wake Duration",
                    5, 1, 240, " min", ap_idle_wake_timer_min, rd_idle_wake_timer_min);

    /* ---- Audio page ---- */
    settings_header(s_settings_audio_page, "Audio");
    lv_obj_t *audio_card = deep_card(s_settings_audio_page);
    lv_obj_set_size(audio_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(audio_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(audio_card, 14, LV_PART_MAIN);
    s_settings_audio_status = settings_status_label(audio_card);

    /* ---- Timer page ---- */
    settings_header(s_settings_timer_page, "Timer");
    screen_timer_settings_create(s_settings_timer_page);

    /* ---- System page ---- */
    settings_header(s_settings_system_page, "System");
    info_panel_create(s_settings_system_page, s_settings_system_page);

    lv_obj_t *diag_card = deep_card(s_settings_system_page);
    lv_obj_set_size(diag_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(diag_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(diag_card, 0, LV_PART_MAIN);
    lv_obj_t *diag_title = lv_label_create(diag_card);
    lv_label_set_text(diag_title, "Diagnostics");
    lv_obj_set_style_text_color(diag_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(diag_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(diag_title, 8, LV_PART_MAIN);
    s_system_cpu_load_switch = settings_switch_row(diag_card, LV_SYMBOL_EYE_OPEN, "CPU Load Meter",
                                                   system_cpu_load_switch_event, NULL);

    lv_obj_t *clock_timing = deep_card(s_settings_system_page);
    lv_obj_set_size(clock_timing, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock_timing, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(clock_timing, 0, LV_PART_MAIN);
    lv_obj_t *clock_timing_title = lv_label_create(clock_timing);
    lv_label_set_text(clock_timing_title, "Clock");
    lv_obj_set_style_text_color(clock_timing_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(clock_timing_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(clock_timing_title, 8, LV_PART_MAIN);
    add_stepper_row(clock_timing, LV_SYMBOL_REFRESH, "Sync Every",
                    60, 60, 1440, " min", ap_time_sync_interval, rd_time_sync_interval);
    add_stepper_row(clock_timing, LV_SYMBOL_REFRESH, "Sync Hour",
                    1, 0, 23, ":00", ap_time_sync_hour, rd_time_sync_hour);

    lv_obj_t *ui_timing = deep_card(s_settings_system_page);
    lv_obj_set_size(ui_timing, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui_timing, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui_timing, 0, LV_PART_MAIN);
    lv_obj_t *ui_timing_title = lv_label_create(ui_timing);
    lv_label_set_text(ui_timing_title, "UI Timing");
    lv_obj_set_style_text_color(ui_timing_title, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_timing_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ui_timing_title, 8, LV_PART_MAIN);
    add_stepper_row(ui_timing, LV_SYMBOL_REFRESH, "Status Update",
                    1, 1, 10, " s", ap_status_update, rd_status_update);
    add_stepper_row(ui_timing, LV_SYMBOL_LIST, "Toast Duration",
                    200, 800, 5000, " ms", ap_toast_duration, rd_toast_duration);

    lv_obj_t *sys_card = deep_card(s_settings_system_page);
    lv_obj_set_size(sys_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sys_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sys_card, 14, LV_PART_MAIN);
    lv_obj_t *sys_btns = settings_button_row(sys_card);
    settings_button(sys_btns, "Reboot", false, system_reboot_clicked);

    /* ---- About page ---- */
    settings_header(s_settings_about_page, "About");
    lv_obj_t *about_card = deep_card(s_settings_about_page);
    lv_obj_set_size(about_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(about_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(about_card, 10, LV_PART_MAIN);
    lv_obj_t *about_title = lv_label_create(about_card);
    lv_label_set_text(about_title, "P4 Wall Display");
    lv_obj_set_style_text_color(about_title, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(about_title, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_t *about_body = lv_label_create(about_card);
    lv_label_set_long_mode(about_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(about_body, LV_PCT(100));
    lv_obj_set_style_text_color(about_body, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(about_body, THEME_FONT_BODY, LV_PART_MAIN);
    {
        const esp_app_desc_t *desc = esp_app_get_description();
        char about_text[256];
        snprintf(about_text, sizeof(about_text),
                 "Firmware: %s\nIDF: %s\nBuild: %s %s\n\n"
                 "ESP32-P4 + WLED on ESP32-S3\n"
                 "LVGL 9 / MIPI-DSI 720x720",
                 desc->version, esp_get_idf_version(),
                 desc->date, desc->time);
        lv_label_set_text(about_body, about_text);
    }

    s_settings_keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(s_settings_keyboard, LV_PCT(100), 220);
    lv_obj_align(s_settings_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_settings_keyboard, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_settings_keyboard, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_settings_keyboard, LV_OBJ_FLAG_HIDDEN);

    settings_load_fields();
    settings_status_refresh(NULL);
    settings_show_panel(s_settings_home);
    lv_timer_create(settings_status_refresh, 1500, NULL);

    return p;
}

/* ============================================================
 * IDLE / AMBIENT WEATHER SCREEN
 * ============================================================ */

static lv_obj_t *s_idle_hour_tens;
static lv_obj_t *s_idle_hour_ones;
static lv_obj_t *s_idle_min_tens;
static lv_obj_t *s_idle_min_ones;
static lv_obj_t *s_idle_sec_tens;
static lv_obj_t *s_idle_sec_ones;
static lv_obj_t *s_idle_ampm;       /* Small AM/PM badge next to clock */
static lv_obj_t *s_idle_date;       /* "Sunday, July 24" */
static lv_obj_t *s_idle_temp;       /* Top-right big temp */
static lv_obj_t *s_idle_cond;       /* Condition text (Cloudy, etc.) */
static lv_obj_t *s_idle_extra;      /* Humidity / wind / etc. summary line */
static lv_obj_t *s_idle_icon;       /* Top-left weather symbol */
static lv_obj_t *s_idle_feels;      /* "Feels Like 98°" */
static lv_obj_t *s_idle_hilo;       /* "Hi 83°  Lo 64°" line under temp */
static lv_obj_t *s_idle_moon_icon;
static lv_obj_t *s_idle_moon_label;
/* Forecast strip handles (5 slots: day name, icon, hi/lo). */
#define IDLE_FORECAST_SLOTS 5
#define IDLE_TOP_PAD_X 12
#define IDLE_WEATHER_W 272
#define IDLE_TEMP_LABEL_W 244
#define IDLE_MOON_LEFT_PAD ((IDLE_WEATHER_W - IDLE_TEMP_LABEL_W) + 12)
#define IDLE_MOON_LABEL_W 170
#define IDLE_MOON_ICON_SCALE 320
#define IDLE_CLOCK_W 404
static lv_obj_t *s_idle_fc_day[IDLE_FORECAST_SLOTS];
static lv_obj_t *s_idle_fc_icon[IDLE_FORECAST_SLOTS];
static lv_obj_t *s_idle_fc_temp[IDLE_FORECAST_SLOTS];

/* Bottom metric tile value labels. */
static lv_obj_t *s_idle_m_sunrise;
static lv_obj_t *s_idle_m_sunset;
static lv_obj_t *s_idle_m_wind;
static lv_obj_t *s_idle_m_humidity;
static lv_obj_t *s_idle_m_uv;
static lv_obj_t *s_idle_m_pressure;
static lv_obj_t *s_idle_m_clouds;
static lv_obj_t *s_idle_m_visibility;
static lv_obj_t *s_idle_m_gust;
static lv_obj_t *s_idle_m_precip;
static lv_obj_t *s_idle_metrics_panel;
static lv_obj_t *s_idle_graph_panel;
static uint32_t s_idle_bottom_next_ms;

/* The idle screen is a separate, cached lv_screen, so the theme/opacity refresh
 * that runs on lv_screen_active() (the main UI) never reaches it. Re-sync it
 * here — called from the theme settings handlers — so its glass panels honor
 * the surface-opacity (transparency) setting like the main screen's do. */
static void idle_apply_theme(void)
{
    if (!s_idle_screen) return;
    theme_recolor_tree(s_idle_screen);
    theme_apply_surface_opacity(s_idle_screen);
}

static void idle_set_digit(lv_obj_t *label, int digit)
{
    char text[2] = {(char)('0' + digit), '\0'};
    label_set_text_if_changed(label, text);
}

static void idle_set_clock_unavailable(void)
{
    label_set_text_if_changed(s_idle_hour_tens, "-");
    label_set_text_if_changed(s_idle_hour_ones, "-");
    label_set_text_if_changed(s_idle_min_tens, "-");
    label_set_text_if_changed(s_idle_min_ones, "-");
    label_set_text_if_changed(s_idle_sec_tens, "-");
    label_set_text_if_changed(s_idle_sec_ones, "-");
    label_set_text_if_changed(s_idle_ampm, "");
    label_set_text_if_changed(s_idle_date, "Awaiting time sync");
}

static void idle_set_clock_time(const struct tm *lt)
{
    static const char *weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int hour12 = lt->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;

    if (hour12 >= 10) idle_set_digit(s_idle_hour_tens, hour12 / 10);
    else              label_set_text_if_changed(s_idle_hour_tens, "");
    idle_set_digit(s_idle_hour_ones, hour12 % 10);
    idle_set_digit(s_idle_min_tens, lt->tm_min / 10);
    idle_set_digit(s_idle_min_ones, lt->tm_min % 10);
    idle_set_digit(s_idle_sec_tens, lt->tm_sec / 10);
    idle_set_digit(s_idle_sec_ones, lt->tm_sec % 10);
    label_set_text_if_changed(s_idle_ampm, lt->tm_hour < 12 ? "AM" : "PM");

    char date[32];
    snprintf(date, sizeof(date), "%s, %s %d",
             weekdays[lt->tm_wday % 7], months[lt->tm_mon % 12], lt->tm_mday);
    label_set_text_if_changed(s_idle_date, date);
}

static void idle_clock_tick_cb(lv_timer_t *t)
{
    if (t && s_idle_screen && lv_screen_active() != s_idle_screen) return;
    if (s_idle_hour_ones) {
        time_t now = time(NULL);
        struct tm lt; localtime_r(&now, &lt);
        if (lt.tm_year + 1900 < 2024) {
            idle_set_clock_unavailable();
        } else {
            idle_set_clock_time(&lt);
        }
    }
}

static void idle_bottom_show_graph(bool show_graph)
{
    if (s_idle_metrics_panel) {
        if (show_graph) lv_obj_add_flag(s_idle_metrics_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_idle_metrics_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_idle_graph_panel) {
        if (show_graph) lv_obj_clear_flag(s_idle_graph_panel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_idle_graph_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void idle_bottom_graph_refresh(void)
{
    weather_state_t w;
    if (weather_state_get(&w) == ESP_OK && w.valid) wx_forecast_graphs_refresh(&w);
    else wx_forecast_graphs_clear("Hourly forecast unavailable");
}

static void idle_bottom_panel_update(bool force)
{
    app_tuning_config_t cfg = load_tuning_config();
    uint32_t now = ui_now_ms();
    uint32_t interval_ms = (uint32_t)cfg.weather_bottom_panel_cycle_s * 1000u;
    bool elapsed = force || !s_idle_bottom_next_ms || (int32_t)(now - s_idle_bottom_next_ms) >= 0;
    bool was_graph_visible = s_idle_graph_visible;

    if (cfg.weather_bottom_panel_mode == APP_WEATHER_BOTTOM_PANEL_DATA) {
        s_idle_graph_visible = false;
        s_idle_bottom_next_ms = now + interval_ms;
    } else if (cfg.weather_bottom_panel_mode == APP_WEATHER_BOTTOM_PANEL_GRAPHS) {
        s_idle_graph_visible = true;
        if (elapsed) s_idle_bottom_next_ms = now + interval_ms;
    } else {
        if (force) {
            s_idle_graph_visible = false;
            s_idle_bottom_next_ms = now + interval_ms;
        } else if (elapsed) {
            if (s_idle_graph_visible) {
                s_idle_graph_visible = false;
            } else {
                s_idle_graph_visible = true;
            }
            s_idle_bottom_next_ms = now + interval_ms;
        }
    }

    idle_bottom_show_graph(s_idle_graph_visible);
    if (s_idle_graph_visible) {
        if (!was_graph_visible) weather_graph_cycle_timer_reset();
        idle_bottom_graph_refresh();
    }
}

static void idle_bottom_panel_reset(void)
{
    s_idle_bottom_next_ms = 0;
    s_idle_graph_visible = false;
    idle_bottom_panel_update(true);
}

static void idle_weather_tick_cb(lv_timer_t *t)
{
    if (t && s_idle_screen && lv_screen_active() != s_idle_screen) return;
    weather_state_t w;
    if (weather_state_get(&w) == ESP_OK && w.valid) {
        char b[64];
        snprintf(b, sizeof(b), "%ld°", (long)w.temp_f);
        label_set_text_if_changed(s_idle_temp, b);
        snprintf(b, sizeof(b), "Feels like %ld°", (long)w.feels_f);
        label_set_text_if_changed(s_idle_feels, b);
        if (s_idle_hilo) {
            if (w.temp_min_f || w.temp_max_f) {
                snprintf(b, sizeof(b), LV_SYMBOL_UP " %ld°   " LV_SYMBOL_DOWN " %ld°",
                         (long)w.temp_max_f, (long)w.temp_min_f);
            } else snprintf(b, sizeof(b), "Hi —   Lo —");
            label_set_text_if_changed(s_idle_hilo, b);
        }
        label_set_text_if_changed(s_idle_cond, w.condition[0] ? w.condition : "—");
        bool night = weather_is_night(&w);
        label_set_text_if_changed(s_idle_icon, weather_symbol(w.condition, w.icon, night));
        lv_obj_set_style_text_color(s_idle_icon, weather_color(w.condition), LV_PART_MAIN);
        if (s_idle_extra) {
            if (w.city[0]) snprintf(b, sizeof(b), LV_SYMBOL_GPS " %s", w.city);
            else b[0] = '\0';
            label_set_text_if_changed(s_idle_extra, b);
        }
        moon_phase_labels_set(s_idle_moon_icon, s_idle_moon_label, &w, true);

        /* Forecast strip */
        for (int i = 0; i < IDLE_FORECAST_SLOTS; i++) {
            if (i < w.day_count) {
                char name[8];
                format_local_weekday(name, sizeof(name), w.days[i].dt_utc, w.tz_offset_s);
                label_set_text_if_changed(s_idle_fc_day[i], name);
                label_set_text_if_changed(s_idle_fc_icon[i], weather_symbol(w.days[i].condition,
                                                                            w.days[i].icon, false));
                lv_obj_set_style_text_color(s_idle_fc_icon[i],
                                            weather_color(w.days[i].condition), LV_PART_MAIN);
                snprintf(b, sizeof(b), "%d° / %d°", w.days[i].hi_f, w.days[i].lo_f);
                label_set_text_if_changed(s_idle_fc_temp[i], b);
            } else {
                label_set_text_if_changed(s_idle_fc_day[i], "---");
                label_set_text_if_changed(s_idle_fc_icon[i], WI_NA);
                label_set_text_if_changed(s_idle_fc_temp[i], "—/—");
            }
        }

        /* Bottom metric tiles */
        char hm[16];
        format_local_hm(hm, sizeof(hm), w.sunrise_utc, w.tz_offset_s);
        label_set_text_if_changed(s_idle_m_sunrise, hm);
        format_local_hm(hm, sizeof(hm), w.sunset_utc, w.tz_offset_s);
        label_set_text_if_changed(s_idle_m_sunset, hm);

        snprintf(b, sizeof(b), "%u.%u mph %s",
                 w.wind_mph_x10 / 10, w.wind_mph_x10 % 10, wind_compass(w.wind_deg));
        label_set_text_if_changed(s_idle_m_wind, b);

        snprintf(b, sizeof(b), "%u%%", w.humidity_pct);
        label_set_text_if_changed(s_idle_m_humidity, b);

        if (w.uv_index_valid) snprintf(b, sizeof(b), "%u", w.uv_index);
        else                  snprintf(b, sizeof(b), "--");
        label_set_text_if_changed(s_idle_m_uv, b);

        if (w.pressure_hpa) {
            wx_format_pressure_inhg(b, sizeof(b), w.pressure_hpa);
        } else snprintf(b, sizeof(b), "--");
        label_set_text_if_changed(s_idle_m_pressure, b);

        snprintf(b, sizeof(b), "%u%%", w.clouds_pct);
        label_set_text_if_changed(s_idle_m_clouds, b);

        if (w.visibility_m) {
            snprintf(b, sizeof(b), "%.1f mi", (double)w.visibility_m / 1609.344);
        } else snprintf(b, sizeof(b), "--");
        label_set_text_if_changed(s_idle_m_visibility, b);

        if (w.wind_gust_valid) {
            snprintf(b, sizeof(b), "%u.%u mph",
                     w.wind_gust_mph_x10 / 10, w.wind_gust_mph_x10 % 10);
        } else snprintf(b, sizeof(b), "--");
        label_set_text_if_changed(s_idle_m_gust, b);

        wx_format_precip_1h(b, sizeof(b), w.rain_1h_mm_x10, w.snow_1h_mm_x10);
        label_set_text_if_changed(s_idle_m_precip, b);
        idle_bottom_panel_update(false);
    } else {
        label_set_text_if_changed(s_idle_temp, "--°");
        label_set_text_if_changed(s_idle_feels, "Feels like —°");
        label_set_text_if_changed(s_idle_hilo, "Hi —   Lo —");
        label_set_text_if_changed(s_idle_cond, "Weather unavailable");
        label_set_text_if_changed(s_idle_icon, WI_NA);
        label_set_text_if_changed(s_idle_extra, "Tap screen to wake");
        moon_phase_labels_set(s_idle_moon_icon, s_idle_moon_label, NULL, true);
        for (int i = 0; i < IDLE_FORECAST_SLOTS; i++) {
            label_set_text_if_changed(s_idle_fc_day[i], "---");
            label_set_text_if_changed(s_idle_fc_icon[i], WI_NA);
            label_set_text_if_changed(s_idle_fc_temp[i], "—/—");
        }
        label_set_text_if_changed(s_idle_m_sunrise, "—:—");
        label_set_text_if_changed(s_idle_m_sunset, "—:—");
        label_set_text_if_changed(s_idle_m_wind, "—");
        label_set_text_if_changed(s_idle_m_humidity, "—");
        label_set_text_if_changed(s_idle_m_uv, "—");
        label_set_text_if_changed(s_idle_m_pressure, "—");
        label_set_text_if_changed(s_idle_m_clouds, "—");
        label_set_text_if_changed(s_idle_m_visibility, "—");
        label_set_text_if_changed(s_idle_m_gust, "—");
        label_set_text_if_changed(s_idle_m_precip, "—");
        idle_bottom_panel_update(false);
    }
}

static lv_obj_t *idle_clean_obj(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *idle_clock_label(lv_obj_t *parent, const lv_font_t *font,
                                  lv_coord_t width, lv_coord_t height,
                                  lv_color_t color, lv_coord_t top_pad)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (top_pad) lv_obj_set_style_pad_top(label, top_pad, LV_PART_MAIN);
    return label;
}

/* Build a "Day / icon / Hi°|Lo° / pop%" card for the forecast strip. */
static void idle_forecast_cell(lv_obj_t *parent, const char *day,
                                const char *icon, const char *hi_lo,
                                lv_obj_t **out_day, lv_obj_t **out_icon,
                                lv_obj_t **out_temp)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, 128, 150);
    lv_obj_set_style_bg_color(cell, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 3, LV_PART_MAIN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *d = lv_label_create(cell);
    lv_label_set_text(d, day);
    lv_obj_set_style_text_font(d, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(d, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    if (out_day) *out_day = d;

    lv_obj_t *ic = lv_label_create(cell);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, THEME_FONT_WX_MEDIUM, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    if (out_icon) *out_icon = ic;

    lv_obj_t *t = lv_label_create(cell);
    lv_label_set_text(t, hi_lo);
    lv_obj_set_style_text_font(t, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    if (out_temp) *out_temp = t;
}

/* Build a styled "icon + label/value" tile for the bottom metric strip. */
static void idle_metric_cell(lv_obj_t *parent, const char *icon,
                              const char *label, const char *value,
                              lv_color_t accent,
                              lv_obj_t **out_value)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 132, 110);
    lv_obj_set_style_bg_color(tile, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(tile, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(tile, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tile, 8, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_label_create(tile);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, THEME_FONT_WX_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, accent, LV_PART_MAIN);

    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_row(col, 2, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, label);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, THEME_TEXT_MUTED, LV_PART_MAIN);

    lv_obj_t *v = lv_label_create(col);
    lv_label_set_text(v, value);
    lv_obj_set_width(v, LV_PCT(100));
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(v, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    if (out_value) *out_value = v;
}

lv_obj_t *screen_idle_create_legacy_unused(void)
{
    return NULL;
}

static void idle_screen_gesture_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_gesture_dir(indev) != LV_DIR_TOP) return;

    app_tuning_config_t cfg = load_tuning_config();
    idle_manager_dismiss_for_minutes(cfg.idle_swipe_dismiss_min);
}

static void idle_screen_attach_gestures(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_add_event_cb(obj, idle_screen_gesture_event, LV_EVENT_GESTURE, NULL);

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        idle_screen_attach_gestures(lv_obj_get_child(obj, i));
    }
}

/* ------------------------------------------------------------
 * Card-based idle layout — current-conditions on the left, clock
 * on the right (icon + temp + Feels Like) inspired by the
 * lmarzen/esp32-weather-epd reference image. Below: 5-day forecast
 * card, then a 5×2 metric tile grid filling the bottom band.
 * ------------------------------------------------------------ */
lv_obj_t *screen_idle_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    s_idle_screen = scr;
    lv_obj_set_style_bg_color(scr, THEME_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Attach background image layer (shared with main screen) */
    ui_background_attach_idle_weather(scr);

    /* ===== Top hero: current weather | clock ===== */
    lv_obj_t *top = lv_obj_create(scr);
    theme_style_glass_panel(top, 14);
    lv_obj_set_size(top, LV_PCT(100), 268);
    lv_obj_set_style_pad_hor(top, IDLE_TOP_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(top, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Clock column (right) ---- */
    lv_obj_t *clock_col = idle_clean_obj(top);
    lv_obj_set_size(clock_col, IDLE_CLOCK_W, LV_PCT(100));
    lv_obj_set_flex_flow(clock_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clock_col, 0, LV_PART_MAIN);

    s_idle_date = lv_label_create(clock_col);
    lv_label_set_text(s_idle_date, "");
    lv_obj_set_width(s_idle_date, IDLE_CLOCK_W);
    lv_label_set_long_mode(s_idle_date, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_idle_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_date, THEME_FONT_XLARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_date, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    s_idle_extra = lv_label_create(clock_col);
    lv_label_set_text(s_idle_extra, "");
    lv_obj_set_width(s_idle_extra, IDLE_CLOCK_W);
    lv_label_set_long_mode(s_idle_extra, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_idle_extra, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_extra, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_extra, THEME_TEXT_MUTED, LV_PART_MAIN);

    /* HH:MM row (fixed digit cells + seconds/AM-PM stack). */
    lv_obj_t *time_row = idle_clean_obj(clock_col);
    lv_obj_set_size(time_row, IDLE_CLOCK_W, 166);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(time_row, 1, LV_PART_MAIN);

    s_idle_hour_tens = idle_clock_label(time_row, THEME_FONT_WX_TIME, 68, 130,
                                        THEME_TEXT_PRIMARY, 0);
    s_idle_hour_ones = idle_clock_label(time_row, THEME_FONT_WX_TIME, 68, 130,
                                        THEME_TEXT_PRIMARY, 0);

    lv_obj_t *colon = idle_clock_label(time_row, THEME_FONT_WX_TIME, 34, 130,
                                       THEME_TEXT_PRIMARY, 0);
    lv_label_set_text(colon, ":");

    s_idle_min_tens = idle_clock_label(time_row, THEME_FONT_WX_TIME, 68, 130,
                                       THEME_TEXT_PRIMARY, 0);
    s_idle_min_ones = idle_clock_label(time_row, THEME_FONT_WX_TIME, 68, 130,
                                       THEME_TEXT_PRIMARY, 0);

    lv_obj_t *sec_col = idle_clean_obj(time_row);
    lv_obj_set_size(sec_col, 88, 132);
    lv_obj_set_flex_flow(sec_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sec_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(sec_col, 0, LV_PART_MAIN);

    lv_obj_t *sec_row = idle_clean_obj(sec_col);
    lv_obj_set_size(sec_row, 88, 96);
    lv_obj_set_flex_flow(sec_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sec_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(sec_row, 0, LV_PART_MAIN);

    s_idle_sec_tens = idle_clock_label(sec_row, THEME_FONT_WX_SECONDS, 44, 92,
                                       THEME_PRIMARY_COLOR, 4);
    s_idle_sec_ones = idle_clock_label(sec_row, THEME_FONT_WX_SECONDS, 44, 92,
                                       THEME_PRIMARY_COLOR, 4);

    s_idle_ampm = lv_label_create(sec_col);
    lv_label_set_text(s_idle_ampm, "");
    lv_obj_set_width(s_idle_ampm, 88);
    lv_label_set_long_mode(s_idle_ampm, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_idle_ampm, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_ampm, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_ampm, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    /* ---- Current weather column (left) ---- */
    lv_obj_t *wx_col = idle_clean_obj(top);
    lv_obj_set_size(wx_col, IDLE_WEATHER_W, LV_PCT(100));
    lv_obj_set_flex_flow(wx_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wx_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wx_col, 0, LV_PART_MAIN);
    lv_obj_move_to_index(wx_col, 0);

    /* Icon + temp side by side */
    lv_obj_t *wx_row = idle_clean_obj(wx_col);
    lv_obj_set_size(wx_row, IDLE_WEATHER_W, 142);
    lv_obj_add_flag(wx_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_idle_icon = lv_label_create(wx_row);
    lv_label_set_text(s_idle_icon, WI_NA);
    lv_obj_set_style_text_font(s_idle_icon, THEME_FONT_WX_MEDIUM, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_icon, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_align(s_idle_icon, LV_ALIGN_LEFT_MID, 0, -2);

    lv_obj_t *temp_cluster = idle_clean_obj(wx_row);
    lv_obj_set_size(temp_cluster, IDLE_TEMP_LABEL_W, 142);
    lv_obj_add_flag(temp_cluster, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(temp_cluster, LV_ALIGN_RIGHT_MID, 0, 0);

    s_idle_temp = lv_label_create(temp_cluster);
    lv_label_set_text(s_idle_temp, "--°");
    lv_obj_set_width(s_idle_temp, IDLE_TEMP_LABEL_W);
    lv_label_set_long_mode(s_idle_temp, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_idle_temp, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_temp, THEME_FONT_WX_HERO_NUM, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_temp, THEME_TEXT_PRIMARY, LV_PART_MAIN);

    s_idle_feels = lv_label_create(wx_col);
    lv_label_set_text(s_idle_feels, "Feels like —°");
    lv_obj_set_width(s_idle_feels, IDLE_WEATHER_W);
    lv_label_set_long_mode(s_idle_feels, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_idle_feels, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_feels, THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_feels, THEME_TEXT_SECONDARY, LV_PART_MAIN);

    s_idle_cond = lv_label_create(wx_col);
    lv_label_set_text(s_idle_cond, "Weather unavailable");
    lv_obj_set_width(s_idle_cond, IDLE_WEATHER_W);
    lv_label_set_long_mode(s_idle_cond, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_idle_cond, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_cond, THEME_FONT_BODY_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_cond, THEME_TEXT_MUTED, LV_PART_MAIN);

    s_idle_hilo = lv_label_create(wx_col);
    lv_label_set_text(s_idle_hilo, "Hi —   Lo —");
    lv_obj_set_width(s_idle_hilo, IDLE_WEATHER_W);
    lv_label_set_long_mode(s_idle_hilo, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_idle_hilo, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_hilo, THEME_FONT_BODY_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_hilo, THEME_TEXT_MUTED, LV_PART_MAIN);

    lv_obj_t *moon_row = idle_clean_obj(wx_col);
    lv_obj_set_size(moon_row, IDLE_WEATHER_W, 36);
    lv_obj_set_flex_flow(moon_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(moon_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(moon_row, IDLE_MOON_LEFT_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_column(moon_row, 8, LV_PART_MAIN);
    lv_obj_add_flag(moon_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_idle_moon_icon = lv_label_create(moon_row);
    lv_label_set_text(s_idle_moon_icon, WI_NA);
    lv_obj_set_style_text_font(s_idle_moon_icon, THEME_FONT_WX_SMALL, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(s_idle_moon_icon, IDLE_MOON_ICON_SCALE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_moon_icon, lv_color_hex(0xD9E2FF), LV_PART_MAIN);

    s_idle_moon_label = lv_label_create(moon_row);
    lv_label_set_text(s_idle_moon_label, "Moon —");
    lv_obj_set_width(s_idle_moon_label, IDLE_MOON_LABEL_W);
    lv_label_set_long_mode(s_idle_moon_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_idle_moon_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_idle_moon_label, THEME_FONT_BODY_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_idle_moon_label, THEME_TEXT_MUTED, LV_PART_MAIN);

    /* ===== 5-day forecast row ===== */
    lv_obj_t *fc_row = lv_obj_create(scr);
    theme_style_glass_panel(fc_row, 14);
    lv_obj_set_size(fc_row, LV_PCT(100), 174);
    lv_obj_set_style_pad_hor(fc_row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(fc_row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(fc_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fc_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(fc_row, LV_OBJ_FLAG_SCROLLABLE);

    const char *days[]  = {"Mon", "Tue", "Wed", "Thu", "Fri"};
    for (size_t i = 0; i < IDLE_FORECAST_SLOTS; i++) {
        idle_forecast_cell(fc_row, days[i], WI_NA, "—/—",
                           &s_idle_fc_day[i], &s_idle_fc_icon[i], &s_idle_fc_temp[i]);
    }

    /* ===== Bottom metrics: 5x2 tile grid ===== */
    lv_obj_t *metrics = lv_obj_create(scr);
    s_idle_metrics_panel = metrics;
    theme_style_glass_panel(metrics, 14);
    lv_obj_set_size(metrics, LV_PCT(100), 230);
    lv_obj_set_flex_grow(metrics, 1);
    lv_obj_set_style_pad_hor(metrics, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(metrics, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_row(metrics, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(metrics, 4, LV_PART_MAIN);
    lv_obj_clear_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);

    idle_metric_cell(metrics, WI_SUNRISE,        "Sunrise",    "—:—", lv_color_hex(0xFFD23F), &s_idle_m_sunrise);
    idle_metric_cell(metrics, WI_SUNSET,         "Sunset",     "—:—", lv_color_hex(0xFF8C42), &s_idle_m_sunset);
    idle_metric_cell(metrics, WI_STRONG_WIND,    "Wind",       "—",   lv_color_hex(0x4FA8FF), &s_idle_m_wind);
    idle_metric_cell(metrics, WI_HUMIDITY,       "Humidity",   "—",   lv_color_hex(0x4FA8FF), &s_idle_m_humidity);
    idle_metric_cell(metrics, WI_HOT,            "UV Index",   "—",   lv_color_hex(0xFFD23F), &s_idle_m_uv);
    idle_metric_cell(metrics, WI_BAROMETER,      "Pressure",   "—",   lv_color_hex(0x9D7BFF), &s_idle_m_pressure);
    idle_metric_cell(metrics, WI_CLOUD,          "Clouds",     "—",   lv_color_hex(0x9AAACD), &s_idle_m_clouds);
    idle_metric_cell(metrics, WI_EYE,            "Visibility", "—",   lv_color_hex(0xB8C1E6), &s_idle_m_visibility);
    idle_metric_cell(metrics, WI_CLOUD_UP,       "Gust",       "—",   lv_color_hex(0x4FA8FF), &s_idle_m_gust);
    idle_metric_cell(metrics, WI_RAINDROPS,      "Precip 1h",  "—",   lv_color_hex(0x4FA8FF), &s_idle_m_precip);

    s_idle_graph_panel = lv_obj_create(scr);
    theme_style_glass_panel(s_idle_graph_panel, 14);
    lv_obj_set_size(s_idle_graph_panel, LV_PCT(100), 230);
    lv_obj_set_flex_grow(s_idle_graph_panel, 1);
    lv_obj_set_style_pad_hor(s_idle_graph_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_idle_graph_panel, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_idle_graph_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_idle_graph_panel, 6, LV_PART_MAIN);
    lv_obj_clear_flag(s_idle_graph_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_idle_graph_panel, LV_OBJ_FLAG_HIDDEN);
    wx_card_header(s_idle_graph_panel, LV_SYMBOL_LIST, "24-HOUR FORECAST GRAPHS", THEME_PRIMARY_COLOR);
    wx_forecast_graphs_carousel_create(s_idle_graph_panel, &s_idle_fc_view, 168, 108);
    wx_forecast_graphs_status_create(s_idle_graph_panel, &s_idle_fc_view);

    idle_bottom_panel_reset();
    lv_timer_create(idle_clock_tick_cb, 250, NULL);
    lv_timer_create(idle_weather_tick_cb, 30000, NULL);
    idle_clock_tick_cb(NULL);
    idle_weather_tick_cb(NULL);
    idle_screen_attach_gestures(scr);
    return scr;
}
