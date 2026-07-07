#include "services.h"

#include "app_config.h"
#include "audio_library.h"
#include "audio_out.h"
#include "sd_error_log.h"
#include "sd_storage.h"
#include "ui_background.h"
#include "wled_state.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *TAG = "serial_diag";

#define SERIAL_DEBUG_LINE_MAX 640
#define SERIAL_DEBUG_TASK_STACK 4096
#define SERIAL_DEBUG_IDLE_POLL_MS 50

static TaskHandle_t s_serial_debug_task;
static bool s_stdin_ready;

static char *trim_ws(char *text)
{
    if (!text) return text;
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static char *next_token(char **cursor)
{
    if (!cursor || !*cursor) return NULL;
    char *start = trim_ws(*cursor);
    if (!*start) {
        *cursor = start;
        return NULL;
    }
    char *end = start;
    while (*end && !isspace((unsigned char)*end)) end++;
    if (*end) *end++ = '\0';
    *cursor = end;
    return start;
}

static void serial_debug_write(const char *text)
{
    if (!text) return;
    fputs(text, stdout);
    fflush(stdout);
}

static void serial_debug_reply(const char *fmt, ...)
{
    char body[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    char out[300];
    snprintf(out, sizeof(out), "\r\n[diag] %s\r\n", body);
    serial_debug_write(out);
    ESP_LOGD(TAG, "%s", body);
}

static void serial_debug_configure_stdin(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGW(TAG, "stdin unavailable: errno=%d", errno);
        return;
    }
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "stdin nonblocking failed: errno=%d", errno);
        return;
    }
    s_stdin_ready = true;
}

static bool serial_debug_read_stdin(uint8_t *ch)
{
    if (!s_stdin_ready || !ch) return false;
    ssize_t n = read(STDIN_FILENO, ch, 1);
    return n == 1;
}

static bool serial_debug_read_byte(uint8_t *ch)
{
    if (serial_debug_read_stdin(ch)) return true;
    vTaskDelay(pdMS_TO_TICKS(SERIAL_DEBUG_IDLE_POLL_MS));
    return false;
}

static void serial_debug_help(void)
{
    serial_debug_reply("commands: help, heap, sd help, bg help, audio help, rs485 help");
    serial_debug_reply("sd: sd status, sd log [count], sd flush, sd clear");
    serial_debug_reply("bg: bg status, bg download [preset], bg delete");
    serial_debug_reply("audio: audio status, audio test, audio list, audio assets, audio download, audio play <file|#>, audio stop");
    serial_debug_reply("rs485: rs485 status, rs485 ping, rs485 provision, rs485 send <json>");
}

static const char *yesno(bool value)
{
    return value ? "yes" : "no";
}

static void serial_debug_heap_line(const char *label, uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps);
    size_t free_now = heap_caps_get_free_size(caps);
    size_t min_free = heap_caps_get_minimum_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    unsigned used_pct = total && free_now < total ? (unsigned)(((total - free_now) * 100u + total / 2u) / total) : 0u;
    serial_debug_reply("heap %s: used=%u%% free=%uKB min=%uKB largest=%uKB total=%uKB",
                       label, used_pct,
                       (unsigned)(free_now / 1024u),
                       (unsigned)(min_free / 1024u),
                       (unsigned)(largest / 1024u),
                       (unsigned)(total / 1024u));
}

