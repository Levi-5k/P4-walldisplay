#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLED_EFFECT_PARAM_COUNT 5

typedef struct {
    uint8_t id;
    const char *name;
    const char *params[WLED_EFFECT_PARAM_COUNT];
} wled_effect_catalog_item_t;

size_t wled_effect_catalog_count(void);
const wled_effect_catalog_item_t *wled_effect_catalog_item(size_t index);
const wled_effect_catalog_item_t *wled_effect_catalog_find(uint8_t id);
int wled_effect_catalog_index_for_id(uint8_t id);
uint8_t wled_effect_catalog_id_for_index(uint16_t index);
uint8_t wled_effect_catalog_adjacent_id(uint8_t current_id, int delta);
const char *wled_effect_catalog_options(void);

#ifdef __cplusplus
}
#endif
