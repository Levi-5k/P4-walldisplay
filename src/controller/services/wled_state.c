#include "wled_state.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "wled_state";

#define MAX_WLED_STATE_SUBS 4

static wled_preset_item_t s_presets[WLED_PRESET_LIST_MAX];
static wled_segment_t s_segments[WLED_SEGMENT_LIST_MAX];
static wled_state_t s_state;
static struct {
    wled_state_cb_t cb;
    void *user;
} s_subs[MAX_WLED_STATE_SUBS];
static uint8_t s_sub_count;

void wled_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    memset(s_presets, 0, sizeof(s_presets));
    memset(s_segments, 0, sizeof(s_segments));
    s_state.presets = s_presets;
    s_state.segments = s_segments;
    s_state.ps = -1;
    s_state.pl = -1;
}

void wled_state_get(wled_state_t *out)
{
    if (!s_state.presets) s_state.presets = s_presets;
    if (!s_state.segments) s_state.segments = s_segments;
    if (out) *out = s_state;
}

esp_err_t wled_state_subscribe(wled_state_cb_t cb, void *user)
{
    if (!cb || s_sub_count >= MAX_WLED_STATE_SUBS) return ESP_ERR_INVALID_ARG;
    s_subs[s_sub_count].cb = cb;
    s_subs[s_sub_count].user = user;
    s_sub_count++;
    return ESP_OK;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

static bool json_bool(const cJSON *obj, const char *key, bool def)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    return def;
}

static void copy_preset_name(char *dst, size_t dst_len, const char *src, uint16_t id)
{
    if (!dst || dst_len == 0) return;
    if (src && src[0]) {
        snprintf(dst, dst_len, "%s", src);
    } else {
        snprintf(dst, dst_len, "Preset %u", (unsigned)id);
    }
    dst[dst_len - 1] = '\0';
}

static void copy_segment_name(char *dst, size_t dst_len, const char *src, uint8_t id)
{
    if (!dst || dst_len == 0) return;
    if (src && src[0]) {
        snprintf(dst, dst_len, "%s", src);
    } else {
        snprintf(dst, dst_len, "Segment %u", (unsigned)id);
    }
    dst[dst_len - 1] = '\0';
}

static void parse_presets(const cJSON *presets)
{
    s_state.preset_count = 0;
    s_state.presets_truncated = false;
    s_state.presets = s_presets;
    if (!cJSON_IsArray(presets)) return;

    const int count = cJSON_GetArraySize(presets);
    for (int i = 0; i < count; i++) {
        const cJSON *entry = cJSON_GetArrayItem(presets, i);
        int id = 0;
        const char *name = NULL;

        if (cJSON_IsArray(entry)) {
            const cJSON *id_item = cJSON_GetArrayItem(entry, 0);
            const cJSON *name_item = cJSON_GetArrayItem(entry, 1);
            if (cJSON_IsNumber(id_item)) id = id_item->valueint;
            if (cJSON_IsString(name_item)) name = name_item->valuestring;
        } else if (cJSON_IsObject(entry)) {
            const cJSON *id_item = cJSON_GetObjectItem(entry, "id");
            const cJSON *name_item = cJSON_GetObjectItem(entry, "n");
            if (cJSON_IsNumber(id_item)) id = id_item->valueint;
            if (cJSON_IsString(name_item)) name = name_item->valuestring;
        }

        if (id < 1 || id > 250) continue;
        if (s_state.preset_count >= WLED_PRESET_LIST_MAX) {
            s_state.presets_truncated = true;
            break;
        }

        wled_preset_item_t *out = &s_presets[s_state.preset_count++];
        out->id = (uint16_t)id;
        copy_preset_name(out->name, sizeof(out->name), name, out->id);
    }
}

static void parse_segment_colors(cJSON *seg, wled_segment_t *out)
{
    if (!seg || !out) return;
    cJSON *col_arr = cJSON_GetObjectItem(seg, "col");
    if (!cJSON_IsArray(col_arr)) return;

    for (int c = 0; c < 3 && c < cJSON_GetArraySize(col_arr); c++) {
        cJSON *rgb = cJSON_GetArrayItem(col_arr, c);
        if (!cJSON_IsArray(rgb)) continue;
        for (int ch = 0; ch < 3 && ch < cJSON_GetArraySize(rgb); ch++) {
            cJSON *v = cJSON_GetArrayItem(rgb, ch);
            if (cJSON_IsNumber(v)) out->col[c][ch] = (uint8_t)v->valueint;
        }
    }
}

