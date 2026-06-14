#include "sd_error_log.h"

#include "sd_storage.h"

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SD_ERROR_LOG_DIR               BSP_SD_MOUNT_POINT "/logs"
#define SD_ERROR_LOG_FILE              SD_ERROR_LOG_DIR "/sd_errors.csv"
#define SD_ERROR_LOG_PREV_FILE         SD_ERROR_LOG_DIR "/sd_errors.prev.csv"
#define SD_ERROR_LOG_CAPACITY          48
#define SD_ERROR_LOG_FALLBACK_CAPACITY 6
#define SD_ERROR_LOG_MAX_BYTES         (128u * 1024u)
#define SD_ERROR_LOG_LINE_MAX          420

static const char *TAG = "sd_err_log";

typedef struct {
    sd_error_log_entry_t entry;
    bool pending;
} sd_error_log_slot_t;

static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;
static sd_error_log_slot_t *s_slots;
static sd_error_log_slot_t s_fallback_slots[SD_ERROR_LOG_FALLBACK_CAPACITY];
static size_t s_capacity;
static bool s_using_fallback;
static bool s_flushing;
static uint32_t s_next_seq = 1;
static uint32_t s_recorded;
static uint32_t s_pending;
static uint32_t s_flushed;
static uint32_t s_dropped;
static uint32_t s_flush_failures;
static esp_err_t s_last_flush_err = ESP_ERR_INVALID_STATE;
static char s_last_flush_detail[SD_ERROR_LOG_DETAIL_LEN] = "Never flushed";

static SemaphoreHandle_t log_mutex(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    return s_mutex;
}

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int64_t unix_time_now(void)
{
    time_t now = time(NULL);
    return now > 1700000000 ? (int64_t)now : 0;
}

static void copy_field(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_len, "%s", src);
}

static esp_err_t ensure_ready_locked(void)
{
    if (s_slots) return ESP_OK;

    s_slots = heap_caps_calloc(SD_ERROR_LOG_CAPACITY, sizeof(*s_slots),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_slots) {
        s_capacity = SD_ERROR_LOG_CAPACITY;
        s_using_fallback = false;
        s_last_flush_err = ESP_OK;
        copy_field(s_last_flush_detail, sizeof(s_last_flush_detail), "RAM log ready");
        return ESP_OK;
    }

    memset(s_fallback_slots, 0, sizeof(s_fallback_slots));
    s_slots = s_fallback_slots;
    s_capacity = SD_ERROR_LOG_FALLBACK_CAPACITY;
    s_using_fallback = true;
    s_last_flush_err = ESP_ERR_NO_MEM;
    copy_field(s_last_flush_detail, sizeof(s_last_flush_detail), "Using tiny internal log buffer");
    ESP_LOGW(TAG, "%s", s_last_flush_detail);
    return ESP_OK;
}

esp_err_t sd_error_log_init(void)
{
    SemaphoreHandle_t mutex = log_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = ensure_ready_locked();
    xSemaphoreGive(mutex);
    return err;
}

void sd_error_log_record(const char *source,
                         const char *operation,
                         const char *path,
                         esp_err_t err,
                         int errno_value,
                         const char *detail)
{
    if (err == ESP_OK || s_flushing) return;
    SemaphoreHandle_t mutex = log_mutex();
    if (!mutex) return;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (ensure_ready_locked() != ESP_OK || s_capacity == 0) {
        xSemaphoreGive(mutex);
        return;
    }

    uint32_t seq = s_next_seq++;
    sd_error_log_slot_t *slot = &s_slots[seq % s_capacity];
    if (slot->pending && s_pending > 0) {
        s_pending--;
        s_dropped++;
    }

    memset(slot, 0, sizeof(*slot));
    slot->entry.seq = seq;
    slot->entry.uptime_ms = uptime_ms();
    slot->entry.unix_time = unix_time_now();
    slot->entry.err = err;
    slot->entry.errno_value = errno_value;
    copy_field(slot->entry.source, sizeof(slot->entry.source), source ? source : "sd");
    copy_field(slot->entry.operation, sizeof(slot->entry.operation), operation ? operation : "op");
    copy_field(slot->entry.path, sizeof(slot->entry.path), path ? path : "");
    copy_field(slot->entry.detail, sizeof(slot->entry.detail), detail ? detail : esp_err_to_name(err));
    slot->pending = true;

    s_recorded++;
    s_pending++;
    xSemaphoreGive(mutex);
}

