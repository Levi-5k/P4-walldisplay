#include "audio_library.h"

#include "audio_out.h"
#include "sd_storage.h"
#include "services.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "audio_lib";

#define AUDIO_LIBRARY_TASK_STACK      12288
#define AUDIO_LIBRARY_HTTP_BUFFER          2048
#define AUDIO_LIBRARY_HTTP_TIMEOUT_MS      30000
#define AUDIO_LIBRARY_MAX_BYTES            (6u * 1024u * 1024u)
#define AUDIO_LIBRARY_MIN_BYTES            44u
#define AUDIO_LIBRARY_READ_YIELD_MS        100
#define AUDIO_LIBRARY_POST_HTTP_SETTLE_MS  500
#define AUDIO_LIBRARY_SD_CHUNK             1024
#define AUDIO_LIBRARY_SD_YIELD_MS          2
#define AUDIO_LIBRARY_MAX_RETRIES          2
#define AUDIO_LIBRARY_WIFI_REFRESH_EVERY   3

static const audio_library_asset_t s_assets[] = {
    {"Classic alarm", "mixkit_classic_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/995/995.wav"},
    {"Digital clock buzzer", "mixkit_digital_clock_buzzer.wav", "https://assets.mixkit.co/active_storage/sfx/992/992.wav"},
    {"Alarm clock beep", "mixkit_alarm_clock_beep.wav", "https://assets.mixkit.co/active_storage/sfx/988/988.wav"},
    {"Morning clock alarm", "mixkit_morning_clock_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1003/1003.wav"},
    {"Emergency alert", "mixkit_emergency_alert.wav", "https://assets.mixkit.co/active_storage/sfx/1007/1007.wav"},
    {"Alert alarm", "mixkit_alert_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1005/1005.wav"},
    {"Warning buzzer", "mixkit_warning_buzzer.wav", "https://assets.mixkit.co/active_storage/sfx/991/991.wav"},
    {"Classic short alarm", "mixkit_classic_short_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/993/993.wav"},
    {"Interface hint", "mixkit_interface_hint.wav", "https://assets.mixkit.co/active_storage/sfx/911/911.wav"},
    {"Game wave alarm", "mixkit_game_wave_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/987/987.wav"},
    {"Facility alarm", "mixkit_facility_alarm_sound.wav", "https://assets.mixkit.co/active_storage/sfx/999/999.wav"},
    {"Rooster morning", "mixkit_rooster_morning.wav", "https://assets.mixkit.co/active_storage/sfx/2462/2462.wav"},
    {"Retro emergency", "mixkit_retro_game_emergency_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1000/1000.wav"},
    {"Hall alert", "mixkit_sound_alert_in_hall.wav", "https://assets.mixkit.co/active_storage/sfx/1006/1006.wav"},
    {"Vintage alarm", "mixkit_vintage_warning_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/990/990.wav"},
    {"Payout alarm", "mixkit_slot_machine_payout_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1996/1996.wav"},
    {"Short rooster", "mixkit_short_rooster_crowing.wav", "https://assets.mixkit.co/active_storage/sfx/2470/2470.wav"},
    {"City siren", "mixkit_city_alert_siren_loop.wav", "https://assets.mixkit.co/active_storage/sfx/1008/1008.wav"},
    {"Win alarm", "mixkit_slot_machine_win_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1995/1995.wav"},
    {"Alarm tone", "mixkit_alarm_tone.wav", "https://assets.mixkit.co/active_storage/sfx/996/996.wav"},
    {"Street alarm", "mixkit_street_public_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/997/997.wav"},
    {"Security breach", "mixkit_security_facility_breach_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/994/994.wav"},
    {"Clock beep", "mixkit_alarm_digital_clock_beep.wav", "https://assets.mixkit.co/active_storage/sfx/989/989.wav"},
    {"Winner alarm", "mixkit_classic_winner_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1997/1997.wav"},
    {"Sci-fi scan alarm", "mixkit_scanning_sci_fi_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/905/905.wav"},
    {"Battleship alarm", "mixkit_battleship_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1001/1001.wav"},
    {"Space shooter", "mixkit_space_shooter_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1002/1002.wav"},
    {"Jackpot alarm", "mixkit_casino_jackpot_alarm_and_coins.wav", "https://assets.mixkit.co/active_storage/sfx/1991/1991.wav"},
    {"Data scanner", "mixkit_data_scanner.wav", "https://assets.mixkit.co/active_storage/sfx/2847/2847.wav"},
    {"Critical alarm", "mixkit_critical_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/1004/1004.wav"},
    {"Facility ping", "mixkit_facility_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/908/908.wav"},
    {"Casino coins", "mixkit_casino_win_alarm_and_coins.wav", "https://assets.mixkit.co/active_storage/sfx/1990/1990.wav"},
    {"Spaceship alarm", "mixkit_spaceship_alarm.wav", "https://assets.mixkit.co/active_storage/sfx/998/998.wav"},
};

