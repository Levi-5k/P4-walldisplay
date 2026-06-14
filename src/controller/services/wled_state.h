#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define WLED_PRESET_LIST_MAX 250
#define WLED_PRESET_NAME_MAX 25

typedef struct {
    uint16_t id;
    char     name[WLED_PRESET_NAME_MAX];
} wled_preset_item_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     on;
    uint8_t  bri;
    uint8_t  transition;
    int16_t  ps;
    int16_t  pl;
    uint16_t preset_count;
    bool     presets_truncated;
    const wled_preset_item_t *presets;

    /* Segment 0 state */
    uint8_t  seg0_fx;
    uint8_t  seg0_pal;
    uint8_t  seg0_sx;
    uint8_t  seg0_ix;
    uint8_t  seg0_c1;
    uint8_t  seg0_c2;
    uint8_t  seg0_c3;
    bool     seg0_o1;
    bool     seg0_o2;
    bool     seg0_o3;
    uint8_t  seg0_cct;
    uint8_t  seg0_col[3][3];

    /* Info */
    char     version[24];
    uint16_t led_count;
    uint32_t uptime_s;

    bool     valid;
    uint32_t last_update_ms;
} wled_state_t;

typedef void (*wled_state_cb_t)(const wled_state_t *state, void *user);

void wled_state_init(void);
void wled_state_parse_json(const char *json, size_t len);
void wled_state_get(wled_state_t *out);
esp_err_t wled_state_subscribe(wled_state_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