static void parse_segments(const cJSON *seg_arr)
{
    s_state.segment_count = 0;
    s_state.segments_truncated = false;
    s_state.segments = s_segments;
    if (!cJSON_IsArray(seg_arr)) return;

    const int count = cJSON_GetArraySize(seg_arr);
    for (int i = 0; i < count; i++) {
        cJSON *seg = cJSON_GetArrayItem(seg_arr, i);
        if (!cJSON_IsObject(seg)) continue;

        if (s_state.segment_count >= WLED_SEGMENT_LIST_MAX) {
            s_state.segments_truncated = true;
            break;
        }

        wled_segment_t *out = &s_segments[s_state.segment_count];
        memset(out, 0, sizeof(*out));
        out->id = (uint8_t)json_int(seg, "id", i);
        copy_segment_name(out->name, sizeof(out->name), cJSON_GetStringValue(cJSON_GetObjectItem(seg, "n")), out->id);
        out->on = json_bool(seg, "on", true);
        out->selected = json_bool(seg, "sel", false);
        out->reverse = json_bool(seg, "rev", false);
        out->mirror = json_bool(seg, "mi", false);
        out->reverse_y = json_bool(seg, "rY", false);
        out->mirror_y = json_bool(seg, "mY", false);
        out->transpose = json_bool(seg, "tp", false);
        out->freeze = json_bool(seg, "frz", false);
        out->start = (uint16_t)json_int(seg, "start", 0);
        out->stop = (uint16_t)json_int(seg, "stop", 0);
        out->start_y = (uint16_t)json_int(seg, "startY", 0);
        out->stop_y = (uint16_t)json_int(seg, "stopY", 1);
        out->len = (uint16_t)json_int(seg, "len", out->stop > out->start ? out->stop - out->start : 0);
        out->group = (uint16_t)json_int(seg, "grp", 1);
        out->spacing = (uint16_t)json_int(seg, "spc", 0);
        out->offset = (uint16_t)json_int(seg, "of", 0);
        out->brightness = (uint8_t)json_int(seg, "bri", 255);
        out->set = (uint8_t)json_int(seg, "set", 0);
        out->sound_sim = (uint8_t)json_int(seg, "si", 0);
        out->map_1d_2d = (uint8_t)json_int(seg, "m12", 0);
        out->blend_mode = (uint8_t)json_int(seg, "bm", 0);
        out->fx = (uint8_t)json_int(seg, "fx", 0);
        out->pal = (uint8_t)json_int(seg, "pal", 0);
        out->sx = (uint8_t)json_int(seg, "sx", 0);
        out->ix = (uint8_t)json_int(seg, "ix", 0);
        out->c1 = (uint8_t)json_int(seg, "c1", 0);
        out->c2 = (uint8_t)json_int(seg, "c2", 0);
        out->c3 = (uint8_t)json_int(seg, "c3", 0);
        out->o1 = json_bool(seg, "o1", false);
        out->o2 = json_bool(seg, "o2", false);
        out->o3 = json_bool(seg, "o3", false);
        out->cct = (uint8_t)json_int(seg, "cct", 0);
        parse_segment_colors(seg, out);
        s_state.segment_count++;
    }
}

