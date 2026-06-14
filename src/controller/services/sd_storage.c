#include "sd_storage.h"

#include "board_power.h"
#include "bsp/esp-bsp.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_error_log.h"
#include "sdmmc_cmd.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SD_RETRY_MS          5000
#define SD_MAX_PATH_LEN      160

static const char *TAG = "sd_store";

static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;
static bool s_mounted;
static bool s_paused;         /* when true, ensure_mounted() returns error */
static bool s_network_busy;   /* when true, block SD access (HTTPS active on SDIO) */
static esp_err_t s_last_err = ESP_ERR_INVALID_STATE;
static TickType_t s_last_attempt_tick;
static char s_last_error[96] = "SD not mounted";

static SemaphoreHandle_t storage_mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    }
    return s_mutex;
}

static void set_last_error(const char *message, esp_err_t err)
{
    if (message && message[0]) {
        snprintf(s_last_error, sizeof(s_last_error), "%s", message);
    } else {
        snprintf(s_last_error, sizeof(s_last_error), "%s", esp_err_to_name(err));
    }
    s_last_err = err;
}

static void log_storage_error(const char *operation,
                              const char *path,
                              esp_err_t err,
                              int errno_value,
                              const char *detail)
{
    sd_error_log_record("sd_storage", operation, path, err, errno_value, detail);
}