static void csv_clean(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";

    size_t out = 0;
    for (size_t i = 0; src[i] && out < dst_len - 1; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\r' || ch == '\n' || ch == ',') {
            dst[out++] = ' ';
        } else if (ch == '"') {
            dst[out++] = '\'';
        } else if (isprint(ch)) {
            dst[out++] = (char)ch;
        } else {
            dst[out++] = '?';
        }
    }
    dst[out] = '\0';
}

static int format_entry_line(const sd_error_log_entry_t *entry, char *line, size_t line_len)
{
    char source[SD_ERROR_LOG_SOURCE_LEN];
    char operation[SD_ERROR_LOG_OP_LEN];
    char path[SD_ERROR_LOG_PATH_LEN];
    char detail[SD_ERROR_LOG_DETAIL_LEN];
    csv_clean(source, sizeof(source), entry->source);
    csv_clean(operation, sizeof(operation), entry->operation);
    csv_clean(path, sizeof(path), entry->path);
    csv_clean(detail, sizeof(detail), entry->detail);

    return snprintf(line, line_len,
                    "%" PRIu32 ",%" PRId64 ",%" PRIu32 ",%s,%s,%s,%d,%d,%s,%s\n",
                    entry->seq,
                    entry->unix_time,
                    entry->uptime_ms,
                    source,
                    operation,
                    esp_err_to_name(entry->err),
                    (int)entry->err,
                    entry->errno_value,
                    path,
                    detail);
}

