#include "ui_background.h"

#include "app_config.h"
#include "jpeg_conv.h"
#include "jpeg_hw.h"
#include "services.h"
#include "sd_storage.h"
#include "theme.h"
#include "toast.h"

#include "bsp/esp-bsp.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BG_DIR             "/sdcard/walldisplay_theme"
/* Per-preset file naming: p<preset>_bg<slot>.raw (HW-decoded RGB565) */
#define BG_FILE_FMT        BG_DIR "/p%u_bg%u.raw"
#define BG_TMP_FMT         BG_DIR "/p%u_bg%u.tmp"
#define BG_MAX_BYTES       (2u * 1024u * 1024u)
#define BG_MIN_BYTES       4u
#define BG_HTTP_BUFFER_BYTES 2048
#define BG_HTTP_BUFFER_MIN_BYTES 512
#define BG_READ_YIELD_MS    0
#define BG_POST_HTTP_SETTLE_MS 0
#define BG_INTER_IMAGE_COOLDOWN_MS 0
#define BG_FILE_CHUNK_BYTES 4096
#define BG_PROGRESS_STEP_BYTES (48u * 1024u)
#define BG_TASK_STACK      16384
#define BG_HTTP_TIMEOUT_MS 20000
/* Bump this when the JPEG converter changes in a way that invalidates
 * previously-converted files (e.g. the v1 pixel-domain converter produced
 * corrupted output; v2 is the lossless transcoder).  On mismatch the
 * auto-download task deletes all images and re-downloads them.              */
#define BG_CONV_VERSION    19
#define BG_NVS_NS          "ui_bg"
#define BG_NVS_KEY_CONV    "conv_ver"
#define BG_NVS_KEY_CONV_PRESET_FMT "conv_p%u"
#define BG_NVS_KEY_DL_FAIL "dl_fail"   /* download crash counter */
#define BG_NVS_KEY_DL_DONE "dl_done"   /* bitmask: bit N = image N saved OK */
#define BG_NVS_KEY_DL_PSET "dl_pset"   /* preset index being downloaded */
#define BG_MAX_DL_FAILURES 3           /* crash-recovery retries before giving up */
#define BG_DL_MAX_RETRIES  2           /* per-image HTTP retries within one run */

static const char *TAG = "ui_bg";

/* ------------------------------------------------------------------ */
/*  Background preset table (shared with screens.c via accessors)     */
/* ------------------------------------------------------------------ */
static const bg_preset_t s_presets[] = {
    {"Nature", {
        "https://images.unsplash.com/photo-1500530855697-b586d89ba3ee?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1441974231531-c6227db76b6e?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1506744038136-46273834b3fb?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1470770841072-f978cf4d019e?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1469474968028-56623f02e42e?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1501785888041-af3ef285b470?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1500534314209-a25ddb2bd429?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1511884642898-4c92249e20b6?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
    }},
    {"City", {
        "https://images.unsplash.com/photo-1449824913935-59a10b8d2000?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1519501025264-65ba15a82390?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1494526585095-c41746248156?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1518005020951-eccb494ad742?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1477959858617-67f85cf4f1df?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1496568816309-51d7c20e3b21?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1514565131-fce0801e5785?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1514924013411-cbf25faa35bb?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
    }},
    {"Space", {
        "https://images.unsplash.com/photo-1462331940025-496dfbfc7564?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1446776811953-b23d57bd21aa?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1451187580459-43490279c0fa?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1419242902214-272b3f66ee7a?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1447433819943-74a20887a81e?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1506318137071-a8e063b4bec0?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1516339901601-2e1b62dc0c45?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1534796636912-3b95b3ab5986?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
    }},
    {"Abstract", {
        "https://images.unsplash.com/photo-1557683316-973673baf926?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1557682250-33bd709cbe85?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1550859492-d5da9d8e45f3?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1541701494587-cb58502866ab?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1558591710-4b4a1ae0f04d?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1558470598-a5dda9640f68?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1618005182384-a83a8bd57fbe?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1614850715649-1d0106293bd1?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
    }},
    {"Ocean", {
        "https://images.unsplash.com/photo-1507525428034-b723cf961d3e?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1471922694854-ff1b63b20054?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1500375592092-40eb2168fd21?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1439405326854-014607f694d7?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1505142468610-359e7d316be0?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1518837695005-2083093ee35b?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1500534623283-312aade485b7?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1519046904884-53103b34b206?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
    }},
    {"Weather", {
        "https://images.unsplash.com/photo-1504608524841-42fe6f032b4b?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1534088568595-a066f410bcda?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1501594907352-04cda38ebc29?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1499346030926-9a72daac6c63?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1527482797697-8795b05a13fe?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1504384308090-c894fdcc538d?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1515694346937-94d85e41e6f0?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
        "https://images.unsplash.com/photo-1519681393784-d120267933ba?auto=format&fit=crop&w=720&h=720&q=100&fm=jpg",
    }},
};

#define PRESET_COUNT  ((uint8_t)(sizeof(s_presets) / sizeof(s_presets[0])))

uint8_t bg_preset_count(void) { return PRESET_COUNT; }

const bg_preset_t *bg_preset_get(uint8_t index)
{
    return (index < PRESET_COUNT) ? &s_presets[index] : &s_presets[0];
}

uint8_t bg_preset_url_count(uint8_t index)
{
    const bg_preset_t *p = bg_preset_get(index);
    uint8_t n = 0;
    for (uint8_t i = 0; i < APP_THEME_MAX_IMAGES; i++) {
        if (p->urls[i] && p->urls[i][0]) n++;
    }
    return n ? n : 1;
}

const char *bg_preset_dropdown_options(void)
{
    /* must match the order of s_presets[] */
    return "Nature\nCity\nSpace\nAbstract\nOcean\nWeather";
}

/* When true, SD card access is blocked to allow HTTPS downloads to work
 * (the SDMMC host can't handle Slot 0 and Slot 1 concurrently).          */
static bool s_migration_pending;

/* Support background on multiple screens (main + idle) */
#define BG_MAX_SCREENS 2
typedef struct {
    lv_obj_t *layer;
    lv_obj_t *image;
    lv_obj_t *scrim;
    bool idle_weather;
} bg_screen_t;
static bg_screen_t s_screens[BG_MAX_SCREENS];
static uint8_t s_screen_count;
static lv_timer_t *s_timer;
static uint8_t s_active_slot;

/* Raw RGB565 image loaded from SD for LVGL display */
static uint8_t *s_raw_pixels;
static lv_image_dsc_t s_raw_dsc;
static bool s_sd_background_allowed;
static bool s_deferred_logged;
static bool s_busy;
static bool s_progress_has_pct;
static uint8_t s_progress_pct;
static uint8_t s_progress_index;
static uint8_t s_progress_total;
static char s_status[120] = "Background ready";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void *bg_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return ptr;
}

static void *bg_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(count, size);
    return ptr;
}

static void *bg_realloc(void *ptr, size_t size)
{
    void *next = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) next = realloc(ptr, size);
    return next;
}

static bool file_exists(const char *path)
{
    return sd_storage_file_exists(path);
}

static void background_cache_version_key(uint8_t preset_index, char *key, size_t key_size)
{
    snprintf(key, key_size, BG_NVS_KEY_CONV_PRESET_FMT, (unsigned)preset_index);
}

