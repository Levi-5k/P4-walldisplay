#include "theme.h"

static uint8_t s_surface_opacity_pct = 100;
static bool s_shadows_enabled = true;

typedef struct {
    uint32_t bg;
    uint32_t surface;
    uint32_t card;
    uint32_t border;
    uint32_t primary;
    uint32_t accent;
} theme_palette_t;

static const theme_palette_t s_default_palette = {
    .bg = THEME_DEFAULT_BG_HEX,
    .surface = THEME_DEFAULT_SURFACE_HEX,
    .card = THEME_DEFAULT_CARD_HEX,
    .border = THEME_DEFAULT_BORDER_HEX,
    .primary = THEME_DEFAULT_PRIMARY_HEX,
    .accent = THEME_DEFAULT_ACCENT_HEX,
};

static theme_palette_t s_palette = {
    .bg = THEME_DEFAULT_BG_HEX,
    .surface = THEME_DEFAULT_SURFACE_HEX,
    .card = THEME_DEFAULT_CARD_HEX,
    .border = THEME_DEFAULT_BORDER_HEX,
    .primary = THEME_DEFAULT_PRIMARY_HEX,
    .accent = THEME_DEFAULT_ACCENT_HEX,
};

static theme_palette_t s_previous_palette = {
    .bg = THEME_DEFAULT_BG_HEX,
    .surface = THEME_DEFAULT_SURFACE_HEX,
    .card = THEME_DEFAULT_CARD_HEX,
    .border = THEME_DEFAULT_BORDER_HEX,
    .primary = THEME_DEFAULT_PRIMARY_HEX,
    .accent = THEME_DEFAULT_ACCENT_HEX,
};

static uint32_t clean_hex(uint32_t hex)
{
    return hex & 0xFFFFFFu;
}

lv_color_t theme_bg_color(void)      { return lv_color_hex(s_palette.bg); }
lv_color_t theme_surface_color(void) { return lv_color_hex(s_palette.surface); }
lv_color_t theme_card_color(void)    { return lv_color_hex(s_palette.card); }
lv_color_t theme_border_color(void)  { return lv_color_hex(s_palette.border); }
lv_color_t theme_primary_color(void) { return lv_color_hex(s_palette.primary); }
lv_color_t theme_accent_color(void)  { return lv_color_hex(s_palette.accent); }

void theme_apply_config(const app_theme_config_t *cfg)
{
    s_previous_palette = s_palette;
    if (!cfg) {
        s_palette = s_default_palette;
        theme_set_surface_opacity_pct(100);
        theme_set_shadows_enabled(true);
        return;
    }
    s_palette.bg = clean_hex(cfg->bg_color_hex);
    s_palette.surface = clean_hex(cfg->surface_color_hex);
    s_palette.card = clean_hex(cfg->card_color_hex);
    s_palette.border = clean_hex(cfg->border_color_hex);
    s_palette.primary = clean_hex(cfg->primary_color_hex);
    s_palette.accent = clean_hex(cfg->accent_color_hex);
    theme_set_surface_opacity_pct(cfg->surface_opacity_pct);
    theme_set_shadows_enabled(cfg->shadows_enabled);
}

static bool color_matches_hex(lv_color_t color, uint32_t hex)
{
    return (lv_color_to_u32(color) & 0xFFFFFFu) == clean_hex(hex);
}

static bool map_palette_color(lv_color_t from, lv_color_t *to)
{
    if (color_matches_hex(from, s_previous_palette.bg) || color_matches_hex(from, s_default_palette.bg)) {
        *to = theme_bg_color();
        return true;
    }
    if (color_matches_hex(from, s_previous_palette.surface) || color_matches_hex(from, s_default_palette.surface)) {
        *to = theme_surface_color();
        return true;
    }
    if (color_matches_hex(from, s_previous_palette.card) || color_matches_hex(from, s_default_palette.card)) {
        *to = theme_card_color();
        return true;
    }
    if (color_matches_hex(from, s_previous_palette.border) || color_matches_hex(from, s_default_palette.border)) {
        *to = theme_border_color();
        return true;
    }
    if (color_matches_hex(from, s_previous_palette.primary) || color_matches_hex(from, s_default_palette.primary)) {
        *to = theme_primary_color();
        return true;
    }
    if (color_matches_hex(from, s_previous_palette.accent) || color_matches_hex(from, s_default_palette.accent)) {
        *to = theme_accent_color();
        return true;
    }
    return false;
}

static void remap_local_color(lv_obj_t *obj, lv_style_prop_t prop, lv_style_selector_t selector)
{
    lv_style_value_t value;
    if (lv_obj_get_local_style_prop(obj, prop, &value, selector) != LV_STYLE_RES_FOUND) return;

    lv_color_t mapped;
    if (!map_palette_color(value.color, &mapped)) return;
    value.color = mapped;
    lv_obj_set_local_style_prop(obj, prop, value, selector);
}

