#include "weather_history.h"

#include "sd_storage.h"

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WEATHER_HISTORY_DIR  BSP_SD_MOUNT_POINT "/weather"
#define WEATHER_HISTORY_PATH WEATHER_HISTORY_DIR "/history.csv"

static const char *TAG = "weather_history";

static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;
static bool s_loaded;
static esp_err_t s_storage_err = ESP_ERR_INVALID_STATE;
static char s_storage_detail[72] = "History not loaded";
static weather_history_sample_t *s_samples;
static uint8_t s_count;

static void set_status(const char *detail, esp_err_t err);

static weather_history_sample_t *history_samples(void)
{
    if (s_samples) return s_samples;
    s_samples = heap_caps_calloc(WEATHER_HISTORY_MAX_POINTS, sizeof(*s_samples),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_samples) set_status("History PSRAM unavailable", ESP_ERR_NO_MEM);
    return s_samples;
}

static SemaphoreHandle_t history_mutex(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    return s_mutex;
}

static void set_status(const char *detail, esp_err_t err)
{
    snprintf(s_storage_detail, sizeof(s_storage_detail), "%s", detail ? detail : esp_err_to_name(err));
    s_storage_err = err;
}

static bool parse_i32(char **cursor, int32_t *out)
{
    if (!cursor || !*cursor || !out) return false;
    char *end = NULL;
    long value = strtol(*cursor, &end, 10);
    if (end == *cursor) return false;
    *out = (int32_t)value;
    *cursor = (*end == ',') ? end + 1 : end;
    return true;
}

static bool parse_sample(char *line, weather_history_sample_t *sample)
{
    int32_t values[13] = {0};
    char *cursor = line;
    for (size_t i = 0; i < 11; i++) {
        if (!parse_i32(&cursor, &values[i])) return false;
    }
    for (size_t i = 11; i < sizeof(values) / sizeof(values[0]); i++) {
        if (!parse_i32(&cursor, &values[i])) break;
    }

    memset(sample, 0, sizeof(*sample));
    sample->observed_utc = values[0] > 0 ? (uint32_t)values[0] : 0;
    sample->temp_f = (int16_t)values[1];
    sample->feels_f = (int16_t)values[2];
    sample->humidity_pct = values[3] < 0 ? 0 : values[3] > 100 ? 100 : (uint8_t)values[3];
    sample->pressure_hpa = values[4] < 0 ? 0 : values[4] > 65535 ? 65535 : (uint16_t)values[4];
    sample->sea_level_hpa = values[11] < 0 ? 0 : values[11] > 65535 ? 65535 : (uint16_t)values[11];
    sample->grnd_level_hpa = values[12] < 0 ? 0 : values[12] > 65535 ? 65535 : (uint16_t)values[12];
    sample->wind_mph_x10 = values[5] < 0 ? 0 : values[5] > 65535 ? 65535 : (uint16_t)values[5];
    sample->wind_gust_mph_x10 = values[6] < 0 ? 0 : values[6] > 65535 ? 65535 : (uint16_t)values[6];
    sample->wind_deg = values[7] < 0 ? 0 : (uint16_t)(values[7] % 360);
    sample->clouds_pct = values[8] < 0 ? 0 : values[8] > 100 ? 100 : (uint8_t)values[8];
    sample->rain_1h_mm_x10 = values[9] < 0 ? 0 : values[9] > 65535 ? 65535 : (uint16_t)values[9];
    sample->snow_1h_mm_x10 = values[10] < 0 ? 0 : values[10] > 65535 ? 65535 : (uint16_t)values[10];
    return sample->observed_utc != 0;
}

static void append_sample_locked(const weather_history_sample_t *sample)
{
    weather_history_sample_t *samples = history_samples();
    if (!samples) return;
    if (!sample || !sample->observed_utc) return;
    if (s_count > 0 && samples[s_count - 1].observed_utc == sample->observed_utc) {
        samples[s_count - 1] = *sample;
        return;
    }
    if (s_count >= WEATHER_HISTORY_MAX_POINTS) {
        memmove(&samples[0], &samples[1], sizeof(samples[0]) * (WEATHER_HISTORY_MAX_POINTS - 1));
        s_count = WEATHER_HISTORY_MAX_POINTS - 1;
    }
    samples[s_count++] = *sample;
}

static esp_err_t load_locked(void)
{
    if (s_loaded) return ESP_OK;
    if (!history_samples()) return ESP_ERR_NO_MEM;

    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) {
        char detail[72];
        snprintf(detail, sizeof(detail), "SD unavailable: %s", sd_storage_last_error());
        set_status(detail, err);
        return err;
    }

    FILE *file = fopen(WEATHER_HISTORY_PATH, "r");
    if (!file) {
        s_count = 0;
        s_loaded = true;
        set_status(errno == ENOENT ? "No saved history yet" : "History open failed", errno == ENOENT ? ESP_OK : ESP_FAIL);
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    char line[176];
    s_count = 0;
    while (fgets(line, sizeof(line), file)) {
        if (line[0] < '0' || line[0] > '9') continue;
        weather_history_sample_t sample;
        if (parse_sample(line, &sample)) append_sample_locked(&sample);
    }
    fclose(file);

    s_loaded = true;
    char detail[72];
    snprintf(detail, sizeof(detail), "Loaded %u saved samples", s_count);
    set_status(detail, ESP_OK);
    return ESP_OK;
}

