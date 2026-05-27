#include "wled_state.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "wled_state";

#define MAX_WLED_STATE_SUBS 4

static wled_state_t s_state;
static struct {
    wled_state_cb_t cb;
    void *user;
} s_subs[MAX_WLED_STATE_SUBS];
static uint8_t s_sub_count;

void wled_state_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

void wled_state_get(wled_state_t *out)
{
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
    return def;
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

        cJSON *seg_arr = cJSON_GetObjectItem(state, "seg");
        if (cJSON_IsArray(seg_arr) && cJSON_GetArraySize(seg_arr) > 0) {
            cJSON *seg0 = cJSON_GetArrayItem(seg_arr, 0);
            if (seg0) {
                s_state.seg0_fx  = (uint8_t)json_int(seg0, "fx", s_state.seg0_fx);
                s_state.seg0_pal = (uint8_t)json_int(seg0, "pal", s_state.seg0_pal);
                s_state.seg0_sx  = (uint8_t)json_int(seg0, "sx", s_state.seg0_sx);
                s_state.seg0_ix  = (uint8_t)json_int(seg0, "ix", s_state.seg0_ix);
                s_state.seg0_c1  = (uint8_t)json_int(seg0, "c1", s_state.seg0_c1);
                s_state.seg0_c2  = (uint8_t)json_int(seg0, "c2", s_state.seg0_c2);
                s_state.seg0_c3  = (uint8_t)json_int(seg0, "c3", s_state.seg0_c3);
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
        cJSON *leds = cJSON_GetObjectItem(info, "leds");
        if (leds) {
            s_state.led_count = (uint16_t)json_int(leds, "count", s_state.led_count);
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
