#include "toast.h"
#include "app_config.h"
#include "theme.h"

#include <string.h>

static lv_obj_t  *s_toast;
static lv_obj_t  *s_toast_text;
static lv_timer_t *s_hide;

static uint32_t toast_duration_ms(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    return tuning.toast_duration_ms;
}

static void toast_clear(void)
{
    if (s_hide) {
        lv_timer_delete(s_hide);
        s_hide = NULL;
    }
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
        s_toast_text = NULL;
    }
}

static void delete_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
        s_toast_text = NULL;
    }
    if (s_hide) {
        lv_timer_delete(s_hide);
        s_hide = NULL;
    }
}

static void hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_hide) {
        lv_timer_delete(s_hide);
        s_hide = NULL;
    }
    delete_cb(NULL);
}

void toast_show(const char *text)
{
    if (!text || !text[0]) return;

    lv_obj_t *layer = lv_layer_sys();
    if (!layer) return;

    if (s_toast && s_toast_text) {
        const char *current = lv_label_get_text(s_toast_text);
        if (!current || strcmp(current, text) != 0) lv_label_set_text(s_toast_text, text);
        lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 34);
        lv_obj_move_foreground(s_toast);
        if (s_hide) {
            lv_timer_set_period(s_hide, toast_duration_ms());
            lv_timer_reset(s_hide);
        }
        return;
    }

    toast_clear();

    s_toast = lv_obj_create(layer);
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(s_toast, 220, LV_PART_MAIN);
    lv_obj_set_style_max_width(s_toast, 600, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toast, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toast, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_toast, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_toast, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_toast, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_toast, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_toast, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toast, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_toast, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_toast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_toast, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_toast, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_toast, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s_toast, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_toast, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toast, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_move_foreground(s_toast);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(s_toast, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *rail = lv_obj_create(s_toast);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, 5, 30);
    lv_obj_set_style_radius(rail, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rail, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    s_toast_text = lv_label_create(s_toast);
    lv_label_set_text(s_toast_text, text);
    lv_obj_set_style_max_width(s_toast_text, 520, LV_PART_MAIN);
    lv_label_set_long_mode(s_toast_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_toast_text, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_toast_text, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_toast_text, 3, LV_PART_MAIN);

    s_hide = lv_timer_create(hide_cb, toast_duration_ms(), NULL);
    lv_timer_set_repeat_count(s_hide, 1);
}