static void handle_heap_command(void)
{
    serial_debug_heap_line("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    serial_debug_heap_line("dma", MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    serial_debug_heap_line("psram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void handle_sd_status(void)
{
    sd_storage_status_t storage;
    sd_storage_status_get(&storage);

    sd_error_log_status_t log;
    sd_error_log_status_get(&log);

    serial_debug_reply("sd mounted=%s paused=%s network_busy=%s last=%s '%s'",
                       yesno(storage.mounted), yesno(storage.paused), yesno(storage.network_busy),
                       esp_err_to_name(storage.last_err), storage.last_error);
    serial_debug_reply("sd log path=%s buffer=%u%s recorded=%u pending=%u flushed=%u dropped=%u",
                       sd_error_log_path(), (unsigned)log.capacity,
                       log.using_fallback ? " fallback" : " psram",
                       (unsigned)log.recorded, (unsigned)log.pending,
                       (unsigned)log.flushed, (unsigned)log.dropped);
    serial_debug_reply("sd log failures=%u flushing=%s last_flush=%s '%s'",
                       (unsigned)log.flush_failures, yesno(log.flushing),
                       esp_err_to_name(log.last_flush_err), log.last_flush_detail);
}

static unsigned parse_count_arg(char *args, unsigned fallback, unsigned max_value)
{
    char *cursor = args;
    char *arg = next_token(&cursor);
    if (!arg) return fallback;

    char *end = NULL;
    long value = strtol(arg, &end, 10);
    if (!end || *trim_ws(end) != '\0' || value <= 0) return fallback;
    if ((unsigned)value > max_value) return max_value;
    return (unsigned)value;
}

static void handle_sd_log(char *args)
{
    unsigned limit = parse_count_arg(args, 8, 24);
    sd_error_log_entry_t *entries = heap_caps_calloc(limit, sizeof(*entries),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) entries = calloc(limit, sizeof(*entries));
    if (!entries) {
        serial_debug_reply("sd log failed: no memory");
        return;
    }

    size_t count = sd_error_log_recent(entries, limit);
    if (count == 0) {
        serial_debug_reply("sd log: no recent RAM entries; file=%s", sd_error_log_path());
        free(entries);
        return;
    }

    serial_debug_reply("sd log: showing %u recent RAM entries; file=%s", (unsigned)count, sd_error_log_path());
    for (size_t i = 0; i < count; i++) {
        const sd_error_log_entry_t *entry = &entries[i];
        serial_debug_reply("#%u +%ums %s/%s %s errno=%d path='%s' detail='%s'",
                           (unsigned)entry->seq, (unsigned)entry->uptime_ms,
                           entry->source, entry->operation,
                           esp_err_to_name(entry->err), entry->errno_value,
                           entry->path[0] ? entry->path : "-",
                           entry->detail[0] ? entry->detail : "-");
    }
    free(entries);
}

static void handle_sd_flush(void)
{
    esp_err_t err = sd_error_log_flush();
    sd_error_log_status_t log;
    sd_error_log_status_get(&log);
    serial_debug_reply("sd flush %s: pending=%u flushed=%u last='%s'",
                       err == ESP_OK ? "ok" : esp_err_to_name(err),
                       (unsigned)log.pending, (unsigned)log.flushed,
                       log.last_flush_detail);
}

static void handle_sd_clear(void)
{
    esp_err_t err = sd_error_log_clear();
    serial_debug_reply("sd clear %s", err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void handle_sd_command(char *args)
{
    char *cursor = args;
    char *sub = next_token(&cursor);
    if (!sub || strcasecmp(sub, "help") == 0) {
        serial_debug_reply("sd commands: sd status, sd log [count], sd flush, sd clear");
    } else if (strcasecmp(sub, "status") == 0 || strcasecmp(sub, "stat") == 0) {
        handle_sd_status();
    } else if (strcasecmp(sub, "log") == 0 || strcasecmp(sub, "tail") == 0) {
        handle_sd_log(cursor);
    } else if (strcasecmp(sub, "flush") == 0 || strcasecmp(sub, "sync") == 0) {
        handle_sd_flush();
    } else if (strcasecmp(sub, "clear") == 0 || strcasecmp(sub, "reset") == 0) {
        handle_sd_clear();
    } else {
        serial_debug_reply("unknown sd command '%s'", sub);
    }
}

static void handle_rs485_status(void)
{
    services_status_t status;
    if (services_status_get(&status) != ESP_OK) {
        serial_debug_reply("rs485 status unavailable");
        return;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    char age[32];
    if (status.wled_last_rx_ms) {
        snprintf(age, sizeof(age), "%ums", (unsigned)(now_ms - status.wled_last_rx_ms));
    } else {
        snprintf(age, sizeof(age), "never");
    }

    serial_debug_reply("rs485 ready=%s queue=%s wled=%s last_rx=%s tx=%u rx=%u dropped=%u overflow=%u",
                       yesno(status.rs485_ready), yesno(cmd_tx_is_ready()),
                       yesno(status.wled_online), age,
                       (unsigned)status.rs485_tx_lines,
                       (unsigned)status.rs485_rx_lines,
                       (unsigned)status.rs485_dropped_tx,
                       (unsigned)status.rs485_rx_overflows);

    wled_state_t wled;
    wled_state_get(&wled);
    if (wled.valid) {
        serial_debug_reply("wled snapshot: on=%s bri=%u fx=%u pal=%u ver='%s' leds=%u uptime=%us ip=%s rssi=%d signal=%u%% ch=%u ap=%s",
                           yesno(wled.on), (unsigned)wled.bri,
                           (unsigned)wled.seg0_fx, (unsigned)wled.seg0_pal,
                           wled.version[0] ? wled.version : "?",
                           (unsigned)wled.led_count,
                           (unsigned)wled.uptime_s,
                           wled.ip_addr[0] ? wled.ip_addr : "-",
                           (int)wled.wifi_rssi,
                           (unsigned)wled.wifi_signal,
                           (unsigned)wled.wifi_channel,
                           yesno(wled.wifi_ap));
    } else {
        serial_debug_reply("wled snapshot: none received yet");
    }
}

static void handle_rs485_ping(void)
{
    esp_err_t err = cmd_tx_send_json("{\"v\":true}");
    serial_debug_reply("rs485 ping %s", err == ESP_OK ? "queued" : esp_err_to_name(err));
}

static void handle_rs485_provision(void)
{
    esp_err_t err = provision_wifi_update();
    serial_debug_reply("rs485 provision %s", err == ESP_OK ? "started" : esp_err_to_name(err));
}

static void handle_rs485_send(char *args)
{
    char *json = trim_ws(args ? args : "");
    if (!json[0]) {
        serial_debug_reply("usage: rs485 send <json>");
        return;
    }
    esp_err_t err = cmd_tx_send_json(json);
    serial_debug_reply("rs485 send %s", err == ESP_OK ? "queued" : esp_err_to_name(err));
}

static void handle_rs485_command(char *args)
{
    char *cursor = args;
    char *sub = next_token(&cursor);
    if (!sub || strcasecmp(sub, "help") == 0) {
        serial_debug_reply("rs485 commands: rs485 status, rs485 ping, rs485 provision, rs485 send <json>");
    } else if (strcasecmp(sub, "status") == 0 || strcasecmp(sub, "stat") == 0) {
        handle_rs485_status();
    } else if (strcasecmp(sub, "ping") == 0 || strcasecmp(sub, "poll") == 0) {
        handle_rs485_ping();
    } else if (strcasecmp(sub, "provision") == 0 || strcasecmp(sub, "prov") == 0) {
        handle_rs485_provision();
    } else if (strcasecmp(sub, "send") == 0 || strcasecmp(sub, "tx") == 0) {
        handle_rs485_send(cursor);
    } else {
        serial_debug_reply("unknown rs485 command '%s'", sub);
    }
}

static bool parse_preset_arg(const char *arg, uint8_t *preset_out)
{
    if (!preset_out) return false;
    uint8_t count = bg_preset_count();
    if (!arg || !arg[0]) {
        app_theme_config_t cfg;
        if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);
        *preset_out = cfg.background_preset < count ? cfg.background_preset : 0;
        return true;
    }

    char *end = NULL;
    long value = strtol(arg, &end, 10);
    if (end && *trim_ws(end) == '\0') {
        if (value == 0) {
            *preset_out = 0;
            return true;
        }
        if (value > 0 && value <= count) {
            *preset_out = (uint8_t)(value - 1);
            return true;
        }
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        const bg_preset_t *preset = bg_preset_get(i);
        if (preset && preset->label && strcasecmp(arg, preset->label) == 0) {
            *preset_out = i;
            return true;
        }
    }
    return false;
}

static void handle_bg_status(void)
{
    ui_background_download_state_t state;
    ui_background_download_state_get(&state);
    if (state.busy) {
        serial_debug_reply("background busy: %s (%u/%u%s%u%%)", state.status,
                           (unsigned)state.image_index, (unsigned)state.image_total,
                           state.has_progress ? " " : " no progress ",
                           state.has_progress ? (unsigned)state.progress_pct : 0u);
        return;
    }

    app_theme_config_t cfg;
    if (app_config_theme_load(&cfg) != ESP_OK) app_config_theme_defaults(&cfg);
    uint8_t preset = cfg.background_preset < bg_preset_count() ? cfg.background_preset : 0;
    uint8_t present = 0;
    uint8_t total = 0;
    bool ready = ui_background_preset_images_present(preset, &present, &total);
    serial_debug_reply("background idle: preset=%s ready=%s files=%u/%u status='%s'",
                       bg_preset_get(preset)->label,
                       ready ? "yes" : "no",
                       (unsigned)present, (unsigned)total,
                       ui_background_status());
}

static void handle_bg_download(char *args)
{
    if (ui_background_is_busy()) {
        serial_debug_reply("background download already running: %s", ui_background_status());
        return;
    }

    uint8_t preset_index = 0;
    char *preset_arg = trim_ws(args ? args : "");
    if (!parse_preset_arg(preset_arg, &preset_index)) {
        serial_debug_reply("unknown preset '%s'", preset_arg);
        return;
    }

    const bg_preset_t *preset = bg_preset_get(preset_index);
    esp_err_t err = ui_background_download_collection_start(preset->urls,
                                                            bg_preset_url_count(preset_index),
                                                            preset_index);
    if (err == ESP_OK) {
        serial_debug_reply("started background download: preset=%s images=%u",
                           preset->label, (unsigned)bg_preset_url_count(preset_index));
    } else {
        serial_debug_reply("background download failed to start: %s", esp_err_to_name(err));
    }
}

static void handle_bg_delete(void)
{
    esp_err_t err = ui_background_delete_folder();
    if (err == ESP_OK) {
        serial_debug_reply("deleted /sdcard/walldisplay_theme and reset background state");
    } else {
        serial_debug_reply("background folder delete failed: %s (%s)",
                           esp_err_to_name(err), ui_background_status());
    }
}

static void handle_bg_command(char *args)
{
    char *cursor = args;
    char *sub = next_token(&cursor);
    if (!sub || strcasecmp(sub, "help") == 0) {
        serial_debug_reply("bg commands: bg download [preset-name|number], bg delete, bg status");
    } else if (strcasecmp(sub, "download") == 0 || strcasecmp(sub, "dl") == 0) {
        handle_bg_download(cursor);
    } else if (strcasecmp(sub, "delete") == 0 || strcasecmp(sub, "clear") == 0 || strcasecmp(sub, "rm") == 0) {
        handle_bg_delete();
    } else if (strcasecmp(sub, "status") == 0) {
        handle_bg_status();
    } else {
        serial_debug_reply("unknown bg command '%s'", sub);
    }
}

static void handle_audio_status(void)
{
    serial_debug_reply("audio ready=%s playing=%s current='%s' status='%s'",
                       yesno(audio_out_is_ready()), yesno(audio_out_is_playing()),
                       audio_out_current_path()[0] ? audio_out_current_path() : "none",
                       audio_out_last_error());
}

static bool handle_audio_resolve_arg(const char *arg, char *path, size_t path_len)
{
    if (!arg || !arg[0] || !path || path_len == 0) return false;
    path[0] = '\0';

    char *end = NULL;
    long index = strtol(arg, &end, 10);
    if (end && *trim_ws(end) == '\0' && index > 0) {
        audio_out_file_t *files = calloc(AUDIO_OUT_MAX_LISTED, sizeof(*files));
        if (!files) return false;
        size_t count = 0;
        bool resolved = false;
        if (audio_out_list_wav(files, AUDIO_OUT_MAX_LISTED, &count) != ESP_OK) {
            free(files);
            return false;
        }
        if ((size_t)index <= count) {
            snprintf(path, path_len, "%s", files[index - 1].path);
            resolved = true;
        }
        free(files);
        return resolved;
    }

    if (arg[0] == '/') {
        snprintf(path, path_len, "%s", arg);
        return true;
    }

    audio_out_file_t *files = calloc(AUDIO_OUT_MAX_LISTED, sizeof(*files));
    if (!files) return false;
    size_t count = 0;
    if (audio_out_list_wav(files, AUDIO_OUT_MAX_LISTED, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            if (strcasecmp(files[i].name, arg) == 0) {
                snprintf(path, path_len, "%s", files[i].path);
                free(files);
                return true;
            }
        }
    }
    free(files);

    snprintf(path, path_len, "%s/%s", AUDIO_OUT_DIR, arg);
    return true;
}

static void handle_audio_list(void)
{
    audio_out_file_t *files = calloc(AUDIO_OUT_MAX_LISTED, sizeof(*files));
    if (!files) {
        serial_debug_reply("audio list failed: no memory");
        return;
    }
    size_t count = 0;
    esp_err_t err = audio_out_list_wav(files, AUDIO_OUT_MAX_LISTED, &count);
    if (err != ESP_OK) {
        serial_debug_reply("audio list failed: %s (%s)", esp_err_to_name(err), audio_out_last_error());
        free(files);
        return;
    }
    if (count == 0) {
        serial_debug_reply("audio files: none in %s", AUDIO_OUT_DIR);
        free(files);
        return;
    }
    serial_debug_reply("audio files in %s:", AUDIO_OUT_DIR);
    for (size_t i = 0; i < count; i++) {
        serial_debug_reply("%u: %s", (unsigned)(i + 1), files[i].name);
    }
    free(files);
}

static void handle_audio_assets(void)
{
    uint8_t present = 0;
    uint8_t total = 0;
    (void)audio_library_assets_present(&present, &total);
    serial_debug_reply("audio library: %u/%u ready, status='%s'", (unsigned)present, (unsigned)total, audio_library_status());
    for (size_t i = 0; i < audio_library_asset_count(); i++) {
        const audio_library_asset_t *asset = audio_library_asset_get(i);
        if (asset) serial_debug_reply("%u: %s -> %s", (unsigned)(i + 1), asset->title, asset->filename);
    }
}

static void handle_audio_download(void)
{
    esp_err_t err = audio_library_download_defaults_start();
    serial_debug_reply("audio download %s", err == ESP_OK ? "started" : audio_library_status());
}

static void handle_audio_play(char *args)
{
    char *arg = trim_ws(args ? args : "");
    if (!arg[0]) {
        serial_debug_reply("usage: audio play <path|name|number>");
        return;
    }

    char path[AUDIO_OUT_PATH_MAX];
    if (!handle_audio_resolve_arg(arg, path, sizeof(path))) {
        serial_debug_reply("audio play: unable to resolve '%s'", arg);
        return;
    }
    esp_err_t err = audio_out_play_wav(path, AUDIO_OUT_DEFAULT_VOLUME);
    serial_debug_reply("audio play %s: %s", path, err == ESP_OK ? "queued" : esp_err_to_name(err));
}

static void handle_audio_command(char *args)
{
    char *cursor = args;
    char *sub = next_token(&cursor);
    if (!sub || strcasecmp(sub, "help") == 0) {
        serial_debug_reply("audio commands: audio status, audio test, audio list, audio assets, audio download, audio play <path|name|number>, audio stop");
    } else if (strcasecmp(sub, "status") == 0 || strcasecmp(sub, "stat") == 0) {
        handle_audio_status();
    } else if (strcasecmp(sub, "test") == 0 || strcasecmp(sub, "tone") == 0 || strcasecmp(sub, "chime") == 0) {
        esp_err_t err = audio_out_play_chime(AUDIO_OUT_DEFAULT_VOLUME);
        serial_debug_reply("audio test %s", err == ESP_OK ? "queued" : esp_err_to_name(err));
    } else if (strcasecmp(sub, "list") == 0 || strcasecmp(sub, "ls") == 0) {
        handle_audio_list();
    } else if (strcasecmp(sub, "assets") == 0 || strcasecmp(sub, "library") == 0) {
        handle_audio_assets();
    } else if (strcasecmp(sub, "download") == 0 || strcasecmp(sub, "dl") == 0) {
        handle_audio_download();
    } else if (strcasecmp(sub, "play") == 0) {
        handle_audio_play(cursor);
    } else if (strcasecmp(sub, "stop") == 0) {
        esp_err_t err = audio_out_stop();
        serial_debug_reply("audio stop %s", err == ESP_OK ? "queued" : esp_err_to_name(err));
    } else {
        serial_debug_reply("unknown audio command '%s'", sub);
    }
}

static void handle_serial_line(char *line)
{
    char *cursor = trim_ws(line);
    char *cmd = next_token(&cursor);
    if (!cmd) return;

    ESP_LOGD(TAG, "command: %s", cmd);

    if (strcasecmp(cmd, "bg") == 0 || strcasecmp(cmd, "background") == 0) {
        handle_bg_command(cursor);
    } else if (strcasecmp(cmd, "audio") == 0 || strcasecmp(cmd, "sound") == 0) {
        handle_audio_command(cursor);
    } else if (strcasecmp(cmd, "sd") == 0 || strcasecmp(cmd, "card") == 0 || strcasecmp(cmd, "storage") == 0) {
        handle_sd_command(cursor);
    } else if (strcasecmp(cmd, "rs485") == 0 || strcasecmp(cmd, "wled") == 0) {
        handle_rs485_command(cursor);
    } else if (strcasecmp(cmd, "heap") == 0 || strcasecmp(cmd, "mem") == 0) {
        handle_heap_command();
    } else if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        serial_debug_help();
    } else {
        serial_debug_reply("unknown command '%s'; type help", cmd);
    }
}

static void serial_debug_task(void *arg)
{
    (void)arg;
    char line[SERIAL_DEBUG_LINE_MAX];
    size_t pos = 0;
    serial_debug_reply("serial diagnostics ready; type help");

    while (1) {
        uint8_t ch = 0;
        if (!serial_debug_read_byte(&ch)) continue;
        if (ch == '\r' || ch == '\n') {
            if (pos) {
                line[pos] = '\0';
                handle_serial_line(line);
                pos = 0;
            }
        } else if (ch == '\b' || ch == 0x7F) {
            if (pos) pos--;
        } else if (isprint((unsigned char)ch) && pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

esp_err_t serial_debug_init(void)
{
    if (s_serial_debug_task) return ESP_OK;

    serial_debug_configure_stdin();

    BaseType_t ok = xTaskCreateWithCaps(serial_debug_task, "serial_diag", SERIAL_DEBUG_TASK_STACK,
                                        NULL, 3, &s_serial_debug_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(serial_debug_task, "serial_diag", SERIAL_DEBUG_TASK_STACK,
                                       NULL, 3, &s_serial_debug_task);
    if (ok != pdPASS) {
        s_serial_debug_task = NULL;
        ESP_LOGW(TAG, "serial diagnostics task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial diagnostics task started");
    return ESP_OK;
}