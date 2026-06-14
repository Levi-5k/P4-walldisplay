#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_ERROR_LOG_SOURCE_LEN 16
#define SD_ERROR_LOG_OP_LEN     24
#define SD_ERROR_LOG_PATH_LEN   96
#define SD_ERROR_LOG_DETAIL_LEN 96

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    int64_t unix_time;
    esp_err_t err;
    int errno_value;
    char source[SD_ERROR_LOG_SOURCE_LEN];
    char operation[SD_ERROR_LOG_OP_LEN];
    char path[SD_ERROR_LOG_PATH_LEN];
    char detail[SD_ERROR_LOG_DETAIL_LEN];
} sd_error_log_entry_t;

typedef struct {
    bool initialized;
    bool using_fallback;
    bool flushing;
    size_t capacity;
    uint32_t recorded;
    uint32_t pending;
    uint32_t flushed;
    uint32_t dropped;
    uint32_t flush_failures;
    esp_err_t last_flush_err;
    char last_flush_detail[SD_ERROR_LOG_DETAIL_LEN];
} sd_error_log_status_t;

esp_err_t sd_error_log_init(void);
void sd_error_log_record(const char *source,
                         const char *operation,
                         const char *path,
                         esp_err_t err,
                         int errno_value,
                         const char *detail);
esp_err_t sd_error_log_flush(void);
esp_err_t sd_error_log_clear(void);
void sd_error_log_status_get(sd_error_log_status_t *out);
size_t sd_error_log_recent(sd_error_log_entry_t *entries, size_t max_entries);
const char *sd_error_log_path(void);

#ifdef __cplusplus
}
#endif
