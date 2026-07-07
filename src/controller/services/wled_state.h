#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define WLED_PRESET_LIST_MAX 250
#define WLED_PRESET_NAME_MAX 25
#define WLED_SEGMENT_LIST_MAX 32
#define WLED_SEGMENT_NAME_MAX 33

typedef struct {
    uint16_t id;
    char     name[WLED_PRESET_NAME_MAX];
} wled_preset_item_t;

typedef struct {
    uint8_t  id;
    char     name[WLED_SEGMENT_NAME_MAX];
    bool     on;
    bool     selected;
    bool     reverse;
    bool     mirror;
    bool     reverse_y;
    bool     mirror_y;
    bool     transpose;
    bool     freeze;
    uint16_t start;
    uint16_t stop;
    uint16_t start_y;
    uint16_t stop_y;
    uint16_t len;
    uint16_t group;
    uint16_t spacing;
    uint16_t offset;
    uint8_t  brightness;
    uint8_t  set;
    uint8_t  sound_sim;
    uint8_t  map_1d_2d;
    uint8_t  blend_mode;
    uint8_t  fx;
    uint8_t  pal;
    uint8_t  sx;
    uint8_t  ix;
    uint8_t  c1;
    uint8_t  c2;
    uint8_t  c3;
    bool     o1;
    bool     o2;
    bool     o3;
    uint8_t  cct;
    uint8_t  col[3][3];
} wled_segment_t;

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
    uint8_t  mainseg;
    uint8_t  segment_count;
    bool     segments_truncated;
    const wled_segment_t *segments;

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
    char     ip_addr[16];
    uint16_t led_count;
    uint32_t uptime_s;
    int16_t  wifi_rssi;
    uint8_t  wifi_signal;
    uint8_t  wifi_channel;
    bool     wifi_ap;

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