typedef struct {
    uint8_t count;
    uint8_t indices[AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE];
} audio_download_batch_t;

static bool s_busy;
static bool s_progress_has_pct;
static uint8_t s_progress_pct;
static uint8_t s_progress_index;
static uint8_t s_progress_total;
static char s_status[120] = "Audio library ready";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

size_t audio_library_asset_count(void)
{
    return sizeof(s_assets) / sizeof(s_assets[0]);
}

const audio_library_asset_t *audio_library_asset_get(size_t index)
{
    return index < audio_library_asset_count() ? &s_assets[index] : NULL;
}

static void audio_library_path(const audio_library_asset_t *asset, char *path, size_t path_len)
{
    if (!asset || !path || path_len == 0) return;
    snprintf(path, path_len, "%s/%s", AUDIO_OUT_DIR, asset->filename);
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

static void set_progress(uint8_t file_index, uint8_t file_total, bool has_pct, uint8_t pct)
{
    if (file_total == 0) file_total = 1;
    if (file_index == 0) file_index = 1;
    if (file_index > file_total) file_index = file_total;
    if (pct > 100) pct = 100;

    portENTER_CRITICAL(&s_state_lock);
    s_progress_index = file_index;
    s_progress_total = file_total;
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

static void *audio_lib_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return ptr;
}

static void *audio_lib_realloc(void *ptr, size_t size)
{
    void *next = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) next = realloc(ptr, size);
    return next;
}

bool audio_library_is_busy(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_busy;
    portEXIT_CRITICAL(&s_state_lock);
    return busy;
}

const char *audio_library_status(void)
{
    return s_status;
}

void audio_library_state_get(audio_library_state_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_state_lock);
    out->busy = s_busy;
    out->has_progress = s_progress_has_pct;
    out->progress_pct = s_progress_pct;
    out->file_index = s_progress_index;
    out->file_total = s_progress_total;
    memcpy(out->status, s_status, sizeof(out->status));
    out->status[sizeof(out->status) - 1] = '\0';
    portEXIT_CRITICAL(&s_state_lock);
}

bool audio_library_assets_present(uint8_t *present_count, uint8_t *total_count)
{
    uint8_t total = (uint8_t)audio_library_asset_count();
    uint8_t present = 0;

    if (sd_storage_ensure_dir(AUDIO_OUT_DIR) == ESP_OK) {
        for (uint8_t i = 0; i < total; i++) {
            char path[AUDIO_OUT_PATH_MAX];
            audio_library_path(&s_assets[i], path, sizeof(path));
            if (sd_storage_file_exists(path)) present++;
        }
    }

    if (present_count) *present_count = present;
    if (total_count) *total_count = total;
    return total > 0 && present >= total;
}

static uint8_t audio_library_select_next_batch(audio_download_batch_t *batch,
                                               uint8_t *present_count,
                                               uint8_t *total_count)
{
    uint8_t total = (uint8_t)audio_library_asset_count();
    uint8_t present = 0;
    uint8_t selected = 0;

    if (batch) memset(batch, 0, sizeof(*batch));

    (void)sd_storage_ensure_dir(AUDIO_OUT_DIR);
    for (uint8_t i = 0; i < total; i++) {
        char path[AUDIO_OUT_PATH_MAX];
        audio_library_path(&s_assets[i], path, sizeof(path));
        if (sd_storage_file_exists(path)) {
            present++;
            continue;
        }
        if (batch && selected < AUDIO_LIBRARY_DOWNLOAD_BATCH_SIZE) {
            batch->indices[selected++] = i;
        }
    }

    if (batch) batch->count = selected;
    if (present_count) *present_count = present;
    if (total_count) *total_count = total;
    return selected;
}

static bool data_is_wav(const uint8_t *data, size_t bytes)
{
    return data && bytes >= 12 &&
           memcmp(data, "RIFF", 4) == 0 &&
           memcmp(data + 8, "WAVE", 4) == 0;
}