static esp_err_t rotate_if_needed(void)
{
    struct stat st;
    if (stat(SD_ERROR_LOG_FILE, &st) != 0 || st.st_size < (off_t)SD_ERROR_LOG_MAX_BYTES) {
        return ESP_OK;
    }

    (void)remove(SD_ERROR_LOG_PREV_FILE);
    if (rename(SD_ERROR_LOG_FILE, SD_ERROR_LOG_PREV_FILE) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_entries(const sd_error_log_entry_t *entries, size_t count)
{
    if (!entries || count == 0) return ESP_OK;

    esp_err_t err = sd_storage_ensure_dir(SD_ERROR_LOG_DIR);
    if (err != ESP_OK) return err;

    (void)rotate_if_needed();

    bool need_header = true;
    struct stat st;
    if (stat(SD_ERROR_LOG_FILE, &st) == 0 && st.st_size > 0) need_header = false;

    FILE *file = fopen(SD_ERROR_LOG_FILE, "ab");
    if (!file) return ESP_FAIL;

    if (need_header) {
        static const char header[] = "seq,unix_time,uptime_ms,source,operation,esp_err,err_code,errno,path,detail\n";
        if (fwrite(header, 1, sizeof(header) - 1, file) != sizeof(header) - 1) {
            fclose(file);
            return ESP_FAIL;
        }
    }

    char line[SD_ERROR_LOG_LINE_MAX];
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        int written = format_entry_line(&entries[i], line, sizeof(line));
        if (written <= 0 || (size_t)written >= sizeof(line)) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(line, 1, (size_t)written, file) != (size_t)written) {
            result = ESP_FAIL;
            break;
        }
    }

    if (result == ESP_OK && fflush(file) != 0) result = ESP_FAIL;
    if (fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
    return result;
}

esp_err_t sd_error_log_flush(void)
{
    SemaphoreHandle_t mutex = log_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = ensure_ready_locked();
    if (err != ESP_OK || s_pending == 0) {
        xSemaphoreGive(mutex);
        return err;
    }
    if (s_flushing) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = s_pending;
    sd_error_log_entry_t *entries = heap_caps_calloc(count, sizeof(*entries),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) entries = calloc(count, sizeof(*entries));
    if (!entries) {
        s_flush_failures++;
        s_last_flush_err = ESP_ERR_NO_MEM;
        copy_field(s_last_flush_detail, sizeof(s_last_flush_detail), "Flush copy allocation failed");
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t copied = 0;
    uint32_t first_seq = s_next_seq > s_capacity ? s_next_seq - (uint32_t)s_capacity : 1;
    for (uint32_t seq = first_seq; seq < s_next_seq && copied < count; seq++) {
        sd_error_log_slot_t *slot = &s_slots[seq % s_capacity];
        if (slot->pending && slot->entry.seq == seq) {
            entries[copied++] = slot->entry;
        }
    }
    s_flushing = true;
    xSemaphoreGive(mutex);

    err = write_entries(entries, copied);

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        for (size_t i = 0; i < copied; i++) {
            sd_error_log_slot_t *slot = &s_slots[entries[i].seq % s_capacity];
            if (slot->pending && slot->entry.seq == entries[i].seq) {
                slot->pending = false;
                if (s_pending > 0) s_pending--;
                s_flushed++;
            }
        }
        s_last_flush_err = ESP_OK;
        snprintf(s_last_flush_detail, sizeof(s_last_flush_detail), "Flushed %u entries", (unsigned)copied);
    } else {
        s_flush_failures++;
        s_last_flush_err = err;
        snprintf(s_last_flush_detail, sizeof(s_last_flush_detail), "Flush failed: %s", esp_err_to_name(err));
    }
    s_flushing = false;
    xSemaphoreGive(mutex);

    free(entries);
    return err;
}

esp_err_t sd_error_log_clear(void)
{
    SemaphoreHandle_t mutex = log_mutex();
    if (!mutex) return ESP_ERR_NO_MEM;

    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = ensure_ready_locked();
    if (err != ESP_OK) {
        xSemaphoreGive(mutex);
        return err;
    }
    if (s_flushing) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_flushing = true;
    xSemaphoreGive(mutex);

    err = sd_storage_ensure_dir(SD_ERROR_LOG_DIR);
    if (err == ESP_OK) {
        if (remove(SD_ERROR_LOG_FILE) != 0 && errno != ENOENT) err = ESP_FAIL;
        if (remove(SD_ERROR_LOG_PREV_FILE) != 0 && errno != ENOENT && err == ESP_OK) err = ESP_FAIL;
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        memset(s_slots, 0, s_capacity * sizeof(*s_slots));
        s_next_seq = 1;
        s_recorded = 0;
        s_pending = 0;
        s_flushed = 0;
        s_dropped = 0;
        s_flush_failures = 0;
        s_last_flush_err = ESP_OK;
        copy_field(s_last_flush_detail, sizeof(s_last_flush_detail), "Log cleared");
    } else {
        s_last_flush_err = err;
        snprintf(s_last_flush_detail, sizeof(s_last_flush_detail), "Clear failed: %s", esp_err_to_name(err));
    }
    s_flushing = false;
    xSemaphoreGive(mutex);
    return err;
}

void sd_error_log_status_get(sd_error_log_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    SemaphoreHandle_t mutex = log_mutex();
    if (!mutex) return;

    xSemaphoreTake(mutex, portMAX_DELAY);
    (void)ensure_ready_locked();
    out->initialized = s_slots != NULL;
    out->using_fallback = s_using_fallback;
    out->flushing = s_flushing;
    out->capacity = s_capacity;
    out->recorded = s_recorded;
    out->pending = s_pending;
    out->flushed = s_flushed;
    out->dropped = s_dropped;
    out->flush_failures = s_flush_failures;
    out->last_flush_err = s_last_flush_err;
    copy_field(out->last_flush_detail, sizeof(out->last_flush_detail), s_last_flush_detail);
    xSemaphoreGive(mutex);
}

size_t sd_error_log_recent(sd_error_log_entry_t *entries, size_t max_entries)
{
    if (!entries || max_entries == 0) return 0;
    SemaphoreHandle_t mutex = log_mutex();
    if (!mutex) return 0;

    xSemaphoreTake(mutex, portMAX_DELAY);
    if (ensure_ready_locked() != ESP_OK || !s_slots || s_next_seq <= 1) {
        xSemaphoreGive(mutex);
        return 0;
    }

    uint32_t available = s_next_seq - 1;
    if (available > s_capacity) available = (uint32_t)s_capacity;
    if (available > max_entries) available = (uint32_t)max_entries;
    uint32_t first_seq = s_next_seq - available;

    size_t copied = 0;
    for (uint32_t seq = first_seq; seq < s_next_seq && copied < max_entries; seq++) {
        sd_error_log_slot_t *slot = &s_slots[seq % s_capacity];
        if (slot->entry.seq == seq) entries[copied++] = slot->entry;
    }
    xSemaphoreGive(mutex);
    return copied;
}

const char *sd_error_log_path(void)
{
    return SD_ERROR_LOG_FILE;
}