static bool is_directory(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint32_t path_hash(const char *path)
{
    uint32_t hash = 2166136261u;
    while (path && *path) {
        hash ^= (uint8_t)*path++;
        hash *= 16777619u;
    }
    return hash;
}

static esp_err_t make_tmp_path(const char *path, char *tmp_path, size_t tmp_len)
{
    if (!path || !tmp_path || tmp_len == 0) return ESP_ERR_INVALID_ARG;

    uint32_t hash = path_hash(path) & 0xFFFFFFu;
    const char *slash = strrchr(path, '/');
    int written;
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        written = snprintf(tmp_path, tmp_len, "%.*s/T%06lX.TMP",
                           (int)dir_len, path, (unsigned long)hash);
    } else {
        written = snprintf(tmp_path, tmp_len, "T%06lX.TMP", (unsigned long)hash);
    }

    return (written < 0 || (size_t)written >= tmp_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t ensure_sd_power(void)
{
    return board_power_enable_vo4_3v3();
}

static esp_err_t mount_sd_locked(void)
{
    if (s_mounted && is_directory(BSP_SD_MOUNT_POINT)) {
        set_last_error("SD ready", ESP_OK);
        return ESP_OK;
    }

    if (is_directory(BSP_SD_MOUNT_POINT)) {
        s_mounted = true;
        set_last_error("SD ready", ESP_OK);
        return ESP_OK;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_last_attempt_tick != 0 && s_last_err != ESP_OK &&
        now - s_last_attempt_tick < pdMS_TO_TICKS(SD_RETRY_MS)) {
        return s_last_err;
    }
    s_last_attempt_tick = now;

    esp_err_t err = ensure_sd_power();
    if (err != ESP_OK) {
        set_last_error("SD power failed", err);
        log_storage_error("power", BSP_SD_MOUNT_POINT, err, 0, s_last_error);
        return err;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 8,
        .allocation_unit_size = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    const sdmmc_slot_config_t slot_config = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    ESP_LOGI(TAG, "mount: calling esp_vfs_fat_sdmmc_mount (slot0, freq=%d kHz)",
             host.max_freq_khz);
    err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &bsp_sdcard);
    ESP_LOGI(TAG, "mount: first attempt returned %s (0x%x)",
             esp_err_to_name(err), (unsigned)err);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mount: retrying after 100ms");
        vTaskDelay(pdMS_TO_TICKS(100));
        err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config,
                                      &mount_config, &bsp_sdcard);
        ESP_LOGI(TAG, "mount: retry returned %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
    }

    if (err == ESP_OK || is_directory(BSP_SD_MOUNT_POINT)) {
        s_mounted = true;
        set_last_error("SD ready", ESP_OK);
        ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    s_mounted = false;
    if (err == ESP_FAIL) {
        set_last_error("SD filesystem mount failed", err);
    } else if (err == ESP_ERR_NOT_FOUND) {
        set_last_error("SD card not found", err);
    } else {
        set_last_error("SD mount failed", err);
    }
    log_storage_error("mount", BSP_SD_MOUNT_POINT, err, 0, s_last_error);
    ESP_LOGW(TAG, "%s: %s", s_last_error, esp_err_to_name(err));
    return err;
}

static esp_err_t unmount_sd_locked(void)
{
    if (!s_mounted) return ESP_OK;
    /* Unmount the FAT filesystem and deinitialise the SD card. */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
    if (err == ESP_OK || !is_directory(BSP_SD_MOUNT_POINT)) {
        s_mounted = false;
        set_last_error("SD unmounted", ESP_OK);
        ESP_LOGI(TAG, "SD unmounted");
    } else {
        log_storage_error("unmount", BSP_SD_MOUNT_POINT, err, 0, "SD unmount failed");
        ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
    }
    return err;
}

void sd_storage_pause(void)
{
    ESP_LOGI(TAG, "pause: entering (mounted=%d)", s_mounted);
    SemaphoreHandle_t mutex = storage_mutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    s_paused = true;
    s_last_attempt_tick = 0;   /* clear retry cooldown */
    set_last_error("SD access paused", ESP_OK);
    ESP_LOGI(TAG, "pause: SD left mounted to preserve hosted SDIO state (mounted=%d)",
             s_mounted);
    if (mutex) xSemaphoreGive(mutex);
}

void sd_storage_resume(void)
{
    ESP_LOGI(TAG, "resume: entering (paused=%d, mounted=%d)",
             s_paused, s_mounted);
    SemaphoreHandle_t mutex = storage_mutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    s_paused = false;
    s_last_attempt_tick = 0;   /* clear retry cooldown */
    if (mutex) xSemaphoreGive(mutex);
    (void)sd_error_log_flush();
    ESP_LOGI(TAG, "resume: done");
}

void sd_storage_set_network_busy(bool busy)
{
    SemaphoreHandle_t mutex = storage_mutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    s_network_busy = busy;
    if (busy) {
        ESP_LOGI(TAG, "SD access blocked (network transfer active)");
    } else {
        ESP_LOGI(TAG, "SD access unblocked (network idle)");
    }
    if (mutex) xSemaphoreGive(mutex);
    if (!busy) (void)sd_error_log_flush();
}

bool sd_storage_is_network_busy(void)
{
    return s_network_busy;
}

esp_err_t sd_storage_ensure_mounted(void)
{
    SemaphoreHandle_t mutex = storage_mutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    if (s_paused || s_network_busy) {
        if (mutex) xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = mount_sd_locked();
    if (mutex) xSemaphoreGive(mutex);
    if (err == ESP_OK) (void)sd_error_log_flush();
    return err;
}

esp_err_t sd_storage_ensure_dir(const char *path)
{
    if (!path || strncmp(path, BSP_SD_MOUNT_POINT, strlen(BSP_SD_MOUNT_POINT)) != 0) {
        log_storage_error("mkdir", path, ESP_ERR_INVALID_ARG, 0, "Invalid SD directory path");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= SD_MAX_PATH_LEN) {
        log_storage_error("mkdir", path, ESP_ERR_INVALID_SIZE, 0, "SD directory path too long");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE) {
            log_storage_error("mkdir", path, err, 0, "SD mount unavailable for mkdir");
        }
        return err;
    }

    char current[SD_MAX_PATH_LEN];
    snprintf(current, sizeof(current), "%s", path);

    for (char *cursor = current + strlen(BSP_SD_MOUNT_POINT); *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (current[0] && !is_directory(current) && mkdir(current, 0775) != 0 && errno != EEXIST) {
            int saved_errno = errno;
            ESP_LOGW(TAG, "mkdir %s failed: errno=%d", current, saved_errno);
            log_storage_error("mkdir", current, ESP_FAIL, saved_errno, "mkdir failed");
            return ESP_FAIL;
        }
        *cursor = '/';
    }

    if (!is_directory(current) && mkdir(current, 0775) != 0 && errno != EEXIST) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "mkdir %s failed: errno=%d", current, saved_errno);
        log_storage_error("mkdir", current, ESP_FAIL, saved_errno, "mkdir failed");
        return ESP_FAIL;
    }
    if (!is_directory(current)) {
        log_storage_error("mkdir", current, ESP_FAIL, 0, "Directory missing after mkdir");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool sd_storage_file_exists(const char *path)
{
    /* When the network is busy (HTTPS active on SDIO Slot 1), any stat()
     * call on an SD path issues SDMMC commands on Slot 0 — the shared host
     * controller can't service both slots under load.  Return false so
     * callers don't accidentally trigger SD bus traffic during downloads.   */
    if (s_paused || s_network_busy) return false;
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t sd_storage_write_text_atomic(const char *path, const char *data, size_t len)
{
    if (!path || !data) {
        log_storage_error("write", path, ESP_ERR_INVALID_ARG, 0, "Invalid write request");
        return ESP_ERR_INVALID_ARG;
    }
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= SD_MAX_PATH_LEN) {
        log_storage_error("write", path, ESP_ERR_INVALID_SIZE, 0, "SD write path too long");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE) {
            log_storage_error("write", path, err, 0, "SD mount unavailable for write");
        }
        return err;
    }

    char tmp_path[SD_MAX_PATH_LEN];
    err = make_tmp_path(path, tmp_path, sizeof(tmp_path));
    if (err != ESP_OK) {
        log_storage_error("write", path, err, 0, "Temp path creation failed");
        return err;
    }

    FILE *file = fopen(tmp_path, "wb");
    if (!file) {
        int saved_errno = errno;
        ESP_LOGW(TAG, "open %s failed: errno=%d", tmp_path, saved_errno);
        char detail[96];
        snprintf(detail, sizeof(detail), "SD temp open failed: errno=%d", saved_errno);
        set_last_error(detail, ESP_FAIL);
        log_storage_error("open", tmp_path, ESP_FAIL, saved_errno, detail);
        return ESP_FAIL;
    }

    if (len > 0 && fwrite(data, 1, len, file) != len) {
        int saved_errno = errno;
        err = ESP_FAIL;
        set_last_error("SD write failed", err);
        log_storage_error("write", tmp_path, err, saved_errno, "SD write failed");
    }
    if (err == ESP_OK && fflush(file) != 0) {
        int saved_errno = errno;
        err = ESP_FAIL;
        set_last_error("SD flush failed", err);
        log_storage_error("flush", tmp_path, err, saved_errno, "SD flush failed");
    }
    int close_result = fclose(file);
    if (close_result != 0 && err == ESP_OK) {
        int saved_errno = errno;
        err = ESP_FAIL;
        set_last_error("SD close failed", err);
        log_storage_error("close", tmp_path, err, saved_errno, "SD close failed");
    }

    if (err == ESP_OK) {
        remove(path);
        if (rename(tmp_path, path) != 0) {
            int saved_errno = errno;
            ESP_LOGW(TAG, "rename %s -> %s failed: errno=%d", tmp_path, path, saved_errno);
            char detail[96];
            snprintf(detail, sizeof(detail), "SD rename failed: errno=%d", saved_errno);
            set_last_error(detail, ESP_FAIL);
            log_storage_error("rename", path, ESP_FAIL, saved_errno, detail);
            err = ESP_FAIL;
        }
    }

    if (err != ESP_OK) {
        remove(tmp_path);
    }
    return err;
}

const char *sd_storage_last_error(void)
{
    return s_last_error;
}

void sd_storage_status_get(sd_storage_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    SemaphoreHandle_t mutex = storage_mutex();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    out->mounted = s_mounted;
    out->paused = s_paused;
    out->network_busy = s_network_busy;
    out->last_err = s_last_err;
    snprintf(out->last_error, sizeof(out->last_error), "%s", s_last_error);
    if (mutex) xSemaphoreGive(mutex);
}
