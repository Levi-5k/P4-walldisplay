#include "nav_bar.h"
#include "theme.h"
#include "fonts/ui_icons.h"

#define NAV_BUTTON_COUNT 4

static lv_obj_t *s_pages[NAV_BUTTON_COUNT];
static int       s_page_count;
static lv_obj_t *s_buttons[NAV_BUTTON_COUNT];
static lv_obj_t *s_button_pages[NAV_BUTTON_COUNT];
static nav_page_shown_cb_t s_page_shown_cb;
static void *s_page_shown_user_data;

static void notify_page_shown(lv_obj_t *page)
{
    if (s_page_shown_cb) s_page_shown_cb(page, s_page_shown_user_data);
}

static void show_only(lv_obj_t *page)
{
    for (int i = 0; i < s_page_count; i++) {
        bool should_hide = s_pages[i] != page;
        bool is_hidden = lv_obj_has_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        if (should_hide && !is_hidden) lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else if (!should_hide && is_hidden) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void highlight_button(lv_obj_t *btn)
{
    for (int i = 0; i < NAV_BUTTON_COUNT; i++) {
        if (!s_buttons[i]) continue;
        bool should_check = s_buttons[i] == btn;
        if (lv_obj_has_state(s_buttons[i], LV_STATE_CHECKED) != should_check) {
            lv_obj_set_state(s_buttons[i], LV_STATE_CHECKED, should_check);
        }
    }
}

static void nav_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn  = lv_event_get_target(e);
    lv_obj_t *page = lv_event_get_user_data(e);
    if (!page) return;
    show_only(page);
    highlight_button(btn);
    notify_page_shown(page);
}

void nav_bar_show_page(lv_obj_t *page)
{
    show_only(page);
    for (int i = 0; i < NAV_BUTTON_COUNT; i++) {
        if (s_button_pages[i] == page) {
            highlight_button(s_buttons[i]);
            break;
        }
    }
    notify_page_shown(page);
}

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *sym, const char *cap,
                          const lv_font_t *icon_font, lv_obj_t *page)
{
    (void)cap;
    lv_obj_t *b = lv_button_create(parent);
    /* Wider pill buttons (was a 76px circle); 31px radius keeps rounded ends. */
    lv_obj_set_size(b, 132, 62);
    lv_obj_set_style_radius(b, 31, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0A0D18), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(b, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(0x222B45), LV_PART_MAIN);
    lv_obj_set_style_border_color(b, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, THEME_PRIMARY_COLOR, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, THEME_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t *icon = lv_label_create(b);
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_font(icon, icon_font ? icon_font : THEME_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, THEME_TEXT_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, THEME_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_center(icon);

    if (page) lv_obj_add_event_cb(b, nav_btn_cb, LV_EVENT_CLICKED, page);
    return b;
}

lv_obj_t *nav_bar_create(lv_obj_t *parent, const nav_pages_t *pages)
{
    s_page_shown_cb = pages->page_shown_cb;
    s_page_shown_user_data = pages->page_shown_user_data;
    for (int i = 0; i < NAV_BUTTON_COUNT; i++) s_button_pages[i] = NULL;

    s_page_count = 0;
    if (pages->lights_page)   s_pages[s_page_count++] = pages->lights_page;
    if (pages->weather_page)  s_pages[s_page_count++] = pages->weather_page;
    if (pages->timer_page)    s_pages[s_page_count++] = pages->timer_page;
    if (pages->settings_page) s_pages[s_page_count++] = pages->settings_page;

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    theme_style_glass_panel(bar, 30);
    lv_obj_set_style_pad_hor(bar, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_buttons[0] = icon_btn(bar, UI_ICON_LIGHTS,   "Lights",   &ui_icons_32, pages->lights_page);
    s_buttons[1] = icon_btn(bar, UI_ICON_WEATHER,  "Weather",  &ui_icons_32, pages->weather_page);
    s_buttons[2] = icon_btn(bar, UI_ICON_TIMER,    "Timer",    &ui_icons_32, pages->timer_page);
    s_buttons[3] = icon_btn(bar, UI_ICON_SETTINGS, "Settings", &ui_icons_32, pages->settings_page);
    s_button_pages[0] = pages->lights_page;
    s_button_pages[1] = pages->weather_page;
    s_button_pages[2] = pages->timer_page;
    s_button_pages[3] = pages->settings_page;

    if (pages->lights_page) {
        show_only(pages->lights_page);
        highlight_button(s_buttons[0]);
        notify_page_shown(pages->lights_page);
    }
    return bar;
}