void wled_state_parse_json(const char *json, size_t len)
{
    if (!json || len == 0) return;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGD(TAG, "JSON parse failed");
        return;
    }

    cJSON *state = cJSON_GetObjectItem(root, "state");
    if (!state) state = root;

    if (cJSON_HasObjectItem(state, "on")) {
        s_state.on = json_bool(state, "on", s_state.on);
        s_state.bri = (uint8_t)json_int(state, "bri", s_state.bri);
        s_state.transition = (uint8_t)json_int(state, "transition", s_state.transition);
        s_state.mainseg = (uint8_t)json_int(state, "mainseg", s_state.mainseg);

        cJSON *seg_arr = cJSON_GetObjectItem(state, "seg");
        if (cJSON_IsArray(seg_arr) && cJSON_GetArraySize(seg_arr) > 0) {
            parse_segments(seg_arr);
            cJSON *seg0 = cJSON_GetArrayItem(seg_arr, 0);
            if (seg0) {
                if (s_state.segment_count > 0) {
                    const wled_segment_t *first = &s_segments[0];
                    s_state.seg0_fx = first->fx;
                    s_state.seg0_pal = first->pal;
                    s_state.seg0_sx = first->sx;
                    s_state.seg0_ix = first->ix;
                    s_state.seg0_c1 = first->c1;
                    s_state.seg0_c2 = first->c2;
                    s_state.seg0_c3 = first->c3;
                    s_state.seg0_o1 = first->o1;
                    s_state.seg0_o2 = first->o2;
                    s_state.seg0_o3 = first->o3;
                    s_state.seg0_cct = first->cct;
                    memcpy(s_state.seg0_col, first->col, sizeof(s_state.seg0_col));
                } else {
                s_state.seg0_fx  = (uint8_t)json_int(seg0, "fx", s_state.seg0_fx);
                s_state.seg0_pal = (uint8_t)json_int(seg0, "pal", s_state.seg0_pal);
                s_state.seg0_sx  = (uint8_t)json_int(seg0, "sx", s_state.seg0_sx);
                s_state.seg0_ix  = (uint8_t)json_int(seg0, "ix", s_state.seg0_ix);
                s_state.seg0_c1  = (uint8_t)json_int(seg0, "c1", s_state.seg0_c1);
                s_state.seg0_c2  = (uint8_t)json_int(seg0, "c2", s_state.seg0_c2);
                s_state.seg0_c3  = (uint8_t)json_int(seg0, "c3", s_state.seg0_c3);
                s_state.seg0_o1  = json_bool(seg0, "o1", s_state.seg0_o1);
                s_state.seg0_o2  = json_bool(seg0, "o2", s_state.seg0_o2);
                s_state.seg0_o3  = json_bool(seg0, "o3", s_state.seg0_o3);
                s_state.seg0_cct = (uint8_t)json_int(seg0, "cct", s_state.seg0_cct);

                cJSON *col_arr = cJSON_GetObjectItem(seg0, "col");
                if (cJSON_IsArray(col_arr)) {
                    for (int c = 0; c < 3 && c < cJSON_GetArraySize(col_arr); c++) {
                        cJSON *rgb = cJSON_GetArrayItem(col_arr, c);
                        if (cJSON_IsArray(rgb)) {
                            for (int ch = 0; ch < 3 && ch < cJSON_GetArraySize(rgb); ch++) {
                                cJSON *v = cJSON_GetArrayItem(rgb, ch);
                                if (cJSON_IsNumber(v))
                                    s_state.seg0_col[c][ch] = (uint8_t)v->valueint;
                            }
                        }
                    }
                }
                }
            }
        }
        s_state.valid = true;
        s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    if (cJSON_HasObjectItem(state, "ps")) {
        s_state.ps = (int16_t)json_int(state, "ps", s_state.ps);
        s_state.valid = true;
        s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    if (cJSON_HasObjectItem(state, "pl")) {
        s_state.pl = (int16_t)json_int(state, "pl", s_state.pl);
        s_state.valid = true;
        s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    if (cJSON_HasObjectItem(root, "presets")) {
        parse_presets(cJSON_GetObjectItem(root, "presets"));
        s_state.presets_truncated = json_bool(root, "ptrunc", s_state.presets_truncated);
        s_state.valid = true;
        s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    cJSON *info = cJSON_GetObjectItem(root, "info");
    if (info) {
        cJSON *ver = cJSON_GetObjectItem(info, "ver");
        if (cJSON_IsString(ver) && ver->valuestring) {
            strncpy(s_state.version, ver->valuestring, sizeof(s_state.version) - 1);
            s_state.version[sizeof(s_state.version) - 1] = '\0';
        }
        cJSON *ip = cJSON_GetObjectItem(info, "ip");
        if (cJSON_IsString(ip) && ip->valuestring) {
            strncpy(s_state.ip_addr, ip->valuestring, sizeof(s_state.ip_addr) - 1);
            s_state.ip_addr[sizeof(s_state.ip_addr) - 1] = '\0';
        }
        cJSON *leds = cJSON_GetObjectItem(info, "leds");
        if (leds) {
            s_state.led_count = (uint16_t)json_int(leds, "count", s_state.led_count);
        }
        cJSON *wifi = cJSON_GetObjectItem(info, "wifi");
        if (wifi) {
            s_state.wifi_rssi = (int16_t)json_int(wifi, "rssi", s_state.wifi_rssi);
            s_state.wifi_signal = (uint8_t)json_int(wifi, "signal", s_state.wifi_signal);
            s_state.wifi_channel = (uint8_t)json_int(wifi, "channel", s_state.wifi_channel);
            s_state.wifi_ap = json_bool(wifi, "ap", s_state.wifi_ap);
        }
        s_state.uptime_s = (uint32_t)json_int(info, "uptime", s_state.uptime_s);
        s_state.valid = true;
        s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }

    cJSON_Delete(root);

    if (s_state.valid) {
        for (uint8_t i = 0; i < s_sub_count; i++) {
            s_subs[i].cb(&s_state, s_subs[i].user);
        }
    }
}