static void recolor_one(lv_obj_t *obj)
{
    static const lv_style_selector_t selectors[] = {
        LV_PART_MAIN,
        LV_PART_MAIN | LV_STATE_CHECKED,
        LV_PART_MAIN | LV_STATE_PRESSED,
        LV_PART_MAIN | LV_STATE_FOCUSED,
        LV_PART_MAIN | LV_STATE_DISABLED,
        LV_PART_INDICATOR,
        LV_PART_KNOB,
        LV_PART_ITEMS,
        LV_PART_ITEMS | LV_STATE_CHECKED,
        LV_PART_SELECTED,
    };
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_GRAD_COLOR,
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_OUTLINE_COLOR,
        LV_STYLE_TEXT_COLOR,
        LV_STYLE_LINE_COLOR,
        LV_STYLE_ARC_COLOR,
        LV_STYLE_SHADOW_COLOR,
    };

    for (size_t selector_index = 0; selector_index < sizeof(selectors) / sizeof(selectors[0]); selector_index++) {
        for (size_t prop_index = 0; prop_index < sizeof(props) / sizeof(props[0]); prop_index++) {
            remap_local_color(obj, props[prop_index], selectors[selector_index]);
        }
    }
}

void theme_recolor_tree(lv_obj_t *root)
{
    if (!root) return;
    recolor_one(root);
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t child_index = 0; child_index < child_count; child_index++) {
        theme_recolor_tree(lv_obj_get_child(root, child_index));
    }
}

void theme_set_surface_opacity_pct(uint8_t pct)
{
    if (pct < 35) pct = 35;
    if (pct > 100) pct = 100;
    s_surface_opacity_pct = pct;
}

uint8_t theme_surface_opacity_pct(void)
{
    return s_surface_opacity_pct;
}

lv_opa_t theme_surface_opa(void)
{
    return (lv_opa_t)((s_surface_opacity_pct * 255u) / 100u);
}

bool theme_shadows_enabled(void)
{
    return s_shadows_enabled;
}

void theme_set_shadows_enabled(bool enabled)
{
    s_shadows_enabled = enabled;
}

/* Decorative card/panel shadow width: a uniform 6 px when on, 0 when off.
 * SW shadow blur is the dominant per-frame cost on this panel, so the Settings
 * toggle disables it outright. Objects opt in via LV_OBJ_FLAG_USER_1. */
int32_t theme_shadow_width(void)
{
    return s_shadows_enabled ? 6 : 0;
}

void theme_apply_shadows(lv_obj_t *root)
{
    if (!root) return;
    if (lv_obj_has_flag(root, LV_OBJ_FLAG_USER_1)) {
        lv_obj_set_style_shadow_width(root, theme_shadow_width(), LV_PART_MAIN);
    }
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t child_index = 0; child_index < child_count; child_index++) {
        theme_apply_shadows(lv_obj_get_child(root, child_index));
    }
}

void theme_style_glass_panel(lv_obj_t *obj, lv_coord_t radius)
{
    if (radius > 8) radius = 8;
    lv_obj_set_style_bg_color(obj, THEME_CARD_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, theme_surface_opa(), LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(obj, 0, LV_PART_MAIN);
}

void theme_apply_surface_opacity(lv_obj_t *root)
{
    if (!root) return;
    lv_color_t bg = lv_obj_get_style_bg_color(root, LV_PART_MAIN);
    if (lv_color_eq(bg, THEME_CARD_COLOR) || lv_color_eq(bg, THEME_SURFACE_COLOR)) {
        lv_obj_set_style_bg_opa(root, theme_surface_opa(), LV_PART_MAIN);
    }
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t child_index = 0; child_index < child_count; child_index++) {
        theme_apply_surface_opacity(lv_obj_get_child(root, child_index));
    }
}

void theme_apply_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, THEME_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr, THEME_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
}

lv_obj_t *theme_card_create(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    theme_style_glass_panel(c, 18);
    lv_obj_set_style_pad_all(c, 16, LV_PART_MAIN);
    /* Hide scrollbars; cards shouldn't scroll by default. */
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *theme_column_create(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(col, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    return col;
}

lv_obj_t *theme_header_create(lv_obj_t *parent, const char *title, const char *caption)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, 4, LV_PART_MAIN);

    lv_obj_t *t = lv_label_create(wrap);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(t, THEME_FONT_TITLE, LV_PART_MAIN);

    if (caption) {
        lv_obj_t *c = lv_label_create(wrap);
        lv_label_set_text(c, caption);
        lv_obj_set_style_text_color(c, THEME_TEXT_SECONDARY, LV_PART_MAIN);
        lv_obj_set_style_text_font(c, THEME_FONT_LABEL, LV_PART_MAIN);
    }
    return t;
}

lv_obj_t *theme_badge_create(lv_obj_t *parent, const char *text, lv_color_t bg)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(b, 4, LV_PART_MAIN);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, bg, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, THEME_FONT_SMALL, LV_PART_MAIN);
    return b;
}

lv_obj_t *theme_section_header(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, THEME_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, THEME_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_pad_top(l, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_left(l, 4, LV_PART_MAIN);
    return l;
}

lv_obj_t *theme_row_create(lv_obj_t *parent, lv_coord_t gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, gap, LV_PART_MAIN);
    return row;
}

lv_obj_t *theme_col_create(lv_obj_t *parent, lv_coord_t gap)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, gap, LV_PART_MAIN);
    return col;
}

void theme_btn_style_primary(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, THEME_PRIMARY_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(btn, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x006CDB), LV_PART_MAIN | LV_STATE_PRESSED);
}

void theme_btn_style_secondary(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, THEME_SURFACE_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, THEME_TEXT_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(btn, THEME_FONT_LABEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, THEME_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, THEME_BORDER_COLOR, LV_PART_MAIN | LV_STATE_PRESSED);
}