static esp_err_t download_wav_to_memory(const audio_library_asset_t *asset,
                                        uint8_t index,
                                        uint8_t total,
                                        uint8_t **out_data,
                                        size_t *out_bytes,
                                        int *out_http_status)
{
    if (out_data) *out_data = NULL;
    if (out_bytes) *out_bytes = 0;
    if (out_http_status) *out_http_status = 0;
    if (!asset || !out_data || !out_bytes) return ESP_ERR_INVALID_ARG;

    set_statusf("Sound %u/%u: connecting", (unsigned)(index + 1), (unsigned)total);

    esp_http_client_config_t http_cfg = {
        .url = asset->url,
        .timeout_ms = AUDIO_LIBRARY_HTTP_TIMEOUT_MS,
        .buffer_size = AUDIO_LIBRARY_HTTP_BUFFER,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 3,
        .user_agent = "P4-WallDisplay/1.0",
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_ERR_NO_MEM;
    (void)esp_http_client_set_header(client, "Accept", "audio/wav,audio/x-wav,audio/*;q=0.6,*/*;q=0.1");

    esp_err_t err = esp_http_client_open(client, 0);
    int http_status = 0;
    int64_t content_len = -1;
    uint8_t *data = NULL;
    size_t bytes = 0;
    size_t capacity = 0;
    bool signature_checked = false;

    if (err == ESP_OK) {
        content_len = esp_http_client_fetch_headers(client);
        http_status = esp_http_client_get_status_code(client);
        if (out_http_status) *out_http_status = http_status;
        if (http_status != 200) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else if (content_len > (int64_t)AUDIO_LIBRARY_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    if (err == ESP_OK) {
        capacity = content_len > 0 ? (size_t)content_len : (256u * 1024u);
        if (capacity < AUDIO_LIBRARY_HTTP_BUFFER) capacity = AUDIO_LIBRARY_HTTP_BUFFER;
        if (capacity > AUDIO_LIBRARY_MAX_BYTES) capacity = AUDIO_LIBRARY_MAX_BYTES;
        data = audio_lib_malloc(capacity);
        if (!data) err = ESP_ERR_NO_MEM;
    }

    int empty_reads = 0;
    uint8_t last_progress_pct = 255;
    while (err == ESP_OK) {
        if (bytes >= capacity) {
            size_t next_capacity = capacity * 2u;
            if (next_capacity < capacity + AUDIO_LIBRARY_HTTP_BUFFER) next_capacity = capacity + AUDIO_LIBRARY_HTTP_BUFFER;
            if (next_capacity > AUDIO_LIBRARY_MAX_BYTES) next_capacity = AUDIO_LIBRARY_MAX_BYTES;
            if (next_capacity <= capacity) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            uint8_t *next = audio_lib_realloc(data, next_capacity);
            if (!next) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            data = next;
            capacity = next_capacity;
        }

        size_t available = capacity - bytes;
        if (available > AUDIO_LIBRARY_HTTP_BUFFER) available = AUDIO_LIBRARY_HTTP_BUFFER;
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
        if (bytes + (size_t)read_len > AUDIO_LIBRARY_MAX_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        bytes += (size_t)read_len;

        if (!signature_checked && bytes >= 12) {
            signature_checked = true;
            if (!data_is_wav(data, bytes)) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }

        if (content_len > 0) {
            unsigned pct = (unsigned)((bytes * 100u) / (size_t)content_len);
            if (pct > 100) pct = 100;
            if (last_progress_pct == 255 || pct >= (unsigned)last_progress_pct + 5u || pct == 100) {
                last_progress_pct = (uint8_t)pct;
                set_item_progress(index, total, (uint8_t)pct);
                set_statusf("Sound %u/%u: %u%%", (unsigned)(index + 1), (unsigned)total, pct);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_LIBRARY_READ_YIELD_MS));
    }

    if (err == ESP_OK && bytes < AUDIO_LIBRARY_MIN_BYTES) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && !data_is_wav(data, bytes)) err = ESP_ERR_INVALID_RESPONSE;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        *out_data = data;
        *out_bytes = bytes;
        data = NULL;
    }
    if (data) free(data);
    if (out_http_status && *out_http_status == 0) *out_http_status = http_status;
    return err;
}

static esp_err_t write_wav_atomic(const char *path, const uint8_t *data, size_t bytes)
{
    if (!path || !data || bytes == 0) return ESP_ERR_INVALID_ARG;

    char tmp_path[AUDIO_OUT_PATH_MAX + 8];
    int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (written <= 0 || written >= (int)sizeof(tmp_path)) return ESP_ERR_INVALID_SIZE;

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
        if (chunk > AUDIO_LIBRARY_SD_CHUNK) chunk = AUDIO_LIBRARY_SD_CHUNK;
        if (fwrite(data + offset, 1, chunk, file) != chunk) {
            err = ESP_FAIL;
            break;
        }
        offset += chunk;
        vTaskDelay(pdMS_TO_TICKS(AUDIO_LIBRARY_SD_YIELD_MS));
    }
    if (err == ESP_OK && fflush(file) != 0) err = ESP_FAIL;
    int close_result = fclose(file);
    if (close_result != 0 && err == ESP_OK) err = ESP_FAIL;

    if (err == ESP_OK) {
        remove(path);
        if (rename(tmp_path, path) != 0) {
            ESP_LOGW(TAG, "rename %s -> %s failed: errno=%d", tmp_path, path, errno);
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) remove(tmp_path);
    return err;
}

static bool wait_wifi_ready(uint32_t timeout_ms, uint32_t stable_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    TickType_t ready_since = 0;
    while (xTaskGetTickCount() < deadline) {
        services_status_t status;
        services_status_get(&status);
        bool ready = status.wifi_connected && status.ip_addr[0] && strcmp(status.ip_addr, "-") != 0;
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

static void audio_library_pause_sd(bool *paused)
{
    if (!paused || *paused) return;
    sd_storage_pause();
    *paused = true;
}

static void audio_library_resume_sd(bool *paused)
{
    if (!paused || !*paused) return;
    sd_storage_resume();
    *paused = false;
}

static bool audio_library_reset_wifi_link(const char *status_text, const char *log_text)
{
    set_statusf("%s", status_text ? status_text : "Recovering Wi-Fi link");
    ESP_LOGW(TAG, "%s", log_text ? log_text : "resetting hosted Wi-Fi link");
    esp_err_t reset_err = services_reset_wifi_link();
    if (reset_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi link reset failed: %s", esp_err_to_name(reset_err));
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }
    if (!wait_wifi_ready(30000, 2500)) {
        ESP_LOGW(TAG, "Wi-Fi link reset did not reconnect before timeout");
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi link reset and reconnected");
    vTaskDelay(pdMS_TO_TICKS(1500));
    return true;
}

static void audio_library_recover_wifi_link(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        set_statusf("Waiting for Wi-Fi link");
        ESP_LOGW(TAG, "audio download failed; waiting for Wi-Fi before retry");
        if (wait_wifi_ready(20000, 2500)) return;
    } else if (err != ESP_ERR_HTTP_CONNECT && err != ESP_FAIL && err != ESP_ERR_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        return;
    }
    (void)audio_library_reset_wifi_link("Recovering Wi-Fi link",
                                        "audio download failed; resetting hosted Wi-Fi link before retry");
}

static void download_task(void *arg)
{
    audio_download_batch_t batch = {0};
    if (arg) {
        memcpy(&batch, arg, sizeof(batch));
        free(arg);
    }

    uint8_t library_total = (uint8_t)audio_library_asset_count();
    uint8_t total = batch.count;
    uint8_t saved = 0;
    uint8_t skipped = 0;
    uint8_t failed = 0;
    bool sd_paused = false;

    if (total == 0) {
        uint8_t ready = 0;
        (void)audio_library_assets_present(&ready, &library_total);
        set_progress(1, 1, true, 100);
        set_statusf("%u timer sounds ready", (unsigned)ready);
        set_busy(false);
        vTaskDelete(NULL);
        return;
    }

    set_statusf("Preparing %u sounds", (unsigned)total);
    (void)sd_storage_ensure_dir(AUDIO_OUT_DIR);

    services_network_bulk_begin();
    for (uint8_t i = 0; i < total; i++) {
        uint8_t asset_index = batch.indices[i];
        if (asset_index >= library_total) continue;

        const audio_library_asset_t *asset = &s_assets[asset_index];
        char path[AUDIO_OUT_PATH_MAX];
        audio_library_path(asset, path, sizeof(path));
        set_progress((uint8_t)(i + 1), total, true, (uint8_t)(((unsigned)i * 100u) / total));

        if (sd_storage_file_exists(path)) {
            skipped++;
            set_statusf("%s already ready", asset->title);
            continue;
        }

        uint8_t *data = NULL;
        size_t bytes = 0;
        int http_status = 0;
        esp_err_t err = ESP_FAIL;

        for (uint8_t attempt = 0; attempt <= AUDIO_LIBRARY_MAX_RETRIES; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "retry %u/%u for %s", (unsigned)attempt,
                         (unsigned)AUDIO_LIBRARY_MAX_RETRIES, asset->title);
                set_statusf("Sound %u/%u: retry %u", (unsigned)(i + 1),
                            (unsigned)total, (unsigned)attempt);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }

            if (!wait_wifi_ready(30000, 1000)) {
                err = ESP_ERR_INVALID_STATE;
                ESP_LOGW(TAG, "Wi-Fi not ready before %s attempt %u",
                         asset->title, (unsigned)(attempt + 1));
                audio_library_recover_wifi_link(err);
                continue;
            }

            audio_library_pause_sd(&sd_paused);
            vTaskDelay(pdMS_TO_TICKS(250));

            services_https_lock();
            sd_storage_set_network_busy(true);
            data = NULL;
            bytes = 0;
            http_status = 0;
            err = download_wav_to_memory(asset, i, total, &data, &bytes, &http_status);
            sd_storage_set_network_busy(false);
            services_https_unlock();

            if (err == ESP_OK && data) break;
            if (data) {
                free(data);
                data = NULL;
            }
            ESP_LOGW(TAG, "download %s attempt %u failed: %s (http %d)",
                     asset->title, (unsigned)(attempt + 1), esp_err_to_name(err), http_status);
            audio_library_recover_wifi_link(err);
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_LIBRARY_POST_HTTP_SETTLE_MS));
        audio_library_resume_sd(&sd_paused);

        if (err != ESP_OK || !data) {
            failed++;
            set_statusf("%s failed: %s", asset->title, http_status ? "HTTP" : esp_err_to_name(err));
            ESP_LOGW(TAG, "download %s failed: %s (http %d)", asset->title, esp_err_to_name(err), http_status);
            if (data) free(data);
            continue;
        }

        err = sd_storage_ensure_dir(AUDIO_OUT_DIR);
        if (err == ESP_OK) {
            set_statusf("Saving %s (%u KB)", asset->title, (unsigned)(bytes / 1024u));
            err = write_wav_atomic(path, data, bytes);
        }
        free(data);

        if (err == ESP_OK) {
            saved++;
            ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)bytes);
            set_statusf("Saved %s", asset->title);
            if (AUDIO_LIBRARY_WIFI_REFRESH_EVERY > 0 && i + 1u < total &&
                saved % AUDIO_LIBRARY_WIFI_REFRESH_EVERY == 0) {
                audio_library_pause_sd(&sd_paused);
                (void)audio_library_reset_wifi_link("Refreshing Wi-Fi link",
                                                    "refreshing hosted Wi-Fi link between audio downloads");
                audio_library_resume_sd(&sd_paused);
            }
        } else {
            failed++;
            set_statusf("Save failed: %s", sd_storage_last_error());
            ESP_LOGW(TAG, "save %s failed: %s", path, esp_err_to_name(err));
        }
    }

    audio_library_resume_sd(&sd_paused);
    services_network_bulk_end();

    uint8_t ready = 0;
    (void)audio_library_assets_present(&ready, &library_total);
    set_progress(total, total, true, 100);
    if (ready == library_total && failed == 0) {
        set_statusf("%u timer sounds ready", (unsigned)ready);
    } else if (failed == 0 && saved > 0) {
        set_statusf("%u new sounds ready (%u/%u total)", (unsigned)saved,
                    (unsigned)ready, (unsigned)library_total);
    } else {
        set_statusf("Audio library: %u/%u ready", (unsigned)ready, (unsigned)library_total);
    }
    set_busy(false);
    vTaskDelete(NULL);
}

esp_err_t audio_library_download_defaults_start(void)
{
    if (!try_start_download()) return ESP_ERR_INVALID_STATE;

    audio_download_batch_t *batch = audio_lib_malloc(sizeof(*batch));
    if (!batch) {
        set_busy(false);
        set_statusf("Unable to start audio download");
        return ESP_ERR_NO_MEM;
    }

    uint8_t present = 0;
    uint8_t total = 0;
    uint8_t selected = audio_library_select_next_batch(batch, &present, &total);
    if (selected == 0) {
        free(batch);
        set_busy(false);
        set_statusf("%u timer sounds ready", (unsigned)present);
        return ESP_ERR_NOT_FOUND;
    }

    set_statusf("Downloading %u more timer sounds", (unsigned)selected);
    set_progress(1, selected, true, 0);

    BaseType_t ok = xTaskCreateWithCaps(download_task, "audio_dl", AUDIO_LIBRARY_TASK_STACK,
                                        batch, 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(download_task, "audio_dl", AUDIO_LIBRARY_TASK_STACK, batch, 5, NULL);
    if (ok != pdPASS) {
        free(batch);
        set_busy(false);
        set_statusf("Unable to start audio download");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}