static bool background_cache_version_current(uint8_t preset_index)
{
    char key[16];
    background_cache_version_key(preset_index, key, sizeof(key));

    nvs_handle_t h;
    uint8_t ver = 0;
    if (nvs_open(BG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_u8(h, key, &ver);
    nvs_close(h);
    return err == ESP_OK && ver == BG_CONV_VERSION;
}

static void mark_background_cache_current(uint8_t preset_index)
{
    char key[16];
    background_cache_version_key(preset_index, key, sizeof(key));

    nvs_handle_t h;
    if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, BG_NVS_KEY_CONV, BG_CONV_VERSION);
        nvs_set_u8(h, key, BG_CONV_VERSION);
        nvs_erase_key(h, BG_NVS_KEY_DL_DONE);
        nvs_erase_key(h, BG_NVS_KEY_DL_PSET);
        nvs_set_u8(h, BG_NVS_KEY_DL_FAIL, 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void clear_download_state_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, BG_NVS_KEY_DL_FAIL, 0);
        nvs_erase_key(h, BG_NVS_KEY_DL_DONE);
        nvs_erase_key(h, BG_NVS_KEY_DL_PSET);
        nvs_erase_key(h, BG_NVS_KEY_CONV);
        for (uint8_t i = 0; i < PRESET_COUNT; i++) {
            char key[16];
            background_cache_version_key(i, key, sizeof(key));
            nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

static int bg_recursive_delete(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return remove(path);

    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *ent;
    char child[160];
    int ret = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (!ent->d_name[0] || strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (bg_recursive_delete(child) != 0) ret = -1;
    }
    closedir(dir);
    if (rmdir(path) != 0 && errno != ENOENT) ret = -1;
    return ret;
}

static uint8_t opacity_from_percent(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return (uint8_t)((pct * 255u) / 100u);
}

static void stop_timer(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

static void drop_current_image_cache(void)
{
    for (uint8_t i = 0; i < s_screen_count; i++) {
        if (!s_screens[i].image) continue;
        const void *src = lv_image_get_src(s_screens[i].image);
        if (src) lv_image_cache_drop(src);
    }
}

static bool find_existing_slot(const app_theme_config_t *cfg, uint8_t start, uint8_t *slot)
{
    if (!cfg || cfg->image_count == 0) return false;
    uint8_t preset = cfg->background_preset;
    if (preset >= bg_preset_count()) preset = 0;
    uint8_t count = cfg->image_count;
    if (count > APP_THEME_MAX_IMAGES) count = APP_THEME_MAX_IMAGES;
    for (uint8_t offset = 0; offset < count; offset++) {
        uint8_t candidate = (uint8_t)((start + offset) % count);
        char path[80];
        snprintf(path, sizeof(path), BG_FILE_FMT, (unsigned)preset, (unsigned)candidate);
        if (file_exists(path)) {
            if (slot) *slot = candidate;
            return true;
        }
    }
    return false;
}

static void apply_background(void);

/**
 * Forcibly release all SD-backed background images from LVGL.
 *
 * LVGL's stdio FS driver reads JPEG files directly from SD via VFS,
 * completely bypassing sd_storage_ensure_mounted().  If a background
 * image is displayed when HTTPS starts, LVGL's render thread will read
 * from SD on every frame — causing Slot 0/Slot 1 collision and crash.
 *
 * Must be called from the LVGL thread (or with bsp_display_lock held).
 */
static void clear_background_for_download(void)
{
    s_sd_background_allowed = false;
    s_deferred_logged = false;
    stop_timer();
    drop_current_image_cache();
    for (uint8_t i = 0; i < s_screen_count; i++) {
        bg_screen_t *s = &s_screens[i];
        if (!s->layer || !s->image || !s->scrim) continue;
        lv_image_set_src(s->image, NULL);
        lv_obj_add_flag(s->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s->layer, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->layer, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->scrim, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    free(s_raw_pixels);
    s_raw_pixels = NULL;
    memset(&s_raw_dsc, 0, sizeof(s_raw_dsc));
    ESP_LOGI(TAG, "background images cleared for download");
}

static void slide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    app_theme_config_t cfg;
    app_config_theme_load(&cfg);
    if (cfg.image_count == 0) return;
    uint8_t count = cfg.image_count > APP_THEME_MAX_IMAGES ? APP_THEME_MAX_IMAGES : cfg.image_count;
    s_active_slot = (uint8_t)((s_active_slot + 1) % count);
    apply_background();
}

static void apply_background(void)
{
    app_theme_config_t cfg;
    app_config_theme_load(&cfg);
    theme_apply_config(&cfg);

    if (s_screen_count == 0) {
        ESP_LOGW(TAG, "apply_background: no screens attached");
        return;
    }

    stop_timer();
    drop_current_image_cache();

    /* Reset all attached screens to default state */
    for (uint8_t i = 0; i < s_screen_count; i++) {
        bg_screen_t *s = &s_screens[i];
        if (!s->layer || !s->image || !s->scrim) continue;
        lv_obj_add_flag(s->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s->layer, THEME_BG_COLOR, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->layer, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->scrim, LV_OPA_TRANSP, LV_PART_MAIN);
    }

    if (!cfg.background_enabled || cfg.image_count == 0) {
        return;
    }

    if (!s_sd_background_allowed) {
        if (!s_deferred_logged) {
            ESP_LOGI(TAG, "SD-backed background deferred until network quiet");
            s_deferred_logged = true;
        }
        return;
    }

    if (sd_storage_ensure_mounted() != ESP_OK) {
        return;
    }

    uint8_t slot = 0;
    if (!find_existing_slot(&cfg, s_active_slot, &slot)) {
        return;
    }
    s_active_slot = slot;

    uint8_t preset = cfg.background_preset;
    if (preset >= bg_preset_count()) preset = 0;

    char raw_path[96];
    snprintf(raw_path, sizeof(raw_path), BG_FILE_FMT, (unsigned)preset, (unsigned)slot);

    ESP_LOGI(TAG, "apply_background: %s (slot %u, preset %u)", raw_path,
             (unsigned)slot, (unsigned)preset);

    /* Load raw RGB565 file: [u16 w][u16 h][u16 padded_w][u16 padded_h][pixels] */
    FILE *rf = fopen(raw_path, "rb");
    if (!rf) {
        ESP_LOGW(TAG, "apply_background: cannot open %s", raw_path);
        return;
    }
    uint16_t hdr[4];
    if (fread(hdr, 1, sizeof(hdr), rf) != sizeof(hdr)) {
        fclose(rf);
        return;
    }
    uint16_t img_w = hdr[0], img_h = hdr[1];
    uint16_t pad_w = hdr[2], pad_h = hdr[3];
    size_t pixel_bytes = (size_t)pad_w * pad_h * 2;
    uint8_t *new_pixels = heap_caps_malloc(pixel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_pixels) {
        fclose(rf);
        ESP_LOGE(TAG, "apply_background: OOM for %ux%u pixels", pad_w, pad_h);
        return;
    }
    size_t got = fread(new_pixels, 1, pixel_bytes, rf);
    fclose(rf);
    if (got != pixel_bytes) {
        free(new_pixels);
        ESP_LOGE(TAG, "apply_background: short read %u/%u", (unsigned)got, (unsigned)pixel_bytes);
        return;
    }

    /* Swap in the new pixel buffer */
    free(s_raw_pixels);
    s_raw_pixels = new_pixels;
    memset(&s_raw_dsc, 0, sizeof(s_raw_dsc));
    s_raw_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_raw_dsc.header.w = pad_w;
    s_raw_dsc.header.h = pad_h;
    s_raw_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_raw_dsc.header.stride = pad_w * 2;
    s_raw_dsc.data_size = (uint32_t)pixel_bytes;
    s_raw_dsc.data = s_raw_pixels;

    bool applied_to_any_screen = false;
    for (uint8_t i = 0; i < s_screen_count; i++) {
        bg_screen_t *s = &s_screens[i];
        if (!s->layer || !s->image || !s->scrim) continue;
        if (cfg.background_idle_only && !s->idle_weather) continue;
        lv_image_set_src(s->image, &s_raw_dsc);
        lv_obj_clear_flag(s->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_background(s->layer);
        lv_obj_set_style_bg_color(s->scrim, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s->scrim, opacity_from_percent(cfg.background_dim_pct), LV_PART_MAIN);
        applied_to_any_screen = true;
    }

    if (applied_to_any_screen && cfg.image_count > 1) {
        uint32_t period = cfg.slideshow_seconds;
        if (period < 5) period = 5;
        s_timer = lv_timer_create(slide_timer_cb, period * 1000u, NULL);
    }
}

void ui_background_pre_init(void)
{
    esp_err_t hw_err = jpeg_hw_init();
    if (hw_err != ESP_OK) {
        ESP_LOGW(TAG, "pre-init: HW JPEG decoder unavailable: %s", esp_err_to_name(hw_err));
    }

    /* Check NVS before the UI starts so migration/download state is known
     * while internal memory is still relatively unfragmented. */
    nvs_handle_t h;
    uint8_t ver = 0;
    uint8_t dl_fail = 0;

    if (nvs_open(BG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, BG_NVS_KEY_CONV, &ver);       /* 0 if key absent */
        nvs_get_u8(h, BG_NVS_KEY_DL_FAIL, &dl_fail); /* 0 if key absent */
        nvs_close(h);
    }
    /* ver stays 0 when the namespace doesn't exist yet (first boot). */

    if (ver != BG_CONV_VERSION && dl_fail < BG_MAX_DL_FAILURES) {
        s_migration_pending = true;
        ESP_LOGW(TAG, "pre-init: migration pending (conv %u!=%u, fails %u)",
                 ver, BG_CONV_VERSION, dl_fail);
    }
}

static void ui_background_attach_common(lv_obj_t *screen, bool idle_weather)
{
    if (!screen || s_screen_count >= BG_MAX_SCREENS) return;

    bg_screen_t *s = &s_screens[s_screen_count];
    s->idle_weather = idle_weather;

    s->layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s->layer);
    lv_obj_set_size(s->layer, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s->layer, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s->layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s->layer, THEME_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->layer, LV_OPA_COVER, LV_PART_MAIN);

    s->image = lv_image_create(s->layer);
    lv_obj_set_size(s->image, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(s->image, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(s->image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s->image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s->scrim = lv_obj_create(s->layer);
    lv_obj_remove_style_all(s->scrim);
    lv_obj_set_size(s->scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s->scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_move_background(s->layer);
    s_screen_count++;
    apply_background();
}

void ui_background_attach(lv_obj_t *screen)
{
    ui_background_attach_common(screen, false);
}

void ui_background_attach_idle_weather(lv_obj_t *screen)
{
    ui_background_attach_common(screen, true);
}

void ui_background_refresh(void)
{
    s_sd_background_allowed = true;
    s_deferred_logged = false;
    apply_background();
}

bool ui_background_is_busy(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_busy;
    portEXIT_CRITICAL(&s_state_lock);
    return busy;
}

const char *ui_background_status(void)
{
    return s_status;
}

void ui_background_download_state_get(ui_background_download_state_t *out)
{
    if (!out) return;

    portENTER_CRITICAL(&s_state_lock);
    out->busy = s_busy;
    out->has_progress = s_progress_has_pct;
    out->progress_pct = s_progress_pct;
    out->image_index = s_progress_index;
    out->image_total = s_progress_total;
    memcpy(out->status, s_status, sizeof(out->status));
    out->status[sizeof(out->status) - 1] = '\0';
    portEXIT_CRITICAL(&s_state_lock);
}

bool ui_background_preset_images_present(uint8_t preset_index,
                                         uint8_t *present_count,
                                         uint8_t *total_count)
{
    if (preset_index >= bg_preset_count()) preset_index = 0;
    uint8_t total = bg_preset_url_count(preset_index);
    uint8_t present = 0;

    for (uint8_t i = 0; i < total; i++) {
        char path[80];
        snprintf(path, sizeof(path), BG_FILE_FMT, (unsigned)preset_index, (unsigned)i);
        if (file_exists(path)) present++;
    }

    if (present_count) *present_count = present;
    if (total_count) *total_count = total;
    return total > 0 && present >= total && background_cache_version_current(preset_index);
}

static void set_busy(bool busy)
{
    portENTER_CRITICAL(&s_state_lock);
    s_busy = busy;
    if (!busy) {
        s_progress_has_pct = false;
        s_progress_pct = 0;
        s_progress_index = 0;
        s_progress_total = 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static bool try_start_download(void)
{
    bool started = false;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_busy) {
        s_busy = true;
        started = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return started;
}

static void set_statusf(const char *fmt, ...)
{
    char text[sizeof(s_status)];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    portENTER_CRITICAL(&s_state_lock);
    snprintf(s_status, sizeof(s_status), "%s", text);
    portEXIT_CRITICAL(&s_state_lock);
}

static void set_progress(uint8_t image_index, uint8_t image_total,
                         bool has_pct, uint8_t pct)
{
    if (image_total == 0) image_total = 1;
    if (image_index == 0) image_index = 1;
    if (image_index > image_total) image_index = image_total;
    if (pct > 100) pct = 100;

    portENTER_CRITICAL(&s_state_lock);
    s_progress_index = image_index;
    s_progress_total = image_total;
    s_progress_has_pct = has_pct;
    s_progress_pct = pct;
    portEXIT_CRITICAL(&s_state_lock);
}

static void set_item_progress(uint8_t index, uint8_t total, uint8_t item_pct)
{
    if (total == 0) total = 1;
    if (item_pct > 100) item_pct = 100;
    unsigned overall = (((unsigned)index * 100u) + item_pct) / total;
    if (overall > 100u) overall = 100u;
    set_progress((uint8_t)(index + 1u), total, true, (uint8_t)overall);
}

static bool copy_trimmed_url(char *out, size_t out_len, const char *url)
{
    if (!out || out_len == 0 || !url) return false;
    while (*url && isspace((unsigned char)*url)) url++;
    const char *end = url + strlen(url);
    while (end > url && isspace((unsigned char)end[-1])) end--;
    size_t len = (size_t)(end - url);
    if (len == 0 || len >= out_len) return false;
    for (size_t i = 0; i < len; i++) {
        if (url[i] == '\n' || url[i] == '\r') return false;
    }
    memcpy(out, url, len);
    out[len] = '\0';
    return true;
}

static bool valid_url(const char *url)
{
    return url && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool url_looks_jpeg(const char *url)
{
    if (!url) return false;
    size_t len = strcspn(url, "?#");
    if (len >= 4) {
        const char *ext = url + len - 4;
        if (tolower((unsigned char)ext[0]) == '.' &&
            tolower((unsigned char)ext[1]) == 'j' &&
            tolower((unsigned char)ext[2]) == 'p' &&
            tolower((unsigned char)ext[3]) == 'g') {
            return true;
        }
    }
    if (len >= 5) {
        const char *ext = url + len - 5;
        if (tolower((unsigned char)ext[0]) == '.' &&
            tolower((unsigned char)ext[1]) == 'j' &&
            tolower((unsigned char)ext[2]) == 'p' &&
            tolower((unsigned char)ext[3]) == 'e' &&
            tolower((unsigned char)ext[4]) == 'g') {
            return true;
        }
    }
    return false;
}

static bool content_type_is_jpeg(const char *content_type)
{
    if (!content_type) return false;
    char lower[96];
    size_t i = 0;
    for (; content_type[i] && i < sizeof(lower) - 1; i++) {
        lower[i] = (char)tolower((unsigned char)content_type[i]);
    }
    lower[i] = '\0';
    return strstr(lower, "image/jpeg") || strstr(lower, "image/jpg");
}

static bool content_type_is_text_response(const char *content_type)
{
    if (!content_type) return false;
    char lower[96];
    size_t i = 0;
    for (; content_type[i] && i < sizeof(lower) - 1; i++) {
        lower[i] = (char)tolower((unsigned char)content_type[i]);
    }
    lower[i] = '\0';
    return strstr(lower, "text/") || strstr(lower, "application/json") ||
           strstr(lower, "application/xml") || strstr(lower, "application/xhtml");
}

static void format_download_error(char *out, size_t out_len, esp_err_t err, int http_status)
{
    if (!out || out_len == 0) return;
    if (http_status > 0 && http_status != 200) {
        snprintf(out, out_len, "HTTP %d downloading image", http_status);
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(out, out_len, "Connect Wi-Fi first");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(out, out_len, "Use an http/https JPG URL");
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(out, out_len, "%s", sd_storage_last_error());
    } else if (err == ESP_ERR_INVALID_SIZE) {
        snprintf(out, out_len, "JPG too large; 2 MB max");
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        snprintf(out, out_len, "Use a valid JPG image URL");
    } else if (err == ESP_ERR_TIMEOUT) {
        snprintf(out, out_len, "Image download timed out");
    } else if (err == ESP_ERR_NO_MEM) {
        snprintf(out, out_len, "Memory tight; skipped image");
    } else if (err == ESP_FAIL) {
        const char *sd_error = sd_storage_last_error();
        snprintf(out, out_len, "%s", (sd_error && strcmp(sd_error, "SD ready") != 0) ? sd_error : "SD write failed");
    } else {
        snprintf(out, out_len, "Background failed: %s", esp_err_to_name(err));
    }
}

static void set_progress_status(uint8_t index, uint8_t total, size_t bytes, int64_t content_len)
{
    if (content_len > 0) {
        unsigned pct = (unsigned)((bytes * 100u) / (size_t)content_len);
        if (pct > 100) pct = 100;
        set_item_progress(index, total, (uint8_t)pct);
        set_statusf("Image %u/%u: %u%% (%u KB)",
                    (unsigned)(index + 1), (unsigned)total, pct, (unsigned)(bytes / 1024u));
    } else {
        set_item_progress(index, total, 0);
        set_statusf("Image %u/%u: %u KB",
                    (unsigned)(index + 1), (unsigned)total, (unsigned)(bytes / 1024u));
    }
}

/**
 * Download a JPEG from @p url into a PSRAM buffer.
 *
 * Reuses one keep-alive client for the batch.  Closing the TLS socket after
 * each image can leave ESP-Hosted SDIO in an unrecoverable state; defer that
 * close until every image has already been saved.
 *
 * Reads run without artificial pacing; the batch-level network/SD gates keep
 * SD card access out of the HTTPS phase.
 */
static esp_err_t download_http_jpeg_to_memory(const char *url,
                                              uint8_t index,
                                              uint8_t total,
                                              size_t http_buffer_size,
                                              esp_http_client_handle_t *p_client,
                                              uint8_t **out_data,
                                              size_t *out_bytes,
                                              int *out_http_status)
{
    esp_err_t err = ESP_OK;
    int http_status = 0;
    size_t bytes = 0;
    size_t capacity = 0;
    uint8_t *data = NULL;
    int64_t content_len = -1;
    bool signature_checked = false;

    if (out_data) *out_data = NULL;
    if (out_bytes) *out_bytes = 0;

    set_statusf("Image %u/%u: connecting", (unsigned)(index + 1), (unsigned)total);

    /* Reuse the batch client when possible. */
    esp_http_client_handle_t client = *p_client;
    if (!client) {
        esp_http_client_config_t http_cfg = {
            .url = url,
            .timeout_ms = BG_HTTP_TIMEOUT_MS,
            .buffer_size = (int)http_buffer_size,
            .buffer_size_tx = 512,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .max_redirection_count = 3,
            .user_agent = "P4-WallDisplay/1.0",
            .keep_alive_enable = true,
        };
        client = esp_http_client_init(&http_cfg);
        if (!client) return ESP_ERR_NO_MEM;
        (void)esp_http_client_set_header(client, "Accept",
            "image/jpeg,image/jpg;q=0.9,image/*;q=0.4,*/*;q=0.1");
        ESP_LOGI(TAG, "dl: new keep-alive HTTP client for %s", url);
    } else {
        esp_http_client_set_url(client, url);
    }

    bool client_open = false;

    err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        client_open = true;
        content_len = esp_http_client_fetch_headers(client);
        http_status = esp_http_client_get_status_code(client);
        if (out_http_status) *out_http_status = http_status;

        /* Follow redirect manually with a pause between connections */
        if (http_status >= 300 && http_status < 400) {
            char *location = NULL;
            esp_http_client_get_header(client, "Location", &location);
            if (location) {
                ESP_LOGI(TAG, "dl: redirect %d -> %.120s", http_status, location);
                esp_http_client_close(client);
                client_open = false;
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_http_client_set_url(client, location);
                err = esp_http_client_open(client, 0);
                if (err == ESP_OK) {
                    client_open = true;
                    content_len = esp_http_client_fetch_headers(client);
                    http_status = esp_http_client_get_status_code(client);
                    if (out_http_status) *out_http_status = http_status;
                }
            }
        }

        if (http_status != 200) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else if (content_len > (int64_t)BG_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            char *content_type = NULL;
            bool have_type = esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK && content_type;
            if (have_type && !content_type_is_jpeg(content_type) &&
                !url_looks_jpeg(url) && content_type_is_text_response(content_type)) {
                ESP_LOGW(TAG, "rejecting non-JPEG content-type: %s", content_type);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }

    if (err == ESP_OK) {
        capacity = content_len > 0 ? (size_t)content_len : (128u * 1024u);
        if (capacity < http_buffer_size) capacity = http_buffer_size;
        if (capacity > BG_MAX_BYTES) capacity = BG_MAX_BYTES;
        data = bg_malloc(capacity);
        if (!data) err = ESP_ERR_NO_MEM;
    }

    int empty_reads = 0;
    size_t last_progress_bytes = 0;
    uint8_t last_progress_pct = 255;
    while (err == ESP_OK) {
        if (bytes >= capacity) {
            size_t next_capacity = capacity * 2u;
            if (next_capacity < capacity + http_buffer_size) next_capacity = capacity + http_buffer_size;
            if (next_capacity > BG_MAX_BYTES) next_capacity = BG_MAX_BYTES;
            if (next_capacity <= capacity) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            uint8_t *next = bg_realloc(data, next_capacity);
            if (!next) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            data = next;
            capacity = next_capacity;
        }

        size_t available = capacity - bytes;
        if (available > http_buffer_size) available = http_buffer_size;
        int read_len = esp_http_client_read(client, (char *)data + bytes, (int)available);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            if (++empty_reads > 25) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        empty_reads = 0;
        if (bytes + (size_t)read_len > BG_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        bytes += (size_t)read_len;
        if (BG_READ_YIELD_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(BG_READ_YIELD_MS));
        }

        if (!signature_checked && bytes >= 2) {
            signature_checked = true;
            if (data[0] != 0xFF || data[1] != 0xD8) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }

        bool should_report = false;
        if (content_len > 0) {
            unsigned pct = (unsigned)((bytes * 100u) / (size_t)content_len);
            if (pct > 100) pct = 100;
            if (last_progress_pct == 255 || pct >= (unsigned)last_progress_pct + 5u || pct == 100) {
                should_report = true;
                last_progress_pct = (uint8_t)pct;
            }
        } else if (bytes - last_progress_bytes >= BG_PROGRESS_STEP_BYTES) {
            should_report = true;
            last_progress_bytes = bytes;
        }
        if (should_report) set_progress_status(index, total, bytes, content_len);
    }

    if (err == ESP_OK && bytes < BG_MIN_BYTES) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && !signature_checked) err = ESP_ERR_INVALID_RESPONSE;

    if (err == ESP_OK) set_progress_status(index, total, bytes, content_len);

    /* On success: hand the client back so the caller can reuse it for the batch.
     * On error:   clean up the client to free TLS/internal-RAM resources.
     *             The old "abandon" strategy leaked ~20 KB of mbedTLS
     *             buffers per failed attempt.                              */
    if (err == ESP_OK) {
        *p_client = client;
    } else {
        ESP_LOGW(TAG, "dl: error %s — closing client", esp_err_to_name(err));
        if (client) {
            if (client_open) esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        *p_client = NULL;
    }

    if (err == ESP_OK) {
        if (out_data) *out_data = data;
        else free(data);
        data = NULL;
        if (out_bytes) *out_bytes = bytes;
    }
    if (data) free(data);
    if (out_http_status && *out_http_status == 0) *out_http_status = http_status;
    return err;
}

/**
 * Write JPEG data from PSRAM to an SD card file in small chunks with
 * yields between each write.
 *
 * The SDMMC host controller shares the bus between Slot 0 (SD card) and
 * Slot 1 (SDIO Wi-Fi).  A sustained multi-block transfer on Slot 0
 * prevents Slot 1 from servicing WiFi management frames (beacons, etc.)
 * → SDIO timeout → "Unrecoverable host sdio state" → reboot.
 *
 * By writing small chunks and yielding between them, we give the SDIO
 * driver task a window to process any pending Slot 1 events.
 */
#define BG_SD_WRITE_CHUNK   1024    /* keep each SD write short */
#define BG_SD_WRITE_YIELD_MS  2     /* yield between chunks      */

static esp_err_t write_jpeg_memory_to_tmp(const char *tmp_path, const uint8_t *data, size_t bytes)
{
    if (!tmp_path || !data || bytes == 0) return ESP_ERR_INVALID_ARG;
    remove(tmp_path);
    FILE *file = fopen(tmp_path, "wb");
    if (!file) {
        ESP_LOGW(TAG, "open %s failed: errno=%d", tmp_path, errno);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    size_t offset = 0;
    while (offset < bytes) {
        size_t chunk = bytes - offset;
        if (chunk > BG_SD_WRITE_CHUNK) chunk = BG_SD_WRITE_CHUNK;
        if (fwrite(data + offset, 1, chunk, file) != chunk) {
            err = ESP_FAIL;
            break;
        }
        offset += chunk;
        /* Yield so the SDIO driver can service Slot 1 (WiFi) between
         * Slot 0 (SD) block writes — prevents host controller starvation. */
        vTaskDelay(pdMS_TO_TICKS(BG_SD_WRITE_YIELD_MS));
    }
    if (err == ESP_OK && fflush(file) != 0) err = ESP_FAIL;
    int close_result = fclose(file);
    if (close_result != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

static esp_err_t download_jpeg_to_tmp(const char *url, const char *tmp_path,
                                      uint8_t index, uint8_t total,
                                      size_t *out_bytes, int *out_http_status)
{
    static const size_t buffer_sizes[] = {BG_HTTP_BUFFER_BYTES, BG_HTTP_BUFFER_MIN_BYTES};
    esp_err_t err = ESP_FAIL;
    esp_http_client_handle_t client = NULL;
    for (size_t attempt = 0; attempt < sizeof(buffer_sizes) / sizeof(buffer_sizes[0]); attempt++) {
        uint8_t *data = NULL;
        size_t bytes = 0;
        err = download_http_jpeg_to_memory(url, index, total, buffer_sizes[attempt],
                                           &client, &data, &bytes, out_http_status);
        if (err == ESP_OK) {
            set_statusf("Image %u/%u: writing %u KB",
                        (unsigned)(index + 1), (unsigned)total, (unsigned)(bytes / 1024u));
            /* Mount SD for the file write */
            esp_err_t sd_err = sd_storage_ensure_mounted();
            if (sd_err != ESP_OK) {
                free(data);
                goto cleanup;
            }
            err = write_jpeg_memory_to_tmp(tmp_path, data, bytes);
            free(data);
            if (out_bytes) *out_bytes = bytes;
            goto cleanup;
        }
        if (data) free(data);
        if (err != ESP_ERR_NO_MEM || attempt + 1 >= sizeof(buffer_sizes) / sizeof(buffer_sizes[0])) break;
        set_statusf("Image %u/%u: memory tight, retrying",
                    (unsigned)(index + 1), (unsigned)total);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    remove(tmp_path);

cleanup:
    if (client) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        (void)services_reset_wifi_link();
    }
    return err;
}

/* ── Image download queue ──────────────────────────────────────
 *
 * Downloads JPEG images sequentially with a batch keep-alive HTTPS client.
 *
 * After each image is downloaded to PSRAM it is written to SD in
 * small chunks and (if progressive) transcoded to baseline JPEG.
 * The optional on_saved callback fires per image so the caller
 * can persist config or NVS state incrementally.
 *
 * Each image is fully downloaded before SD writes or JPEG conversion run.
 *
 * Usage:
 *   bg_dl_queue_t q;
 *   bg_dl_queue_init(&q);
 *   bg_dl_queue_add(&q, url, preset, slot, false, slot);
 *   q.on_saved = my_callback;
 *   bg_dl_queue_run(&q);
 *   // inspect q.succeeded / q.failed / q.last_error
 */

typedef void (*bg_dl_saved_fn)(uint8_t index, uint8_t tag,
                                const char *path, size_t bytes, void *ctx);

typedef struct {
    const char *url;           /* points to caller's storage — must stay valid */
    char        dest[80];      /* final path on SD */
    char        tmp[80];       /* temp path for atomic write */
    bool        skip;          /* true → already present, don't download */
    uint8_t     tag;           /* caller-defined (e.g. slot number) */
} bg_dl_item_t;

typedef struct {
    bg_dl_item_t   items[APP_THEME_MAX_IMAGES];
    uint8_t        count;
    uint8_t        max_retries;    /* per-item HTTP retry count (0 = one attempt) */

    /* Filled by bg_dl_queue_run() */
    uint8_t        succeeded;
    uint8_t        failed;
    uint8_t        skipped;
    char           last_error[96];

    /* Optional per-item callback after successful save + convert */
    bg_dl_saved_fn on_saved;
    void          *ctx;
} bg_dl_queue_t;

static void bg_dl_queue_init(bg_dl_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->max_retries = BG_DL_MAX_RETRIES;
}

static bool bg_dl_queue_add(bg_dl_queue_t *q, const char *url,
                             unsigned preset, unsigned slot,
                             bool skip, uint8_t tag)
{
    if (q->count >= APP_THEME_MAX_IMAGES) return false;
    bg_dl_item_t *item = &q->items[q->count++];
    item->url  = url;
    item->skip = skip;
    item->tag  = tag;
    if (skip) q->skipped++;
    snprintf(item->dest, sizeof(item->dest), BG_FILE_FMT, preset, slot);
    snprintf(item->tmp,  sizeof(item->tmp),  BG_TMP_FMT,  preset, slot);
    return true;
}

/** Check SD card and mark items whose dest file already exists as skip. */
static void bg_dl_queue_skip_existing(bg_dl_queue_t *q)
{
    if (sd_storage_ensure_mounted() != ESP_OK) return;
    for (uint8_t i = 0; i < q->count; i++) {
        if (q->items[i].skip) continue;
        if (file_exists(q->items[i].dest)) {
            q->items[i].skip = true;
            q->skipped++;
            ESP_LOGI(TAG, "  [%u] already on SD, skipping", i + 1);
        }
    }
}

static void bg_download_pause_sd(bool *paused)
{
    if (!paused || *paused) return;
    sd_storage_pause();
    *paused = true;
}

static void bg_download_resume_sd(bool *paused)
{
    if (!paused || !*paused) return;
    sd_storage_resume();
    *paused = false;
}

static bool bg_download_wait_wifi_ready(uint32_t timeout_ms, uint32_t stable_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    TickType_t ready_since = 0;
    while (xTaskGetTickCount() < deadline) {
        services_status_t service_status;
        services_status_get(&service_status);
        bool ready = service_status.wifi_connected &&
                     service_status.ip_addr[0] &&
                     strcmp(service_status.ip_addr, "-") != 0;
        if (ready) {
            TickType_t now = xTaskGetTickCount();
            if (ready_since == 0) ready_since = now;
            if ((now - ready_since) >= pdMS_TO_TICKS(stable_ms)) return true;
        } else {
            ready_since = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

static void bg_download_reset_wifi_link(const char *status_text, const char *log_text)
{
    set_statusf("%s", status_text);
    ESP_LOGW(TAG, "%s", log_text);
    esp_err_t reset_err = services_reset_wifi_link();
    if (reset_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi link reset failed: %s", esp_err_to_name(reset_err));
        vTaskDelay(pdMS_TO_TICKS(3000));
        return;
    }
    if (!bg_download_wait_wifi_ready(30000, 2500)) {
        ESP_LOGW(TAG, "Wi-Fi link reset did not reconnect before timeout");
    } else {
        ESP_LOGI(TAG, "Wi-Fi link reset and reconnected");
    }
}

static void bg_download_recover_wifi_link(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        set_statusf("Waiting for Wi-Fi link");
        ESP_LOGW(TAG, "download failed; waiting for Wi-Fi before retry");
        if (bg_download_wait_wifi_ready(20000, 2500)) return;
    } else if (err != ESP_ERR_HTTP_CONNECT && err != ESP_FAIL && err != ESP_ERR_TIMEOUT) {
        return;
    }
    bg_download_reset_wifi_link("Recovering Wi-Fi link",
                                "download failed; resetting hosted Wi-Fi link before retry");
}

static void bg_dl_queue_run(bg_dl_queue_t *q)
{
    uint8_t to_download = q->count - q->skipped;
    uint8_t dl_index = 0;   /* 0-based counter of non-skipped items */
    bool sd_paused = false;
    esp_http_client_handle_t client = NULL;

    if (q->skipped > 0) {
        ESP_LOGI(TAG, "download queue: %u already on disk, downloading %u",
                 q->skipped, to_download);
    }

    if (to_download > 0) {
        services_network_bulk_begin();
        bg_download_pause_sd(&sd_paused);
    }

    for (uint8_t i = 0; i < q->count; i++) {
        bg_dl_item_t *item = &q->items[i];
        if (item->skip) {
            continue;
        }

        dl_index++;
        ESP_LOGI(TAG, "===== DOWNLOAD %u/%u (slot %u) =====",
                 dl_index, to_download, i);
        set_item_progress(i, q->count, 0);

        /* ── Download to PSRAM ──
         * The SDIO bus is fragile under sustained HTTPS (espressif/
         * esp-hosted-mcu #167, #184).  Keep the HTTPS mutex held
         * through the entire download + SD-write cycle so nothing else
         * touches the SDIO bus between images.                      */
        uint8_t  *data = NULL;
        size_t    bytes = 0;
        int       http_status = 0;
        esp_err_t err = ESP_FAIL;

        for (uint8_t attempt = 0; attempt <= q->max_retries; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "  retry %u/%u",
                         attempt, q->max_retries);
                set_statusf("Image %u/%u: retry %u",
                            (unsigned)dl_index, (unsigned)to_download, attempt);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }

            if (!bg_download_wait_wifi_ready(30000, 1000)) {
                err = ESP_ERR_INVALID_STATE;
                ESP_LOGW(TAG, "  Wi-Fi not ready before image %u attempt %u",
                         dl_index, attempt + 1);
                bg_download_recover_wifi_link(err);
                continue;
            }

            services_https_lock();
            sd_storage_set_network_busy(true);

            data = NULL;
            bytes = 0;
            http_status = 0;
            err = download_http_jpeg_to_memory(
                item->url, (uint8_t)(dl_index - 1), to_download,
                BG_HTTP_BUFFER_BYTES,
                &client, &data, &bytes, &http_status);

            if (BG_POST_HTTP_SETTLE_MS > 0) {
                vTaskDelay(pdMS_TO_TICKS(BG_POST_HTTP_SETTLE_MS));
            }

            sd_storage_set_network_busy(false);

            if (err == ESP_OK && data) break;

            services_https_unlock();

            /* Download failed — free partial data and retry */
            if (data) { free(data); data = NULL; }

            ESP_LOGW(TAG, "  attempt %u failed: %s (http %d)",
                     attempt + 1, esp_err_to_name(err), http_status);
            bg_download_recover_wifi_link(err);
        }

        ESP_LOGI(TAG, "  [%u/%u] download %s, %u bytes, http %d",
                 dl_index, to_download, esp_err_to_name(err),
                 (unsigned)bytes, http_status);

        if (err != ESP_OK || !data) {
            q->failed++;
            format_download_error(q->last_error, sizeof(q->last_error),
                                  err, http_status);
            set_statusf("Image %u/%u failed: %s",
                        (unsigned)dl_index, (unsigned)to_download,
                        q->last_error);
            if (data) free(data);
            continue;
        }

        /* ── Progressive → baseline if needed (HW decoder requires baseline) ── */
        uint8_t *baseline = data;
        size_t baseline_len = bytes;
        bool converted = false;
        if (bytes >= 4) {
            bool is_progressive = false;
            for (size_t j = 0; j + 1 < bytes; j++) {
                if (data[j] == 0xFF && data[j + 1] == 0xC2) { is_progressive = true; break; }
                if (data[j] == 0xFF && data[j + 1] == 0xC0) break;
            }
            if (is_progressive) {
                set_statusf("Converting image %u/%u",
                            (unsigned)dl_index, (unsigned)to_download);
                /* Write JPEG to tmp, convert in-place, read back */
                bg_download_resume_sd(&sd_paused);
                esp_err_t sd_err = sd_storage_ensure_dir(BG_DIR);
                if (sd_err == ESP_OK) {
                    esp_err_t werr = write_jpeg_memory_to_tmp(item->tmp, data, bytes);
                    if (werr == ESP_OK) {
                        esp_err_t conv = jpeg_progressive_to_baseline(item->tmp);
                        if (conv == ESP_OK) {
                            free(data); data = NULL;
                            FILE *cf = fopen(item->tmp, "rb");
                            if (cf) {
                                fseek(cf, 0, SEEK_END);
                                baseline_len = (size_t)ftell(cf);
                                fseek(cf, 0, SEEK_SET);
                                baseline = bg_malloc(baseline_len);
                                if (baseline && fread(baseline, 1, baseline_len, cf) == baseline_len) {
                                    converted = true;
                                } else {
                                    free(baseline); baseline = data; baseline_len = bytes;
                                }
                                fclose(cf);
                            }
                            remove(item->tmp);
                        } else {
                            remove(item->tmp);
                        }
                    }
                    bg_download_pause_sd(&sd_paused);
                }
            }
        }

        /* ── HW JPEG decode → raw RGB565 on SD ── */
        set_statusf("Decoding image %u/%u",
                    (unsigned)dl_index, (unsigned)to_download);

        bg_download_resume_sd(&sd_paused);
        esp_err_t sd_err = sd_storage_ensure_dir(BG_DIR);
        if (sd_err != ESP_OK) {
            q->failed++;
            if (converted) free(baseline); else free(data);
            data = NULL;
            set_statusf("Image %u/%u: SD not available",
                        (unsigned)dl_index, (unsigned)to_download);
            services_https_unlock();
            goto after_item;
        }

        remove(item->tmp);
        esp_err_t hw_err = jpeg_hw_decode_to_file(baseline, baseline_len, item->tmp);
        if (converted) free(baseline); else free(data);
        data = NULL;

        if (hw_err != ESP_OK) {
            remove(item->tmp);
            q->failed++;
            format_download_error(q->last_error, sizeof(q->last_error), hw_err, 0);
            set_statusf("Image %u/%u: decode failed",
                        (unsigned)dl_index, (unsigned)to_download);
            services_https_unlock();
            goto after_item;
        }

        remove(item->dest);
        if (rename(item->tmp, item->dest) != 0) {
            ESP_LOGW(TAG, "rename %s -> %s failed: errno=%d",
                     item->tmp, item->dest, errno);
            remove(item->tmp);
            q->failed++;
            services_https_unlock();
            goto after_item;
        }

        q->succeeded++;
        uint8_t total_done = q->skipped + q->succeeded;
        ESP_LOGI(TAG, "  SAVED %s (%u bytes) [%u/%u complete]",
                 item->dest, (unsigned)bytes,
                 total_done, q->count);
        set_progress((uint8_t)(i + 1), q->count, true,
                     (uint8_t)(((unsigned)total_done * 100u) / q->count));
        set_statusf("%u/%u images ready",
                    (unsigned)total_done, (unsigned)q->count);

        if (q->on_saved) {
            q->on_saved(i, item->tag, item->dest, bytes, q->ctx);
        }

        services_https_unlock();

after_item:
        if (dl_index < to_download) {
            bg_download_pause_sd(&sd_paused);
            if (BG_INTER_IMAGE_COOLDOWN_MS > 0) {
                vTaskDelay(pdMS_TO_TICKS(BG_INTER_IMAGE_COOLDOWN_MS));
            }
        }
    }

    if (client) {
        services_https_lock();
        sd_storage_set_network_busy(true);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;
        if (BG_POST_HTTP_SETTLE_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(BG_POST_HTTP_SETTLE_MS));
        }
        sd_storage_set_network_busy(false);
        services_https_unlock();
    }

    bg_download_resume_sd(&sd_paused);
    if (to_download > 0) services_network_bulk_end();
}

/* ── Manual download (UI-triggered) ────────────────────────── */

typedef struct {
    bool replace_collection;
    bool bulk_claimed;
    uint8_t preset_index;
    uint8_t url_count;
    char urls[APP_THEME_MAX_IMAGES][256];
} download_req_t;

static void finish_download_ui(const char *message)
{
    if (bsp_display_lock(0)) {
        ui_background_refresh();
        toast_show(message);
        bsp_display_unlock();
    }
}

static void download_req_release_bulk(download_req_t *req)
{
    if (req && req->bulk_claimed) {
        services_network_bulk_end();
        req->bulk_claimed = false;
    }
}

/* Callback: update theme config after each image is saved. */
typedef struct {
    app_theme_config_t *cfg;
    const char        (*urls)[256];
    uint8_t             preset_index;
} dl_save_ctx_t;

static void dl_on_saved(uint8_t index, uint8_t tag, const char *path,
                         size_t bytes, void *ctx)
{
    (void)path; (void)bytes;
    dl_save_ctx_t *c = ctx;
    uint8_t slot = tag;

    c->cfg->background_preset = c->preset_index;
    c->cfg->background_enabled = true;
    if (c->cfg->image_count < slot + 1) c->cfg->image_count = slot + 1;
    if (c->cfg->image_count > APP_THEME_MAX_IMAGES)
        c->cfg->image_count = APP_THEME_MAX_IMAGES;
    c->cfg->next_slot = (uint8_t)((slot + 1) % APP_THEME_MAX_IMAGES);
    snprintf(c->cfg->background_url, sizeof(c->cfg->background_url),
             "%s", c->urls[index]);
    (void)app_config_theme_save(c->cfg);
}

static void download_task(void *arg)
{
    download_req_t *req = (download_req_t *)arg;
    esp_err_t err = ESP_OK;

    services_status_t st;
    services_status_get(&st);
    if (!st.wifi_connected) err = ESP_ERR_INVALID_STATE;

    app_theme_config_t cfg;
    if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);

    unsigned pidx = req->preset_index;
    uint8_t total = req->url_count ? req->url_count : 1;

    /* Forcibly clear all LVGL background images so the render thread
     * stops reading JPEGs from SD via the stdio VFS driver.  This is
     * critical — LVGL bypasses sd_storage_ensure_mounted() entirely.  */
    if (bsp_display_lock(pdMS_TO_TICKS(2000))) {
        clear_background_for_download();
        bsp_display_unlock();
    } else {
        s_sd_background_allowed = false;
        ESP_LOGW(TAG, "could not acquire display lock to clear backgrounds");
    }

    if (err == ESP_OK) {
        /* Compute target slots up front */
        uint8_t slots[APP_THEME_MAX_IMAGES];
        for (uint8_t i = 0; i < total; i++) {
            slots[i] = req->replace_collection
                ? i
                : (cfg.next_slot % APP_THEME_MAX_IMAGES);
            if (!req->replace_collection && cfg.image_count > 1
                && slots[i] == s_active_slot) {
                slots[i] = (uint8_t)((slots[i] + 1) % APP_THEME_MAX_IMAGES);
            }
            if (!req->replace_collection) {
                cfg.next_slot = (uint8_t)((slots[i] + 1) % APP_THEME_MAX_IMAGES);
            }
        }

        /* Clean old files if replacing collection */
        if (req->replace_collection) {
            for (uint8_t j = 0; j < APP_THEME_MAX_IMAGES; j++) {
                char old_path[80];
                snprintf(old_path, sizeof(old_path), BG_FILE_FMT, pidx, (unsigned)j);
                remove(old_path);
                snprintf(old_path, sizeof(old_path), BG_TMP_FMT, pidx, (unsigned)j);
                remove(old_path);
            }
            cfg.background_preset = req->preset_index;
            cfg.background_enabled = false;
            cfg.image_count = 0;
            cfg.next_slot = 0;
            (void)app_config_theme_save(&cfg);

            /* Clear NVS download progress — starting fresh */
            nvs_handle_t h;
            if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, BG_NVS_KEY_DL_DONE);
                nvs_erase_key(h, BG_NVS_KEY_DL_PSET);
                nvs_set_u8(h, BG_NVS_KEY_DL_FAIL, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        }

        /* Build download queue */
        bg_dl_queue_t q;
        bg_dl_queue_init(&q);
        for (uint8_t i = 0; i < total; i++) {
            bg_dl_queue_add(&q, req->urls[i], pidx, (unsigned)slots[i],
                            false, slots[i]);
        }
        /* Skip images already on SD (e.g. partial prior download) */
        if (!req->replace_collection) {
            bg_dl_queue_skip_existing(&q);
        }

        dl_save_ctx_t save_ctx = {
            .cfg           = &cfg,
            .urls          = (const char (*)[256])req->urls,
            .preset_index  = req->preset_index,
        };
        q.on_saved = dl_on_saved;
        q.ctx      = &save_ctx;

        vTaskDelay(pdMS_TO_TICKS(200));
        bg_dl_queue_run(&q);

        /* Display result */
        if (q.succeeded > 0) {
            if (req->replace_collection && q.succeeded >= total && q.failed == 0) {
                mark_background_cache_current(req->preset_index);
            }
            if (q.failed) set_statusf("Saved %u/%u images",
                                       (unsigned)q.succeeded, (unsigned)total);
            else set_statusf("Saved %u images", (unsigned)q.succeeded);
            finish_download_ui(q.failed ? s_status : "Background images saved");
        } else {
            set_statusf("%s", q.last_error);
            finish_download_ui(s_status);
        }
    } else {
        char message[96];
        format_download_error(message, sizeof(message), err, 0);
        set_statusf("%s", message);
        finish_download_ui(s_status);
        ESP_LOGW(TAG, "background download failed: %s", esp_err_to_name(err));
    }

    download_req_release_bulk(req);
    set_busy(false);
    free(req);
    vTaskDelete(NULL);
}

static esp_err_t start_download_request(download_req_t *req)
{
    if (!req || req->url_count == 0) {
        if (req) free(req);
        set_statusf("No background URLs");
        return ESP_ERR_INVALID_ARG;
    }
    if (!try_start_download()) {
        free(req);
        return ESP_ERR_INVALID_STATE;
    }

    set_statusf(req->url_count > 1 ? "Downloading %u images" : "Downloading background",
                (unsigned)req->url_count);
    set_progress(1, req->url_count, true, 0);
    services_network_bulk_begin();
    req->bulk_claimed = true;

    BaseType_t ok = xTaskCreateWithCaps(download_task, "bg_download", BG_TASK_STACK, req, 5,
                                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(download_task, "bg_download", BG_TASK_STACK, req, 5, NULL);
    if (ok != pdPASS) {
        download_req_release_bulk(req);
        set_busy(false);
        free(req);
        set_statusf("Unable to start image loader");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ui_background_download_start(const char *url, uint8_t preset_index)
{
    char clean_url[256];
    if (!copy_trimmed_url(clean_url, sizeof(clean_url), url) || !valid_url(clean_url)) {
        set_statusf("Use an http/https JPG URL");
        return ESP_ERR_INVALID_ARG;
    }

    download_req_t *req = bg_calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->url_count = 1;
    req->preset_index = preset_index;
    req->replace_collection = false;
    snprintf(req->urls[0], sizeof(req->urls[0]), "%s", clean_url);
    return start_download_request(req);
}

esp_err_t ui_background_download_collection_start(const char *const *urls,
                                                   uint8_t url_count,
                                                   uint8_t preset_index)
{
    if (!urls || url_count == 0) {
        set_statusf("No background URLs");
        return ESP_ERR_INVALID_ARG;
    }
    if (url_count > APP_THEME_MAX_IMAGES) url_count = APP_THEME_MAX_IMAGES;

    download_req_t *req = bg_calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->replace_collection = true;
    req->preset_index = preset_index;

    for (uint8_t i = 0; i < url_count; i++) {
        char clean_url[256];
        if (!copy_trimmed_url(clean_url, sizeof(clean_url), urls[i]) || !valid_url(clean_url)) {
            free(req);
            set_statusf("Bad preset image URL");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(req->urls[req->url_count], sizeof(req->urls[req->url_count]), "%s", clean_url);
        req->url_count++;
    }

    return start_download_request(req);
}

esp_err_t ui_background_clear_images(void)
{
    /* Reset all download state so auto-download starts fresh */
    clear_download_state_nvs();
    esp_err_t err = app_config_theme_clear();
    esp_err_t sd_err = sd_storage_ensure_mounted();
    if (sd_err == ESP_OK) {
        for (uint8_t p = 0; p < bg_preset_count(); p++) {
            for (uint8_t i = 0; i < APP_THEME_MAX_IMAGES; i++) {
                char path[80];
                snprintf(path, sizeof(path), BG_FILE_FMT, (unsigned)p, (unsigned)i);
                remove(path);
                snprintf(path, sizeof(path), BG_TMP_FMT, (unsigned)p, (unsigned)i);
                remove(path);
            }
        }
    } else {
        ESP_LOGW(TAG, "background clear could not access SD: %s", esp_err_to_name(sd_err));
    }
    if (err == ESP_OK && sd_err == ESP_OK) {
        set_statusf("Background cleared");
    } else if (err == ESP_OK) {
        set_statusf("Theme cleared; %s", sd_storage_last_error());
    } else {
        set_statusf("Clear failed: %s", esp_err_to_name(err));
    }
    ui_background_refresh();
    return err;
}

esp_err_t ui_background_delete_folder(void)
{
    if (ui_background_is_busy()) {
        set_statusf("Background download busy");
        return ESP_ERR_INVALID_STATE;
    }

    clear_download_state_nvs();
    esp_err_t cfg_err = app_config_theme_clear();

    if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
        clear_background_for_download();
        bsp_display_unlock();
    } else {
        s_sd_background_allowed = false;
        ESP_LOGW(TAG, "background folder delete could not acquire display lock");
    }

    esp_err_t sd_err = sd_storage_ensure_mounted();
    int del_ret = -1;
    if (sd_err == ESP_OK) {
        del_ret = bg_recursive_delete(BG_DIR);
    } else {
        ESP_LOGW(TAG, "background folder delete could not access SD: %s", esp_err_to_name(sd_err));
    }

    if (cfg_err == ESP_OK && sd_err == ESP_OK && del_ret == 0) {
        set_statusf("Background folder deleted");
    } else if (sd_err != ESP_OK) {
        set_statusf("Folder delete failed: %s", sd_storage_last_error());
    } else if (del_ret != 0) {
        set_statusf("Folder delete failed: %s", strerror(errno));
    } else {
        set_statusf("Theme reset failed: %s", esp_err_to_name(cfg_err));
    }

    if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
        ui_background_refresh();
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "background folder delete could not refresh UI");
    }

    if (cfg_err != ESP_OK) return cfg_err;
    if (sd_err != ESP_OK) return sd_err;
    return del_ret == 0 ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  Auto-download: fetch ALL preset images if missing from SD         */
/* ------------------------------------------------------------------ */
static bool preset_images_present(uint8_t preset_idx)
{
    uint8_t url_count = bg_preset_url_count(preset_idx);
    for (uint8_t i = 0; i < url_count; i++) {
        char path[80];
        snprintf(path, sizeof(path), BG_FILE_FMT, (unsigned)preset_idx, (unsigned)i);
        if (!file_exists(path)) return false;
    }
    return true;
}

/* Callback: update NVS progress after each successful save.
 * - Reset crash counter (this image didn't crash)
 * - Set bit in dl_done bitmask so crash recovery knows what's done */
static void auto_dl_on_saved(uint8_t index, uint8_t tag, const char *path,
                              size_t bytes, void *ctx)
{
    (void)index; (void)path; (void)bytes; (void)ctx;
    nvs_handle_t fh;
    if (nvs_open(BG_NVS_NS, NVS_READWRITE, &fh) == ESP_OK) {
        nvs_set_u8(fh, BG_NVS_KEY_DL_FAIL, 0);
        uint8_t done = 0;
        nvs_get_u8(fh, BG_NVS_KEY_DL_DONE, &done);
        done |= (1u << tag);   /* tag = image index */
        nvs_set_u8(fh, BG_NVS_KEY_DL_DONE, done);
        nvs_commit(fh);
        nvs_close(fh);
        ESP_LOGI(TAG, "  NVS dl_done=0x%02x (image %u saved)", done, tag);
    }
}

static void auto_download_task(void *arg)
{
    (void)arg;

    /* Give the Wi-Fi coprocessor SDIO link time to stabilise after boot. */
    ESP_LOGI(TAG, "auto-download: checking background images...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ---- Check NVS converter version ---- */
    bool need_migration = false;
    {
        nvs_handle_t h;
        uint8_t stored_ver = 0;
        if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_get_u8(h, BG_NVS_KEY_CONV, &stored_ver) != ESP_OK)
                stored_ver = 0;
            if (stored_ver != BG_CONV_VERSION) {
                need_migration = true;
                nvs_erase_key(h, BG_NVS_KEY_DL_FAIL);
                nvs_commit(h);
                ESP_LOGW(TAG, "auto-download: converter version changed (%u -> %u)",
                         (unsigned)stored_ver, (unsigned)BG_CONV_VERSION);
            }
            nvs_close(h);
        }
    }

    /* Download queue handles SD writes, JPEG conversion, and per-image NVS progress. */

    uint8_t total_presets = bg_preset_count();
    app_theme_config_t dl_cfg;
    if (app_config_theme_load(&dl_cfg) != ESP_OK) app_config_theme_defaults(&dl_cfg);
    uint8_t active_preset = dl_cfg.background_preset;
    if (active_preset >= total_presets) active_preset = 0;

    uint8_t total_images = bg_preset_url_count(active_preset);
    const bg_preset_t *preset = bg_preset_get(active_preset);
    uint8_t written_ok = 0;

    if (!need_migration && preset_images_present(active_preset)) {
        ESP_LOGI(TAG, "auto-download: no migration needed, images present");
        goto refresh;
    }
    if (!need_migration) {
        ESP_LOGW(TAG, "auto-download: version OK but images missing — re-downloading");
    }

    /* Crash-safety: read NVS state and bump failure counter.
     * dl_fail counts consecutive crash-recovery attempts.
     * dl_pset / dl_done track which preset + images are done so
     * after a crash we can resume where we left off.              */
    uint8_t nvs_done = 0;       /* bitmask of images saved in prior runs */
    {
        nvs_handle_t h;
        uint8_t fail_count = 0;
        if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_u8(h, BG_NVS_KEY_DL_FAIL, &fail_count);
            if (fail_count >= BG_MAX_DL_FAILURES) {
                ESP_LOGE(TAG, "auto-download: giving up after %u crash(es); "
                         "use Settings to download manually", fail_count);
                nvs_close(h);
                goto finish;
            }

            /* If preset changed, clear the done bitmask */
            uint8_t saved_pset = 0xFF;
            nvs_get_u8(h, BG_NVS_KEY_DL_PSET, &saved_pset);
            if (saved_pset != active_preset) {
                nvs_set_u8(h, BG_NVS_KEY_DL_DONE, 0);
                ESP_LOGI(TAG, "auto-download: preset changed %u->%u, reset dl_done",
                         saved_pset, active_preset);
            } else {
                nvs_get_u8(h, BG_NVS_KEY_DL_DONE, &nvs_done);
            }

            nvs_set_u8(h, BG_NVS_KEY_DL_PSET, active_preset);
            nvs_set_u8(h, BG_NVS_KEY_DL_FAIL, fail_count + 1);
            nvs_commit(h);
            nvs_close(h);

            ESP_LOGI(TAG, "auto-download: fail_count=%u, nvs_done=0x%02x",
                     fail_count, nvs_done);
        }
    }

    /* Forcibly clear all LVGL background images so the render thread
     * stops reading JPEGs from SD via the stdio VFS driver.  */
    if (bsp_display_lock(pdMS_TO_TICKS(2000))) {
        clear_background_for_download();
        bsp_display_unlock();
    } else {
        s_sd_background_allowed = false;
        ESP_LOGW(TAG, "auto-download: could not acquire display lock to clear backgrounds");
    }

    /* Build download queue — all items initially unskipped.
     * bg_dl_queue_skip_existing() checks SD for files already on disk.
     * We also skip any images that NVS says were saved in a prior run
     * (covers the case where SD mount fails but NVS remembers state). */
    {
        bg_dl_queue_t q;
        bg_dl_queue_init(&q);
        for (uint8_t i = 0; i < total_images; i++) {
            bool nvs_has_it = (nvs_done >> i) & 1u;
            bg_dl_queue_add(&q, preset->urls[i],
                            (unsigned)active_preset, (unsigned)i,
                            nvs_has_it, i);
        }
        bg_dl_queue_skip_existing(&q);

        uint8_t to_download = q.count - q.skipped;

        ESP_LOGI(TAG, "auto-download: preset '%s' (%u images, %u to download)",
                 preset->label, total_images, to_download);

        if (to_download == 0) {
            ESP_LOGI(TAG, "auto-download: all images present");
            written_ok = total_images;
            goto done;
        }

        q.on_saved = auto_dl_on_saved;
        q.ctx      = NULL;

        vTaskDelay(pdMS_TO_TICKS(200));
        bg_dl_queue_run(&q);

        written_ok = q.skipped + q.succeeded;
    }

done:
    ESP_LOGI(TAG, "auto-download: done (%u/%u complete)", written_ok, total_images);

    /* Persist version + reset crash counter + clear progress state */
    {
        nvs_handle_t h;
        if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            if (written_ok >= total_images) {
                nvs_close(h);
                mark_background_cache_current(active_preset);
                h = 0;
            }
            if (h) {
                nvs_set_u8(h, BG_NVS_KEY_DL_FAIL, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }

    /* Update theme config */
    {
        app_theme_config_t cfg;
        if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);
        cfg.background_enabled = true;
        cfg.background_preset = active_preset;
        cfg.image_count = bg_preset_url_count(active_preset);
        (void)app_config_theme_save(&cfg);
    }

    goto refresh;

finish:
    s_migration_pending = false;

refresh:

    /* Make sure the active preset's images are enabled and refresh display */
    {
        app_theme_config_t cfg;
        if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);
        uint8_t active = cfg.background_preset;
        if (active >= total_presets) active = 0;
        if (!cfg.background_enabled || cfg.image_count == 0 || written_ok > 0) {
            cfg.background_enabled = true;
            cfg.background_preset = active;
            cfg.image_count = bg_preset_url_count(active);
            (void)app_config_theme_save(&cfg);
        }
        if (bsp_display_lock(pdMS_TO_TICKS(1000))) {
            ui_background_refresh();
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "auto-download: could not acquire display lock for refresh");
        }
        ESP_LOGI(TAG, "auto-download: done (written=%u)", written_ok);
    }

    vTaskDelete(NULL);
}

void ui_background_auto_download(void)
{
    BaseType_t ok = xTaskCreateWithCaps(auto_download_task, "bg_auto", 16384,
                                         NULL, 4, NULL,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(auto_download_task, "bg_auto", 16384, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "auto-download: could not create task");
    }
}