static esp_err_t save_locked(void)
{
    weather_history_sample_t *samples = history_samples();
    if (!samples) return ESP_ERR_NO_MEM;

    esp_err_t err = sd_storage_ensure_dir(WEATHER_HISTORY_DIR);
    if (err != ESP_OK) {
        char detail[72];
        snprintf(detail, sizeof(detail), "History SD save failed: %s", sd_storage_last_error());
        set_status(detail, err);
        return err;
    }

    size_t cap = 256 + (size_t)s_count * 120;
    char *data = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        set_status("History buffer unavailable", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    int written = snprintf(data, cap,
                           "observed_utc,temp_f,feels_f,humidity_pct,pressure_hpa,wind_mph_x10,wind_gust_mph_x10,wind_deg,clouds_pct,rain_1h_mm_x10,snow_1h_mm_x10,sea_level_hpa,grnd_level_hpa\n");
    if (written < 0 || (size_t)written >= cap) {
        free(data);
        set_status("History header too large", ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    len = (size_t)written;

    for (uint8_t i = 0; i < s_count; i++) {
        const weather_history_sample_t *s = &samples[i];
        written = snprintf(data + len, cap - len,
                           "%lu,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                           (unsigned long)s->observed_utc,
                           (int)s->temp_f,
                           (int)s->feels_f,
                           s->humidity_pct,
                           s->pressure_hpa,
                           s->wind_mph_x10,
                           s->wind_gust_mph_x10,
                           s->wind_deg,
                           s->clouds_pct,
                           s->rain_1h_mm_x10,
                           s->snow_1h_mm_x10,
                           s->sea_level_hpa,
                           s->grnd_level_hpa);
        if (written < 0 || (size_t)written >= cap - len) {
            free(data);
            set_status("History CSV too large", ESP_ERR_INVALID_SIZE);
            return ESP_ERR_INVALID_SIZE;
        }
        len += (size_t)written;
    }

    err = sd_storage_write_text_atomic(WEATHER_HISTORY_PATH, data, len);
    free(data);

    if (err == ESP_OK) {
        char detail[72];
        snprintf(detail, sizeof(detail), "Saved %u samples to SD", s_count);
        set_status(detail, ESP_OK);
    } else {
        char detail[72];
        snprintf(detail, sizeof(detail), "History write failed: %s", sd_storage_last_error());
        set_status(detail, err);
    }
    return err;
}

esp_err_t weather_history_init(void)
{
    SemaphoreHandle_t mutex = history_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t weather_history_record(const weather_state_t *state)
{
    if (!state || !state->valid) return ESP_ERR_INVALID_ARG;
    SemaphoreHandle_t mutex = history_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;

    weather_history_sample_t sample = {
        .observed_utc = state->observed_utc ? state->observed_utc : (uint32_t)time(NULL),
        .temp_f = (int16_t)state->temp_f,
        .feels_f = (int16_t)state->feels_f,
        .humidity_pct = state->humidity_pct,
        .pressure_hpa = state->pressure_hpa,
        .sea_level_hpa = state->sea_level_hpa,
        .grnd_level_hpa = state->grnd_level_hpa,
        .wind_mph_x10 = state->wind_mph_x10,
        .wind_gust_mph_x10 = state->wind_gust_mph_x10,
        .wind_deg = state->wind_deg,
        .clouds_pct = state->clouds_pct,
        .rain_1h_mm_x10 = state->rain_1h_mm_x10,
        .snow_1h_mm_x10 = state->snow_1h_mm_x10,
    };
    if (!sample.observed_utc) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!s_loaded) (void)load_locked();
    if (!s_samples) {
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }
    append_sample_locked(&sample);
    esp_err_t err = save_locked();
    xSemaphoreGive(mutex);

    if (err != ESP_OK) ESP_LOGW(TAG, "%s", s_storage_detail);
    return err;
}

esp_err_t weather_history_get(weather_history_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    SemaphoreHandle_t mutex = history_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(mutex, portMAX_DELAY);
    weather_history_sample_t *samples = history_samples();
    if (!samples) {
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(out, 0, sizeof(*out));
    out->loaded = s_loaded;
    out->count = s_count;
    out->storage_err = s_storage_err;
    snprintf(out->storage_detail, sizeof(out->storage_detail), "%s", s_storage_detail);
    if (s_count) memcpy(out->samples, samples, sizeof(samples[0]) * s_count);
    xSemaphoreGive(mutex);
    return ESP_OK;
}

const char *weather_history_file_path(void)
{
    return WEATHER_HISTORY_PATH;
}