#include "status_bar.h"
#include "theme.h"
#include "led_state.h"
#include "services.h"
#include "app_config.h"
#include "fonts/ui_icons.h"

#include <string.h>

static lv_obj_t *s_wifi;
static lv_obj_t *s_wled;
static lv_obj_t *s_rs485;
static lv_obj_t *s_weather;
static lv_obj_t *s_audio;
static lv_obj_t *s_brightness;
static lv_timer_t *s_status_timer;

static lv_color_t muted(void)   { return THEME_TEXT_SECONDARY; }
static lv_color_t ok(void)      { return THEME_PRIMARY_COLOR; }
static lv_color_t warn(void)    { return THEME_ACCENT_COLOR; }
static lv_color_t offline(void) { return THEME_TEXT_MUTED; }

static uint8_t brightness_warn_threshold_pct(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    return tuning.low_brightness_warn_pct;
}

/* Set both the glyph and color of a status icon, each change-detected so an
 * unchanged state is a no-op (cheap to call every refresh tick). */
static void set_icon(lv_obj_t *icon, const char *glyph, lv_color_t color)
{
    if (!icon) return;
    if (glyph && strcmp(lv_label_get_text(icon), glyph) != 0) {
        lv_label_set_text(icon, glyph);
    }
    if (!lv_color_eq(lv_obj_get_style_text_color(icon, LV_PART_MAIN), color)) {
        lv_obj_set_style_text_color(icon, color, LV_PART_MAIN);
    }
}

static void status_refresh(lv_timer_t *timer)
{
    (void)timer;
    services_status_t st;
    services_status_get(&st);

    led_state_t leds;
    led_state_get(&leds);

    /* Wi-Fi: full bars when connected; a slashed glyph when configured-but-down
     * (warn) or not configured (offline). No RSSI is exposed, so we don't fake
     * intermediate strength levels. */
    if (st.wifi_connected) {
        set_icon(s_wifi, UI_ICON_WIFI, ok());
    } else {
        set_icon(s_wifi, UI_ICON_WIFI_OFF, st.wifi_configured ? warn() : offline());
    }

    set_icon(s_wled, st.wled_online ? UI_ICON_WLED : UI_ICON_WLED_OFF,
             st.wled_online ? ok() : offline());
    set_icon(s_rs485, st.rs485_ready ? UI_ICON_LINK : UI_ICON_LINK_OFF,
             st.rs485_ready ? muted() : offline());
    set_icon(s_weather, st.weather_online ? UI_ICON_WEATHER : UI_ICON_WEATHER_OFF,
             st.weather_online ? ok() : (st.weather_configured ? warn() : offline()));
    set_icon(s_audio, st.sound_sync_ready ? UI_ICON_AUDIO : UI_ICON_AUDIO_OFF,
             st.sound_sync_ready ? ok() : offline());
    /* Brightness: full sun when bright, dimmed sun below the warn threshold. */
    {
        bool low = leds.display_brightness_pct < brightness_warn_threshold_pct();
        set_icon(s_brightness, low ? UI_ICON_SUN_DIM : UI_ICON_SUN, low ? warn() : muted());
    }
}

static lv_obj_t *icon(lv_obj_t *parent, const char *sym, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &ui_icons_bold_30, LV_PART_MAIN);
    return l;
}

static uint32_t status_refresh_period_ms(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    return (uint32_t)tuning.status_bar_update_s * 1000u;
}

void status_bar_apply_tuning(void)
{
    if (s_status_timer) lv_timer_set_period(s_status_timer, status_refresh_period_ms());
    status_refresh(NULL);
}

lv_obj_t *status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 42, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(left, 28, LV_PART_MAIN);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi  = icon(left, UI_ICON_WIFI,  THEME_TEXT_SECONDARY);
    s_wled  = icon(left, UI_ICON_WLED,  THEME_TEXT_SECONDARY);
    s_rs485 = icon(left, UI_ICON_LINK,  THEME_TEXT_SECONDARY);

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 28, LV_PART_MAIN);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    s_weather    = icon(right, UI_ICON_WEATHER, THEME_TEXT_SECONDARY);
    s_audio      = icon(right, UI_ICON_AUDIO,   THEME_TEXT_SECONDARY);
    s_brightness = icon(right, UI_ICON_SUN,     THEME_TEXT_SECONDARY);

    s_status_timer = lv_timer_create(status_refresh, status_refresh_period_ms(), NULL);
    status_refresh(NULL);

    return bar;
}
