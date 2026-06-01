#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of representative colors shown in a palette preview strip. */
#define WLED_PALETTE_PREVIEW_COUNT 4

typedef struct {
    uint8_t  id;                                   /* WLED palette index */
    const char *name;                              /* display name */
    uint32_t preview[WLED_PALETTE_PREVIEW_COUNT];  /* 0xRRGGBB swatch hints */
} wled_palette_catalog_item_t;

size_t wled_palette_catalog_count(void);
const wled_palette_catalog_item_t *wled_palette_catalog_item(size_t index);
const wled_palette_catalog_item_t *wled_palette_catalog_find(uint8_t id);
int wled_palette_catalog_index_for_id(uint8_t id);
uint8_t wled_palette_catalog_id_for_index(uint16_t index);

/* Newline-separated palette names for lv_dropdown_set_options(). */
const char *wled_palette_catalog_options(void);

#ifdef __cplusplus
}
#endif
