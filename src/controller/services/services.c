#include "services.h"

#include "app_config.h"
#include "board_power.h"
#include "board_pins.h"
#include "led_state.h"
#include "sd_storage.h"
#include "sound_sync_tx.h"
#include "wled_state.h"
#include "weather/weather_api.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#if CONFIG_SOC_WIFI_SUPPORTED || CONFIG_ESP_WIFI_REMOTE_ENABLED
#define WALLDISPLAY_HAS_WIFI_DRIVER 1
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_sntp.h"
#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif
#else
#define WALLDISPLAY_HAS_WIFI_DRIVER 0
#endif

static const char *TAG = "services";

#define CMD_QUEUE_LEN       16
#define CMD_JSON_MAX        512
#define RS485_RX_LINE_MAX   32768
#define RS485_UART_BUF_SIZE 8192
#define CMD_TX_REPLY_GUARD_MS 200u
#define CMD_TX_TASK_STACK_BYTES 4096
#define RS485_RX_TASK_STACK_BYTES 8192
#define GEOIP_TASK_STACK_BYTES 6144
#define TIME_SYNC_TASK_STACK_BYTES 4096
#define TIME_SYNC_SCHED_TASK_STACK_BYTES 4096
#define TIME_VALID_MIN_EPOCH 1704067200LL
#define TIME_SYNC_RETRY_MS 60000u
#define WLED_PROVISION_INITIAL_RETRY_MS 60000u
#define WLED_PROVISION_MAX_RETRY_MS     300000u
#define WLED_LOCAL_ECHO_HOLD_MS 2500u
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

typedef struct {
    char text[CMD_JSON_MAX];
} cmd_msg_t;

typedef struct {
    uint32_t seq;
    uint32_t delay_ms;
    bool use_preset;
    uint16_t preset_id;
    led_state_t state;
} delayed_power_cmd_t;

static QueueHandle_t     s_cmd_q;
static TaskHandle_t      s_cmd_task;
static TaskHandle_t      s_rx_task;
static TaskHandle_t      s_link_task;
static TaskHandle_t      s_provision_task;
static volatile bool     s_provision_force_requested;
static TaskHandle_t      s_geoip_task;
static TaskHandle_t      s_time_sync_task;
static TaskHandle_t      s_time_sync_sched_task;
static SemaphoreHandle_t s_status_mtx;
static SemaphoreHandle_t s_https_mtx;    /* exclusive HTTPS-over-SDIO lock */
static EventGroupHandle_t s_wifi_events;
static portMUX_TYPE s_bulk_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_bulk_network_users;
static volatile uint32_t s_wled_local_echo_hold_until_ms;
static volatile uint32_t s_power_request_seq;
static bool s_suppress_next_led_publish;

static services_status_t s_status = {
    .wifi_supported = WALLDISPLAY_HAS_WIFI_DRIVER,
    .ip_addr = "-",
    .wifi_detail = "Not configured",
    .location_area = "Unknown",
    .timezone = "UTC",
    .weather_detail = "Not configured",
    .time_detail = "Not started",
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_body_t;

#if WALLDISPLAY_HAS_WIFI_DRIVER
static esp_netif_t *s_sta_netif;
static int s_wifi_retries;
static bool s_sntp_started;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_handlers_registered;
static bool s_wifi_auto_connect_enabled;
#if CONFIG_ESP_HOSTED_ENABLED
static bool s_hosted_initialized;
#endif
#endif

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static uint32_t services_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool time_before_deadline(uint32_t now_ms, uint32_t deadline_ms)
{
    return deadline_ms && (int32_t)(deadline_ms - now_ms) > 0;
}

static void note_wled_local_echo_hold_for(uint32_t hold_ms)
{
    s_wled_local_echo_hold_until_ms = services_now_ms() + hold_ms;
}

static void note_wled_local_echo_hold(void)
{
    note_wled_local_echo_hold_for(WLED_LOCAL_ECHO_HOLD_MS);
}

static void time_sync_schedule_notify(void)
{
    if (s_time_sync_sched_task) xTaskNotifyGive(s_time_sync_sched_task);
}

static bool wled_local_echo_hold_active(void)
{
    return time_before_deadline(services_now_ms(), s_wled_local_echo_hold_until_ms);
}

static void status_lock(void)
{
    if (!s_status_mtx) s_status_mtx = xSemaphoreCreateMutex();
    if (s_status_mtx) xSemaphoreTake(s_status_mtx, portMAX_DELAY);
}

static void status_unlock(void)
{
    if (s_status_mtx) xSemaphoreGive(s_status_mtx);
}

esp_err_t services_status_get(services_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    status_lock();
    *out = s_status;
    status_unlock();
    return ESP_OK;
}

void services_status_note_weather(bool configured, bool online, const char *detail)
{
    char next_detail[sizeof(s_status.weather_detail)];
    snprintf(next_detail, sizeof(next_detail), "%s", detail ? detail : "");

    status_lock();
    bool changed = s_status.weather_configured != configured ||
                   s_status.weather_online != online ||
                   strcmp(s_status.weather_detail, next_detail) != 0;
    s_status.weather_configured = configured;
    s_status.weather_online = online;
    copy_text(s_status.weather_detail, sizeof(s_status.weather_detail), next_detail);
    s_status.weather_status_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    status_unlock();

    if (changed) {
        ESP_LOGI(TAG, "Weather status: configured=%d online=%d detail='%s'",
                 configured, online, next_detail);
    }
}

static void status_note_wifi_config(bool configured, const char *ssid)
{
    status_lock();
    s_status.wifi_supported = WALLDISPLAY_HAS_WIFI_DRIVER;
    s_status.wifi_configured = configured;
    copy_text(s_status.wifi_ssid, sizeof(s_status.wifi_ssid), configured ? ssid : "");
    if (!configured) {
        s_status.wifi_connected = false;
        copy_text(s_status.ip_addr, sizeof(s_status.ip_addr), "-");
        copy_text(s_status.wifi_detail, sizeof(s_status.wifi_detail), "Not configured");
        s_status.wifi_disconnect_reason = 0;
    }
    status_unlock();
}

static void status_note_wifi_detail(const char *detail, int reason)
{
    status_lock();
    copy_text(s_status.wifi_detail, sizeof(s_status.wifi_detail), detail);
    s_status.wifi_disconnect_reason = reason;
    status_unlock();
}

static void status_note_wifi_connected(bool connected, const char *ip, const char *detail)
{
    status_lock();
    s_status.wifi_connected = connected;
    copy_text(s_status.ip_addr, sizeof(s_status.ip_addr), connected ? ip : "-");
    if (detail) copy_text(s_status.wifi_detail, sizeof(s_status.wifi_detail), detail);
    if (connected) s_status.wifi_disconnect_reason = 0;
    status_unlock();
}

static bool status_wifi_is_connected(void)
{
    status_lock();
    bool connected = s_status.wifi_connected;
    status_unlock();
    return connected;
}

static void status_note_location(bool ready, const char *area, const char *timezone, int offset_sec,
                                 bool has_coords, int32_t lat_x1e6, int32_t lon_x1e6)
{
    status_lock();
    s_status.location_ready = ready;
    s_status.location_has_coords = has_coords;
    copy_text(s_status.location_area, sizeof(s_status.location_area), area && area[0] ? area : "Unknown");
    copy_text(s_status.timezone, sizeof(s_status.timezone), timezone && timezone[0] ? timezone : "UTC");
    s_status.timezone_offset_sec = offset_sec;
    s_status.location_lat_x1e6 = has_coords ? lat_x1e6 : 0;
    s_status.location_lon_x1e6 = has_coords ? lon_x1e6 : 0;
    status_unlock();
}

static void status_note_time_sync(bool started, bool synced, const char *detail)
{
    status_lock();
    s_status.time_sync_started = started;
    s_status.time_synced = synced;
    copy_text(s_status.time_detail, sizeof(s_status.time_detail), detail);
    s_status.time_sync_status_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    status_unlock();
}

static void status_note_rs485(bool ready)
{
    status_lock();
    s_status.rs485_ready = ready;
    status_unlock();
}

static void status_note_rs485_tx(void)
{
    status_lock();
    s_status.rs485_tx_lines++;
    status_unlock();
}

static void status_note_rs485_drop(void)
{
    status_lock();
    s_status.rs485_dropped_tx++;
    status_unlock();
}

static void status_note_rs485_overflow(void)
{
    status_lock();
    s_status.rs485_rx_overflows++;
    status_unlock();
}

static void status_note_wled_rx(void)
{
    status_lock();
    s_status.rs485_rx_lines++;
    s_status.wled_online = true;
    s_status.wled_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    status_unlock();
}

static void status_note_wled_online(bool online)
{
    status_lock();
    s_status.wled_online = online;
    status_unlock();
}

static void status_note_audio(bool audio, bool fft, bool sync)
{
    status_lock();
    s_status.audio_ready = audio;
    s_status.fft_ready = fft;
    s_status.sound_sync_ready = sync;
    status_unlock();
}

static bool read_wifi_creds(char *ssid, size_t ssid_len, char *psk, size_t psk_len)
{
    if (ssid && ssid_len) ssid[0] = '\0';
    if (psk && psk_len) psk[0] = '\0';

    app_wifi_config_t cfg;
    if (app_config_wifi_load(&cfg) != ESP_OK || !cfg.configured) return false;

    if (ssid && ssid_len) snprintf(ssid, ssid_len, "%s", cfg.ssid);
    if (psk && psk_len) snprintf(psk, psk_len, "%s", cfg.psk);
    return cfg.ssid[0] != '\0';
}

static bool weather_location_cached(void)
{
    app_weather_config_t cfg;
    return app_config_weather_load(&cfg) == ESP_OK && cfg.lat_x1e6 && cfg.lon_x1e6;
}

static esp_err_t append_http_body(http_body_t *body, const char *data, size_t len)
{
    if (!body || !data || len == 0) return ESP_OK;
    if (body->len + len + 1 > body->cap) {
        size_t new_cap = body->cap ? body->cap * 2 : 1024;
        while (new_cap < body->len + len + 1) new_cap *= 2;
        if (new_cap > 4096) return ESP_ERR_NO_MEM;
        char *next = realloc(body->data, new_cap);
        if (!next) return ESP_ERR_NO_MEM;
        body->data = next;
        body->cap = new_cap;
    }
    memcpy(body->data + body->len, data, len);
    body->len += len;
    body->data[body->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_body_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
        return append_http_body((http_body_t *)evt->user_data, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static void apply_timezone_offset(int offset_sec)
{
    int west_minutes = -(offset_sec / 60);
    int abs_minutes = west_minutes < 0 ? -west_minutes : west_minutes;
    int hours = abs_minutes / 60;
    int minutes = abs_minutes % 60;
    char posix_tz[24];

    if (west_minutes == 0) {
        snprintf(posix_tz, sizeof(posix_tz), "UTC0");
    } else if (minutes) {
        snprintf(posix_tz, sizeof(posix_tz), "LOC%s%d:%02d",
                 west_minutes < 0 ? "-" : "", hours, minutes);
    } else {
        snprintf(posix_tz, sizeof(posix_tz), "LOC%s%d",
                 west_minutes < 0 ? "-" : "", hours);
    }

    setenv("TZ", posix_tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone offset applied: %s", posix_tz);
}

static void format_utc_offset(char *out, size_t out_len, int offset_sec)
{
    int minutes = offset_sec / 60;
    char sign = minutes >= 0 ? '+' : '-';
    if (minutes < 0) minutes = -minutes;
    snprintf(out, out_len, "UTC%c%02d:%02d", sign, minutes / 60, minutes % 60);
}

void services_note_weather_timezone(int32_t lat_x1e6, int32_t lon_x1e6,
                                    int offset_sec, const char *city, const char *country)
{
    char timezone[48];
    char area[72];
    format_utc_offset(timezone, sizeof(timezone), offset_sec);

    if (city && city[0] && country && country[0]) {
        snprintf(area, sizeof(area), "%s, %s", city, country);
    } else if (city && city[0]) {
        snprintf(area, sizeof(area), "%s", city);
    } else {
        snprintf(area, sizeof(area), "Saved location");
    }

    apply_timezone_offset(offset_sec);
    status_lock();
    s_status.location_ready = true;
    s_status.location_has_coords = true;
    bool area_unknown = !s_status.location_area[0] ||
                        strcmp(s_status.location_area, "Unknown") == 0 ||
                        strcmp(s_status.location_area, "Saved location") == 0;
    if (area_unknown) copy_text(s_status.location_area, sizeof(s_status.location_area), area);
    copy_text(s_status.timezone, sizeof(s_status.timezone), timezone);
    s_status.timezone_offset_sec = offset_sec;
    s_status.location_lat_x1e6 = lat_x1e6;
    s_status.location_lon_x1e6 = lon_x1e6;
    status_unlock();
    ESP_LOGI(TAG, "Weather timezone applied: %s", timezone);
    time_sync_schedule_notify();
}

static int32_t geo_round_x1e6(double value)
{
    double scaled = value * 1000000.0;
    return (int32_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

typedef struct {
    char area[72];
    char timezone[48];
    int offset_sec;
    bool has_coords;
    int32_t lat_x1e6;
    int32_t lon_x1e6;
} geoip_result_t;

static void geoip_format_area(char *area, size_t area_len,
                              const char *city_text, const char *region_text, const char *country_text)
{
    if (city_text && city_text[0] && region_text && region_text[0]) {
        snprintf(area, area_len, "%s, %s", city_text, region_text);
    } else if (city_text && city_text[0]) {
        snprintf(area, area_len, "%s", city_text);
    } else if (region_text && region_text[0]) {
        snprintf(area, area_len, "%s", region_text);
    } else {
        snprintf(area, area_len, "%s", country_text && country_text[0] ? country_text : "Unknown");
    }
}

static esp_err_t geoip_http_get(const char *url, const char *provider, http_body_t *body)
{
    if (services_network_bulk_active()) return ESP_ERR_INVALID_STATE;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .event_handler = http_body_event_cb,
        .user_data = body,
        .max_redirection_count = 2,
        .user_agent = "P4-WallDisplay/1.0",
    };

    ESP_LOGI(TAG, "IP location lookup via %s", provider);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    services_network_bulk_begin();
    services_https_lock();
    sd_storage_set_network_busy(true);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    sd_storage_set_network_busy(false);
    services_https_unlock();
    services_network_bulk_end();
    if (err != ESP_OK) return err;
    if (status != 200 || !body->data || body->len == 0) {
        ESP_LOGW(TAG, "%s returned HTTP %d", provider, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t geoip_parse_ip_api(const char *json, size_t len, geoip_result_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *api_status = cJSON_GetObjectItem(root, "status");
    cJSON *city = cJSON_GetObjectItem(root, "city");
    cJSON *region = cJSON_GetObjectItem(root, "regionName");
    cJSON *country = cJSON_GetObjectItem(root, "country");
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    cJSON *offset = cJSON_GetObjectItem(root, "offset");
    cJSON *lat = cJSON_GetObjectItem(root, "lat");
    cJSON *lon = cJSON_GetObjectItem(root, "lon");

    if (!cJSON_IsString(api_status) || strcmp(api_status->valuestring, "success") != 0 ||
        !cJSON_IsString(timezone) || !cJSON_IsNumber(offset)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *city_text = cJSON_IsString(city) ? city->valuestring : "";
    const char *region_text = cJSON_IsString(region) ? region->valuestring : "";
    const char *country_text = cJSON_IsString(country) ? country->valuestring : "";
    geoip_format_area(out->area, sizeof(out->area), city_text, region_text, country_text);
    snprintf(out->timezone, sizeof(out->timezone), "%s", timezone->valuestring);
    out->offset_sec = (int)cJSON_GetNumberValue(offset);
    out->has_coords = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    out->lat_x1e6 = out->has_coords ? geo_round_x1e6(cJSON_GetNumberValue(lat)) : 0;
    out->lon_x1e6 = out->has_coords ? geo_round_x1e6(cJSON_GetNumberValue(lon)) : 0;

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t geoip_parse_ipwhois(const char *json, size_t len, geoip_result_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *success = cJSON_GetObjectItem(root, "success");
    cJSON *city = cJSON_GetObjectItem(root, "city");
    cJSON *region = cJSON_GetObjectItem(root, "region");
    cJSON *country = cJSON_GetObjectItem(root, "country");
    cJSON *lat = cJSON_GetObjectItem(root, "latitude");
    cJSON *lon = cJSON_GetObjectItem(root, "longitude");
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    cJSON *timezone_id = cJSON_GetObjectItem(timezone, "id");
    cJSON *offset = cJSON_GetObjectItem(timezone, "offset");

    if (!cJSON_IsBool(success) || !cJSON_IsTrue(success) ||
        !cJSON_IsString(timezone_id) || !cJSON_IsNumber(offset)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *city_text = cJSON_IsString(city) ? city->valuestring : "";
    const char *region_text = cJSON_IsString(region) ? region->valuestring : "";
    const char *country_text = cJSON_IsString(country) ? country->valuestring : "";
    geoip_format_area(out->area, sizeof(out->area), city_text, region_text, country_text);
    snprintf(out->timezone, sizeof(out->timezone), "%s", timezone_id->valuestring);
    out->offset_sec = (int)cJSON_GetNumberValue(offset);
    out->has_coords = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    out->lat_x1e6 = out->has_coords ? geo_round_x1e6(cJSON_GetNumberValue(lat)) : 0;
    out->lon_x1e6 = out->has_coords ? geo_round_x1e6(cJSON_GetNumberValue(lon)) : 0;

    cJSON_Delete(root);
    return ESP_OK;
}

static bool weather_coords_match(int32_t left_lat_x1e6, int32_t left_lon_x1e6,
                                 int32_t right_lat_x1e6, int32_t right_lon_x1e6)
{
    return left_lat_x1e6 == right_lat_x1e6 && left_lon_x1e6 == right_lon_x1e6;
}

static bool geoip_should_refresh_weather(const geoip_result_t *result)
{
    if (!result || !result->has_coords) return false;

    services_status_t status;
    if (services_status_get(&status) == ESP_OK && status.location_has_coords) {
        return !weather_coords_match(status.location_lat_x1e6, status.location_lon_x1e6,
                                     result->lat_x1e6, result->lon_x1e6);
    }

    app_weather_config_t cached;
    if (app_config_weather_load(&cached) == ESP_OK && cached.lat_x1e6 && cached.lon_x1e6) {
        return !weather_coords_match(cached.lat_x1e6, cached.lon_x1e6,
                                     result->lat_x1e6, result->lon_x1e6);
    }

    return true;
}

static esp_err_t geoip_apply_result(const geoip_result_t *result, const char *provider)
{
    if (!result || !result->timezone[0]) return ESP_ERR_INVALID_ARG;
    bool refresh_weather = geoip_should_refresh_weather(result);
    apply_timezone_offset(result->offset_sec);
    status_note_location(true, result->area, result->timezone, result->offset_sec,
                         result->has_coords, result->lat_x1e6, result->lon_x1e6);
    time_sync_schedule_notify();

    if (result->has_coords) {
        esp_err_t save_err = app_config_weather_save_location(result->lat_x1e6, result->lon_x1e6);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "IP location cache save failed: %s", esp_err_to_name(save_err));
        }
        ESP_LOGI(TAG, "IP location via %s: %s, %s (%ld,%ld)", provider,
                 result->area, result->timezone, (long)result->lat_x1e6, (long)result->lon_x1e6);
    } else {
        ESP_LOGW(TAG, "IP location via %s had no coordinates: %s, %s", provider,
                 result->area, result->timezone);
    }
    if (refresh_weather) (void)weather_api_request_refresh();
    return result->has_coords ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t geoip_fetch_once(void)
{
    typedef esp_err_t (*geoip_parser_t)(const char *, size_t, geoip_result_t *);
    typedef struct {
        const char *provider;
        const char *url;
        geoip_parser_t parser;
    } geoip_provider_t;

    static const geoip_provider_t providers[] = {
        {
            "ip-api",
            "http://ip-api.com/json/?fields=status,message,city,regionName,country,lat,lon,timezone,offset",
            geoip_parse_ip_api,
        },
        {
            "ipwho.is",
            "http://ipwho.is/?fields=success,message,city,region,country,latitude,longitude,timezone",
            geoip_parse_ipwhois,
        },
    };

    esp_err_t last_err = ESP_ERR_INVALID_RESPONSE;
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        http_body_t body = {0};
        esp_err_t err = geoip_http_get(providers[i].url, providers[i].provider, &body);
        if (err == ESP_OK) {
            geoip_result_t result = {0};
            err = providers[i].parser(body.data, body.len, &result);
            if (err == ESP_OK) err = geoip_apply_result(&result, providers[i].provider);
        }
        free(body.data);
        if (err == ESP_OK) return ESP_OK;
        last_err = err;
        ESP_LOGW(TAG, "IP location via %s failed: %s", providers[i].provider, esp_err_to_name(err));
    }
    return last_err;
}

static void geoip_worker(void *arg)
{
    (void)arg;
    /* Brief pause so SNTP can finish its single UDP exchange before we
     * start a TCP connection.  The SD-backed background deferral
     * (s_sd_background_allowed) prevents the real SDMMC slot conflict. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!status_wifi_is_connected()) break;
        esp_err_t err = geoip_fetch_once();
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "IP location lookup failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    s_geoip_task = NULL;
    vTaskDelete(NULL);
}

static void geoip_start(void)
{
    if (s_geoip_task) {
        ESP_LOGI(TAG, "IP location lookup already running");
        return;
    }
    BaseType_t ok = xTaskCreateWithCaps(geoip_worker, "geoip", GEOIP_TASK_STACK_BYTES,
                                        NULL, 4, &s_geoip_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(geoip_worker, "geoip", GEOIP_TASK_STACK_BYTES,
                                       NULL, 4, &s_geoip_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Unable to start IP location lookup");
    } else {
        ESP_LOGI(TAG, "IP location lookup started");
    }
}

esp_err_t services_location_request_refresh(void)
{
    if (!status_wifi_is_connected()) {
        ESP_LOGW(TAG, "IP location refresh skipped: Wi-Fi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "IP location refresh requested");
    geoip_start();
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Wi-Fi station
 * -------------------------------------------------------------------------- */

#if WALLDISPLAY_HAS_WIFI_DRIVER
static bool system_time_is_valid(void)
{
    time_t now = time(NULL);
    return (int64_t)now >= TIME_VALID_MIN_EPOCH;
}

static uint16_t time_sync_interval_min_from_tuning(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    if (tuning.time_sync_interval_min < 60) tuning.time_sync_interval_min = 60;
    if (tuning.time_sync_interval_min > 1440) tuning.time_sync_interval_min = 1440;
    return tuning.time_sync_interval_min;
}

static uint8_t time_sync_hour_from_tuning(void)
{
    app_tuning_config_t tuning;
    if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);
    return tuning.time_sync_hour < 24 ? tuning.time_sync_hour : 3;
}

static uint32_t time_sync_interval_ms_from_tuning(void)
{
    return (uint32_t)time_sync_interval_min_from_tuning() * 60u * 1000u;
}

static uint32_t time_sync_delay_ms_from_tuning(void)
{
    if (!system_time_is_valid()) return TIME_SYNC_RETRY_MS;

    time_t now = time(NULL);
    struct tm target;
    localtime_r(&now, &target);
    target.tm_hour = time_sync_hour_from_tuning();
    target.tm_min = 0;
    target.tm_sec = 0;

    time_t target_time = mktime(&target);
    int64_t interval_s = (int64_t)time_sync_interval_min_from_tuning() * 60LL;
    if (interval_s < 3600LL) interval_s = 3600LL;
    if (interval_s > 86400LL) interval_s = 86400LL;

    while (target_time <= now) target_time += (time_t)interval_s;
    int64_t delay_s = (int64_t)(target_time - now);
    if (delay_s < 60LL) delay_s = 60LL;
    if (delay_s > 86400LL) delay_s = 86400LL;
    return (uint32_t)delay_s * 1000u;
}

static void sntp_sync_cb(struct timeval *tv)
{
    time_t now = tv ? (time_t)tv->tv_sec : time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char stamp[40];
    if (lt.tm_year + 1900 >= 2024) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &lt);
    } else {
        snprintf(stamp, sizeof(stamp), "epoch %lld", (long long)now);
    }
    status_note_time_sync(true, system_time_is_valid(), "Synced");
    ESP_LOGI(TAG, "SNTP time synced: %s", stamp);
}

static void start_sntp_once(void)
{
    bool valid = system_time_is_valid();
    uint16_t interval_min = time_sync_interval_min_from_tuning();
    esp_sntp_set_sync_interval((uint32_t)interval_min * 60u * 1000u);
    if (s_sntp_started) {
        status_note_time_sync(true, valid, valid ? "Synced" : "Syncing");
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_setservername(0, "pool.ntp.org");
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_setservername(1, "time.nist.gov");
#endif
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 2
    esp_sntp_setservername(2, "time.google.com");
#endif
    esp_sntp_init();
    s_sntp_started = true;
    status_note_time_sync(true, valid, valid ? "Synced" : "Syncing");
    ESP_LOGI(TAG, "SNTP started (%d server slots, %u min interval)",
             CONFIG_LWIP_SNTP_MAX_SERVERS, (unsigned)interval_min);
}

static void time_sync_request_now(const char *reason)
{
    start_sntp_once();
    if (!status_wifi_is_connected()) {
        status_note_time_sync(s_sntp_started, system_time_is_valid(),
                              system_time_is_valid() ? "Synced" : "Waiting for Wi-Fi");
        return;
    }
    if (s_sntp_started) {
        bool restarted = esp_sntp_restart();
        status_note_time_sync(true, system_time_is_valid(), restarted ? "Syncing" : "Sync pending");
        ESP_LOGI(TAG, "SNTP sync requested%s%s (restart=%d)",
                 reason ? ": " : "", reason ? reason : "", restarted);
    }
}

static void time_sync_schedule_worker(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t delay_ms = time_sync_delay_ms_from_tuning();
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms)) > 0) continue;
        if (status_wifi_is_connected()) time_sync_request_now("scheduled");
    }
}

static void time_sync_schedule_start(void)
{
    if (s_time_sync_sched_task) return;
    BaseType_t ok = xTaskCreateWithCaps(time_sync_schedule_worker, "time_sched", TIME_SYNC_SCHED_TASK_STACK_BYTES,
                                        NULL, 3, &s_time_sync_sched_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(time_sync_schedule_worker, "time_sched", TIME_SYNC_SCHED_TASK_STACK_BYTES,
                                       NULL, 3, &s_time_sync_sched_task);
    if (ok != pdPASS) {
        s_time_sync_sched_task = NULL;
        ESP_LOGW(TAG, "Unable to start periodic time sync scheduler");
    }
}

static void time_sync_worker(void *arg)
{
    (void)arg;
    for (int attempt = 0; attempt < 4; attempt++) {
        for (int second = 0; second < 15; second++) {
            if (!status_wifi_is_connected()) {
                status_note_time_sync(s_sntp_started, system_time_is_valid(),
                                      system_time_is_valid() ? "Synced" : "Waiting for Wi-Fi");
                s_time_sync_task = NULL;
                vTaskDelete(NULL);
            }
            if (system_time_is_valid()) {
                status_note_time_sync(true, true, "Synced");
                s_time_sync_task = NULL;
                vTaskDelete(NULL);
            }
            sntp_sync_status_t sync_status = esp_sntp_get_sync_status();
            status_note_time_sync(s_sntp_started, false,
                                  sync_status == SNTP_SYNC_STATUS_IN_PROGRESS ? "Adjusting clock" :
                                  attempt ? "Retrying SNTP" : "Syncing");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (system_time_is_valid() || !status_wifi_is_connected()) break;
        status_note_time_sync(s_sntp_started, false, "Retrying SNTP");
        if (s_sntp_started) {
            bool restarted = esp_sntp_restart();
            ESP_LOGW(TAG, "SNTP retry %d: restart=%d", attempt + 1, restarted);
        } else {
            start_sntp_once();
        }
    }

    if (system_time_is_valid()) {
        status_note_time_sync(true, true, "Synced");
    } else {
        status_note_time_sync(s_sntp_started, false, status_wifi_is_connected() ? "Time sync pending" : "Waiting for Wi-Fi");
        ESP_LOGW(TAG, "SNTP did not produce a valid clock during boot window");
    }
    s_time_sync_task = NULL;
    vTaskDelete(NULL);
}

static void time_sync_start_monitor(void)
{
    start_sntp_once();
    time_sync_schedule_start();
    if (system_time_is_valid()) {
        status_note_time_sync(true, true, "Synced");
        return;
    }
    if (s_time_sync_task) return;
    BaseType_t ok = xTaskCreateWithCaps(time_sync_worker, "time_sync", TIME_SYNC_TASK_STACK_BYTES,
                                        NULL, 4, &s_time_sync_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(time_sync_worker, "time_sync", TIME_SYNC_TASK_STACK_BYTES,
                                       NULL, 4, &s_time_sync_task);
    if (ok != pdPASS) {
        s_time_sync_task = NULL;
        status_note_time_sync(s_sntp_started, false, "Monitor failed");
        ESP_LOGW(TAG, "Unable to start time sync monitor");
    }
}

esp_err_t services_time_sync_apply_tuning(void)
{
    uint16_t interval_min = time_sync_interval_min_from_tuning();
    esp_sntp_set_sync_interval(time_sync_interval_ms_from_tuning());
    if (s_time_sync_sched_task) xTaskNotifyGive(s_time_sync_sched_task);

    if (!s_sntp_started) {
        ESP_LOGI(TAG, "SNTP schedule set to every %u min at %02u:00; client has not started yet",
                 (unsigned)interval_min, (unsigned)time_sync_hour_from_tuning());
        return ESP_OK;
    }

    bool valid = system_time_is_valid();
    if (!status_wifi_is_connected()) {
        status_note_time_sync(true, valid, valid ? "Synced" : "Waiting for Wi-Fi");
        ESP_LOGI(TAG, "SNTP schedule set to every %u min at %02u:00; waiting for Wi-Fi",
                 (unsigned)interval_min, (unsigned)time_sync_hour_from_tuning());
        return ESP_OK;
    }

    status_note_time_sync(true, valid, valid ? "Synced" : "Sync pending");
    ESP_LOGI(TAG, "SNTP schedule set to every %u min at %02u:00",
             (unsigned)interval_min, (unsigned)time_sync_hour_from_tuning());
    return ESP_OK;
}

static bool wifi_disconnect_reason_is_credentials(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_802_1X_AUTH_FAILED;
}

static const char *wifi_disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "Incorrect password";
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found";
    case WIFI_REASON_ASSOC_FAIL:
        return "Association failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal lost";
    default:
        return "Disconnected";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_wifi_auto_connect_enabled) status_note_wifi_detail("Connecting", 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = event ? event->reason : 0;
        const char *detail = wifi_disconnect_reason_text(reason);
        if (!s_wifi_auto_connect_enabled) return;
        if (wifi_disconnect_reason_is_credentials(reason) && s_wifi_retries < 2) {
            s_wifi_retries++;
            status_note_wifi_connected(false, NULL, "Retrying Wi-Fi");
            status_note_wifi_detail("Retrying Wi-Fi", 0);
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi auth failed; retry %d before reporting failure", s_wifi_retries);
            return;
        }
        status_note_wifi_connected(false, NULL, detail);
        status_note_wifi_detail(detail, reason);
        if (wifi_disconnect_reason_is_credentials(reason) || reason == WIFI_REASON_NO_AP_FOUND) {
            s_wifi_auto_connect_enabled = false;
            ESP_LOGW(TAG, "Wi-Fi connection failed: %s (%u)", detail, reason);
            if (s_wifi_events) xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            return;
        }
        if (s_wifi_retries < 8) {
            s_wifi_retries++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %d", s_wifi_retries);
        } else if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi got IP %s", ip);
        s_wifi_retries = 0;
        status_note_wifi_connected(true, ip, "Connected");
        time_sync_start_monitor();
        if (weather_location_cached()) {
            ESP_LOGI(TAG, "Using cached weather location; skipping boot GeoIP");
        } else {
            geoip_start();
        }
        (void)weather_api_request_refresh();
        if (s_wifi_events) xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_driver_prepare(bool register_handlers)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

#if CONFIG_ESP_HOSTED_ENABLED
    if (!s_hosted_initialized) {
        ESP_LOGI(TAG, "Starting ESP-Hosted link to Wi-Fi coprocessor");
        err = esp_hosted_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_hosted_connect_to_slave();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed: %s", esp_err_to_name(err));
            return err;
        }
        s_hosted_initialized = true;
        ESP_LOGI(TAG, "ESP-Hosted link ready");
    }
#endif

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) return ESP_ERR_NO_MEM;
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#if CONFIG_ESP_HOSTED_ENABLED
            if (s_hosted_initialized) {
                (void)esp_hosted_deinit();
                s_hosted_initialized = false;
            }
#endif
            return err;
        }
        s_wifi_initialized = true;
    }

    if (register_handlers && !s_wifi_handlers_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                                wifi_event_handler, NULL, NULL),
                            TAG, "wifi handler register failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                                wifi_event_handler, NULL, NULL),
                            TAG, "ip handler register failed");
        s_wifi_handlers_registered = true;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi power save failed");
    return ESP_OK;
}
#endif

esp_err_t wifi_radio_preinit(void)
{
#if WALLDISPLAY_HAS_WIFI_DRIVER
    return wifi_driver_prepare(false);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if WALLDISPLAY_HAS_WIFI_DRIVER
static esp_err_t wifi_start_with_credentials(const char *ssid, const char *psk)
{
    esp_err_t err = wifi_driver_prepare(true);
    if (err != ESP_OK) return err;

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", psk);
    wifi_config.sta.threshold.authmode = psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_wifi_auto_connect_enabled = false;
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi config failed");
    s_wifi_retries = 0;
    status_note_wifi_config(true, ssid);
    status_note_wifi_detail("Connecting", 0);
    if (s_wifi_events) xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
    }
    s_wifi_auto_connect_enabled = true;
    (void)esp_wifi_connect();
    ESP_LOGI(TAG, "Wi-Fi STA start requested for SSID '%s'", ssid);
    return ESP_OK;
}
#else
esp_err_t services_time_sync_apply_tuning(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t wifi_start_with_credentials(const char *ssid, const char *psk)
{
    (void)ssid;
    (void)psk;
    ESP_LOGW(TAG, "Wi-Fi credentials are present, but esp_hosted/esp_wifi_remote is not enabled");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

esp_err_t wifi_sta_init(void)
{
    char ssid[33];
    char psk[65];
    bool have_creds = read_wifi_creds(ssid, sizeof(ssid), psk, sizeof(psk));
    status_note_wifi_config(have_creds, have_creds ? ssid : NULL);

    if (!have_creds) {
#if WALLDISPLAY_HAS_WIFI_DRIVER
        s_wifi_auto_connect_enabled = false;
        if (s_wifi_started) {
            (void)esp_wifi_disconnect();
            (void)esp_wifi_stop();
            s_wifi_started = false;
        }
#endif
        ESP_LOGW(TAG, "Wi-Fi credentials missing in NVS namespace wifi_creds");
        return ESP_ERR_NOT_FOUND;
    }

    return wifi_start_with_credentials(ssid, psk);
}

esp_err_t wifi_sta_connect(const char *ssid, const char *psk)
{
    esp_err_t err = app_config_wifi_save(ssid, psk);
    if (err != ESP_OK) return err;
    return wifi_start_with_credentials(ssid, psk ? psk : "");
}

esp_err_t services_reset_wifi_link(void)
{
#if !WALLDISPLAY_HAS_WIFI_DRIVER || !CONFIG_ESP_HOSTED_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_LOGW(TAG, "Resetting SDIO/WiFi link (C6 coprocessor will reboot)");
    s_wifi_auto_connect_enabled = false;
    if (s_wifi_events) xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    status_note_wifi_connected(false, NULL, "Resetting Wi-Fi");
    status_note_wifi_detail("Resetting Wi-Fi", 0);

    /* ── Kill the C6 immediately via GPIO reset ──
     * Pull the reset pin LOW before Wi-Fi/netif teardown.  If the hosted
     * transport is already degraded, even cleanup traffic can trip the SDIO
     * driver's unrecoverable state path.                                */
    gpio_set_level(BSP_C6_RST, 0);        /* hold C6 in reset (active-high EN) */
    ESP_LOGI(TAG, "C6 reset pin pulled LOW, waiting for SDIO bus idle");
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_event_sta_disconnected_t disconnect_event = {0};
    disconnect_event.reason = WIFI_REASON_CONNECTION_FAIL;
    (void)esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                         &disconnect_event, sizeof(disconnect_event),
                         pdMS_TO_TICKS(250));
    vTaskDelay(pdMS_TO_TICKS(100));
    (void)esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL, 0,
                         pdMS_TO_TICKS(250));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ── Tear down host-side drivers (slave is dead, no SDIO traffic) ── */
    (void)esp_hosted_deinit();             /* clean up host SDIO driver */
    s_hosted_initialized = false;
    vTaskDelay(pdMS_TO_TICKS(100));

    /* WiFi deinit after transport is down — no SDIO commands possible */
    if (s_wifi_started) (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    s_wifi_started = false;
    s_wifi_initialized = false;
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    /* ── Rebuild ── wifi_driver_prepare re-inits hosted (which resets the
     * C6 via GPIO 54) and the WiFi driver with all guards.               */
    esp_err_t err = wifi_driver_prepare(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "link reset: wifi_driver_prepare failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Re-read stored credentials and reconnect */
    char ssid[33] = {0}, psk[65] = {0};
    if (read_wifi_creds(ssid, sizeof(ssid), psk, sizeof(psk))) {
        wifi_config_t wifi_config = {0};
        snprintf((char *)wifi_config.sta.ssid,
                 sizeof(wifi_config.sta.ssid), "%s", ssid);
        snprintf((char *)wifi_config.sta.password,
                 sizeof(wifi_config.sta.password), "%s", psk);
        wifi_config.sta.threshold.authmode =
            psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        (void)esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "link reset: esp_wifi_start failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
        s_wifi_retries = 0;
        s_wifi_auto_connect_enabled = true;
        (void)esp_wifi_connect();
        ESP_LOGI(TAG, "SDIO/WiFi link reset OK, reconnecting to '%s'", ssid);
    } else {
        ESP_LOGW(TAG, "link reset OK but no WiFi credentials stored");
    }

    return ESP_OK;
#endif
}

void services_https_lock(void)
{
    if (!s_https_mtx) s_https_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_https_mtx, portMAX_DELAY);
}

void services_https_unlock(void)
{
    if (s_https_mtx) xSemaphoreGive(s_https_mtx);
}

void services_network_bulk_begin(void)
{
    portENTER_CRITICAL(&s_bulk_lock);
    s_bulk_network_users++;
    portEXIT_CRITICAL(&s_bulk_lock);
}

void services_network_bulk_end(void)
{
    portENTER_CRITICAL(&s_bulk_lock);
    if (s_bulk_network_users > 0) s_bulk_network_users--;
    portEXIT_CRITICAL(&s_bulk_lock);
}

bool services_network_bulk_active(void)
{
    portENTER_CRITICAL(&s_bulk_lock);
    bool active = s_bulk_network_users > 0;
    portEXIT_CRITICAL(&s_bulk_lock);
    return active;
}

esp_err_t wifi_scan_networks(wifi_scan_result_t *results, size_t max_results, size_t *result_count)
{
    if (result_count) *result_count = 0;
    if (!results || max_results == 0 || !result_count) return ESP_ERR_INVALID_ARG;

#if WALLDISPLAY_HAS_WIFI_DRIVER
    ESP_LOGI(TAG, "Wi-Fi scan requested");
    esp_err_t err = wifi_driver_prepare(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan prepare failed: %s", esp_err_to_name(err));
        return err;
    }

    bool previous_auto_connect = s_wifi_auto_connect_enabled;
    bool was_connected = status_wifi_is_connected();
    s_wifi_auto_connect_enabled = false;
    status_note_wifi_detail("Scanning", 0);
    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            s_wifi_auto_connect_enabled = previous_auto_connect;
            ESP_LOGE(TAG, "Wi-Fi start for scan failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
    } else if (!was_connected) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {.min = 80, .max = 240},
            .passive = 360,
        },
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        s_wifi_auto_connect_enabled = previous_auto_connect;
        if (previous_auto_connect && !status_wifi_is_connected()) (void)esp_wifi_connect();
        status_note_wifi_detail(status_wifi_is_connected() ? "Connected" : "Scan failed", 0);
        ESP_LOGE(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK || ap_count == 0) {
        s_wifi_auto_connect_enabled = previous_auto_connect;
        if (previous_auto_connect && !status_wifi_is_connected()) (void)esp_wifi_connect();
        status_note_wifi_detail((was_connected || status_wifi_is_connected()) ? "Connected" : "Scan complete", 0);
        ESP_LOGI(TAG, "Wi-Fi scan completed with %u APs (%s)", ap_count, esp_err_to_name(err));
        return err;
    }

    uint16_t fetch_count = ap_count > 32 ? 32 : ap_count;
    wifi_ap_record_t *records = calloc(fetch_count, sizeof(*records));
    if (!records) {
        s_wifi_auto_connect_enabled = previous_auto_connect;
        if (previous_auto_connect && !status_wifi_is_connected()) (void)esp_wifi_connect();
        status_note_wifi_detail((was_connected || status_wifi_is_connected()) ? "Connected" : "Scan failed", 0);
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&fetch_count, records);
    if (err == ESP_OK) {
        size_t written = 0;
        for (uint16_t i = 0; i < fetch_count && written < max_results; i++) {
            if (!records[i].ssid[0]) continue;

            bool duplicate = false;
            for (size_t j = 0; j < written; j++) {
                if (strncmp(results[j].ssid, (const char *)records[i].ssid, sizeof(results[j].ssid)) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            snprintf(results[written].ssid, sizeof(results[written].ssid), "%s", (const char *)records[i].ssid);
            results[written].rssi = records[i].rssi;
            results[written].channel = records[i].primary;
            results[written].secure = records[i].authmode != WIFI_AUTH_OPEN;
            written++;
        }
        *result_count = written;
        ESP_LOGI(TAG, "Wi-Fi scan found %u AP records, showing %u SSIDs", ap_count, (unsigned)written);
    }

    free(records);
    s_wifi_auto_connect_enabled = previous_auto_connect;
    if (previous_auto_connect && !status_wifi_is_connected()) {
        status_note_wifi_detail("Reconnecting", 0);
        (void)esp_wifi_connect();
    } else if (was_connected || status_wifi_is_connected()) {
        status_note_wifi_detail("Connected", 0);
    } else {
        status_note_wifi_detail("Scan complete", 0);
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* --------------------------------------------------------------------------
 * RS-485 / WLED command transport
 * -------------------------------------------------------------------------- */

bool cmd_tx_is_ready(void)
{
    return s_cmd_q != NULL;
}

esp_err_t cmd_tx_send_json(const char *json)
{
    if (!json || !s_cmd_q) return ESP_ERR_INVALID_STATE;

    size_t len = strlen(json);
    while (len && (json[len - 1] == '\n' || json[len - 1] == '\r')) len--;
    if (len == 0 || len >= CMD_JSON_MAX) return ESP_ERR_INVALID_ARG;

    cmd_msg_t msg = {0};
    memcpy(msg.text, json, len);
    msg.text[len] = '\0';

    if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE) {
        cmd_msg_t dropped;
        (void)xQueueReceive(s_cmd_q, &dropped, 0);
        status_note_rs485_drop();
        if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void cmd_tx_drain_pending(void)
{
    if (!s_cmd_q) return;
    cmd_msg_t dropped;
    while (xQueueReceive(s_cmd_q, &dropped, 0) == pdTRUE) {
        status_note_rs485_drop();
    }
}

static uint8_t pct_to_wled_bri(uint8_t pct);
static esp_err_t publish_light_fields_to_wled(const led_state_t *state);

static esp_err_t send_power_preset(uint16_t preset_id)
{
    char json[32];
    snprintf(json, sizeof(json), "{\"ps\":%u}", (unsigned)preset_id);
    return cmd_tx_send_json(json);
}

static esp_err_t send_power_wake_black(void)
{
    return cmd_tx_send_json("{\"on\":true,\"bri\":1,\"transition\":0,\"seg\":{\"col\":[[0,0,0,0],[0,0,0,0],[0,0,0,0]]}}");
}

static void delayed_power_cmd_task(void *arg)
{
    delayed_power_cmd_t *cmd = (delayed_power_cmd_t *)arg;
    if (!cmd) {
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    if (cmd->seq == s_power_request_seq) {
        esp_err_t err = cmd->use_preset
            ? send_power_preset(cmd->preset_id)
            : publish_light_fields_to_wled(&cmd->state);
        if (err == ESP_OK) note_wled_local_echo_hold();
    }

    free(cmd);
    vTaskDelete(NULL);
}

static esp_err_t schedule_delayed_power_cmd(uint32_t delay_ms, bool use_preset,
                                            uint16_t preset_id, const led_state_t *state,
                                            uint32_t seq)
{
    delayed_power_cmd_t *cmd = calloc(1, sizeof(*cmd));
    if (!cmd) return ESP_ERR_NO_MEM;
    cmd->seq = seq;
    cmd->delay_ms = delay_ms;
    cmd->use_preset = use_preset;
    cmd->preset_id = preset_id;
    if (state) cmd->state = *state;

    BaseType_t ok = xTaskCreate(delayed_power_cmd_task, "pwr_delay", 3072, cmd, 5, NULL);
    if (ok != pdPASS) {
        free(cmd);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t services_request_light_power(bool on, bool *used_preset)
{
    if (used_preset) *used_preset = false;
    uint32_t seq = ++s_power_request_seq;

    app_tuning_config_t cfg;
    if (app_config_tuning_load(&cfg) != ESP_OK) app_config_tuning_defaults(&cfg);
    uint16_t preset_id = on ? cfg.power_on_preset_id : cfg.power_off_preset_id;
    bool use_preset = cfg.power_preset_mode_enabled && preset_id > 0;

    led_state_t current;
    led_state_get(&current);
    bool turning_on = on && !current.power;

    if (turning_on && cfg.power_on_delay_ms > 0) {
        cmd_tx_drain_pending();
        esp_err_t err = send_power_wake_black();
        if (err == ESP_OK) note_wled_local_echo_hold_for((uint32_t)cfg.power_on_delay_ms + WLED_LOCAL_ECHO_HOLD_MS);

        s_suppress_next_led_publish = true;
        led_state_set_power(true);
        s_suppress_next_led_publish = false;

        led_state_t delayed_state;
        led_state_get(&delayed_state);
        esp_err_t sched_err = schedule_delayed_power_cmd(cfg.power_on_delay_ms, use_preset,
                                                         preset_id, &delayed_state, seq);
        if (sched_err != ESP_OK) {
            sched_err = use_preset ? send_power_preset(preset_id) : publish_light_fields_to_wled(&delayed_state);
        }
        if (used_preset) *used_preset = use_preset;
        return err == ESP_OK ? sched_err : err;
    }

    esp_err_t err = ESP_OK;
    if (use_preset) {
        cmd_tx_drain_pending();
        err = send_power_preset(preset_id);
        if (used_preset) *used_preset = true;
    }

    if (err == ESP_OK && use_preset) {
        s_suppress_next_led_publish = true;
    }
    led_state_set_power(on);
    s_suppress_next_led_publish = false;
    return err;
}

static uint8_t pct_to_wled_bri(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return (uint8_t)(((uint32_t)pct * 255u + 50u) / 100u);
}

static void hue_to_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint16_t region = hue / 43;
    uint16_t rem = (hue - (region * 43)) * 6;
    uint8_t q = (uint8_t)(255 - rem);
    uint8_t t = (uint8_t)rem;

    switch (region) {
    default:
    case 0: *r = 255; *g = t;   *b = 0;   break;
    case 1: *r = q;   *g = 255; *b = 0;   break;
    case 2: *r = 0;   *g = 255; *b = t;   break;
    case 3: *r = 0;   *g = q;   *b = 255; break;
    case 4: *r = t;   *g = 0;   *b = 255; break;
    case 5: *r = 255; *g = 0;   *b = q;   break;
    }
}

static uint8_t kelvin_to_wled_cct(const led_state_t *state)
{
    uint16_t min_k = state->kelvin_min;
    uint16_t max_k = state->kelvin_max;
    if (max_k <= min_k) return 127;
    uint16_t k = state->kelvin;
    if (k < min_k) k = min_k;
    if (k > max_k) k = max_k;
    uint32_t span = (uint32_t)max_k - min_k;
    return (uint8_t)((((uint32_t)k - min_k) * 255u + span / 2u) / span);
}

static bool led_light_fields_match(const led_state_t *a, const led_state_t *b)
{
    return a->power == b->power &&
           a->brightness_pct == b->brightness_pct &&
           a->kelvin == b->kelvin &&
           a->kelvin_min == b->kelvin_min &&
           a->kelvin_max == b->kelvin_max;
}

static bool led_hue_fields_match(const led_state_t *a, const led_state_t *b)
{
    return a->primary_hue == b->primary_hue &&
           a->secondary_hue == b->secondary_hue;
}

static esp_err_t publish_light_fields_to_wled(const led_state_t *state)
{
    char json[128];
    snprintf(json, sizeof(json),
             "{\"on\":%s,\"bri\":%u,\"transition\":7,\"seg\":{\"cct\":%u}}",
             state->power ? "true" : "false",
             pct_to_wled_bri(state->brightness_pct),
             kelvin_to_wled_cct(state));
    return cmd_tx_send_json(json);
}

static esp_err_t publish_hue_fields_to_wled(const led_state_t *state)
{
    uint8_t primary[3] = {0};
    uint8_t secondary[3] = {0};
    hue_to_rgb(state->primary_hue, &primary[0], &primary[1], &primary[2]);
    hue_to_rgb(state->secondary_hue, &secondary[0], &secondary[1], &secondary[2]);

    char json[112];
    snprintf(json, sizeof(json),
             "{\"seg\":{\"col\":[[%u,%u,%u],[%u,%u,%u]]}}",
             primary[0], primary[1], primary[2],
             secondary[0], secondary[1], secondary[2]);
    return cmd_tx_send_json(json);
}

static void publish_led_state_to_wled(const led_state_t *state, void *user)
{
    (void)user;
    static bool have_last;
    static led_state_t last;

    if (!state) return;

    bool light_changed = !have_last || !led_light_fields_match(state, &last);
    bool hue_changed = !have_last || !led_hue_fields_match(state, &last);
    if (!light_changed && !hue_changed) return;

    if (s_suppress_next_led_publish) {
        last = *state;
        have_last = true;
        note_wled_local_echo_hold();
        return;
    }

    esp_err_t light_err = light_changed ? publish_light_fields_to_wled(state) : ESP_OK;
    esp_err_t hue_err = hue_changed ? publish_hue_fields_to_wled(state) : ESP_OK;
    if (light_err == ESP_OK && hue_err == ESP_OK) {
        last = *state;
        have_last = true;
        note_wled_local_echo_hold();
    }
}

static void cmd_worker(void *arg)
{
    (void)arg;
    cmd_msg_t msg;
    while (xQueueReceive(s_cmd_q, &msg, portMAX_DELAY) == pdTRUE) {
        size_t len = strlen(msg.text);
        if (len == 0) continue;
        int written = uart_write_bytes(BSP_RS485_UART_NUM, msg.text, len);
        uart_write_bytes(BSP_RS485_UART_NUM, "\n", 1);
        uart_wait_tx_done(BSP_RS485_UART_NUM, pdMS_TO_TICKS(250));
        if (written == (int)len) status_note_rs485_tx();
        vTaskDelay(pdMS_TO_TICKS(CMD_TX_REPLY_GUARD_MS));
    }
}

static void handle_rs485_line(const char *line)
{
    if (!line || !line[0]) return;
    status_note_wled_rx();
    ESP_LOGD(TAG, "RS-485 RX: %s", line);
    if (line[0] == '{') {
        wled_state_parse_json(line, strlen(line));
    }
}

static void rs485_rx_worker(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    char *line = heap_caps_malloc(RS485_RX_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) line = heap_caps_malloc(RS485_RX_LINE_MAX, MALLOC_CAP_8BIT);
    if (!line) {
        status_note_rs485_overflow();
        vTaskDelete(NULL);
        return;
    }
    size_t pos = 0;

    while (1) {
        int n = uart_read_bytes(BSP_RS485_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(250));
        if (n > 0) taskYIELD();
        for (int i = 0; i < n; i++) {
            char ch = (char)buf[i];
            if (ch == '\n' || ch == '\r') {
                if (pos) {
                    line[pos] = '\0';
                    handle_rs485_line(line);
                    pos = 0;
                }
            } else if (pos < RS485_RX_LINE_MAX - 1) {
                line[pos++] = ch;
            } else {
                pos = 0;
                status_note_rs485_overflow();
            }
        }
    }
}

esp_err_t cmd_tx_init(void)
{
    if (s_cmd_q) return ESP_OK;

    ESP_RETURN_ON_ERROR(board_power_enable_vo4_3v3(), TAG, "RS-485 VO4 power enable failed");

    uart_config_t cfg = {
        .baud_rate = BSP_RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(BSP_RS485_UART_NUM, &cfg), TAG, "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(BSP_RS485_UART_NUM, BSP_RS485_TX, BSP_RS485_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pin config failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(BSP_RS485_UART_NUM, RS485_UART_BUF_SIZE, RS485_UART_BUF_SIZE, 0, NULL, 0),
                        TAG, "uart driver install failed");

    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    if (!s_cmd_q) return ESP_ERR_NO_MEM;

    if (xTaskCreate(cmd_worker, "cmd_tx", CMD_TX_TASK_STACK_BYTES, NULL, 6, &s_cmd_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rs485_rx_worker, "rs485_rx", RS485_RX_TASK_STACK_BYTES, NULL, 5, &s_rx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    status_note_rs485(true);

    led_state_subscribe(publish_led_state_to_wled, NULL);
    led_state_t initial;
    led_state_get(&initial);
    publish_led_state_to_wled(&initial, NULL);

    ESP_LOGI(TAG, "RS-485 command transport ready on UART%d @ %d", BSP_RS485_UART_NUM, BSP_RS485_BAUD);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Provisioning + link health
 * -------------------------------------------------------------------------- */

static void json_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    size_t out = 0;
    for (size_t i = 0; src && src[i] && out + 1 < dst_len; i++) {
        char ch = src[i];
        if ((ch == '\"' || ch == '\\') && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = ch;
        } else if ((unsigned char)ch >= 0x20) {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

static bool wled_status_seen_within(uint32_t max_age_ms)
{
    services_status_t st;
    services_status_get(&st);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return st.wled_online && st.wled_last_rx_ms &&
           (now_ms - st.wled_last_rx_ms) < max_age_ms;
}

static bool wled_wait_recently_seen(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (wled_status_seen_within(30000u)) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

static bool provision_wait(uint32_t wait_ms)
{
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms)) > 0;
}

static void provision_worker(void *arg)
{
    (void)arg;

    char ssid[33];
    char psk[65];
    char ssid_json[70];
    char psk_json[140];
    char json[360];
    char audio_json[220];
    uint32_t retry_ms = WLED_PROVISION_INITIAL_RETRY_MS;
    bool force = false;

    while (1) {
        if (s_provision_force_requested) {
            s_provision_force_requested = false;
            force = true;
            retry_ms = WLED_PROVISION_INITIAL_RETRY_MS;
        }

        if (services_network_bulk_active()) {
            if (provision_wait(1000)) force = true;
            continue;
        }

        if (!cmd_tx_is_ready()) {
            if (provision_wait(1000)) force = true;
            continue;
        }

        if (!read_wifi_creds(ssid, sizeof(ssid), psk, sizeof(psk))) {
            ESP_LOGW(TAG, "Provisioning skipped; no wifi_creds.ssid in NVS");
            s_provision_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        json_escape(ssid, ssid_json, sizeof(ssid_json));
        json_escape(psk, psk_json, sizeof(psk_json));
        snprintf(json, sizeof(json),
                 "{\"nw\":{\"ins\":[{\"ssid\":\"%s\",\"psk\":\"%s\"}]},"
                 "\"id\":{\"mdns\":\"wled-86box\",\"name\":\"86Box LED\"},"
                  "\"if\":{\"sync\":{\"recv\":true,\"port\":%u,\"group\":%u}}}",
                  ssid_json, psk_json, (unsigned)SOUND_SYNC_PORT, (unsigned)SOUND_SYNC_GROUP);
           snprintf(audio_json, sizeof(audio_json),
                  "{\"um\":{\"AudioReactive\":{\"on\":true,\"digitalmic\":{\"type\":254,\"pin\":[-1,-1,-1,-1]},\"sync\":{\"port\":%u,\"mode\":2}}}}",
                  (unsigned)SOUND_SYNC_PORT);

            ESP_LOGI(TAG, "%s WLED provisioning config for SSID '%s'",
                 force ? "Force-sending" : "Sending", ssid);
        (void)cmd_tx_send_json(json);
           (void)cmd_tx_send_json(audio_json);
        (void)cmd_tx_send_json("{\"v\":true}");

        if (wled_wait_recently_seen(8000)) {
            ESP_LOGI(TAG, "WLED responded after provisioning");
            s_provision_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "No WLED response after provisioning; retry in %us", (unsigned)(retry_ms / 1000u));
        if (provision_wait(retry_ms)) {
            force = true;
            retry_ms = WLED_PROVISION_INITIAL_RETRY_MS;
            continue;
        }
        if (retry_ms < WLED_PROVISION_MAX_RETRY_MS) {
            retry_ms *= 2u;
            if (retry_ms > WLED_PROVISION_MAX_RETRY_MS) retry_ms = WLED_PROVISION_MAX_RETRY_MS;
        }
    }
}

static esp_err_t provision_start(bool force)
{
    if (force) s_provision_force_requested = true;
    if (s_provision_task) {
        if (force) xTaskNotifyGive(s_provision_task);
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreateWithCaps(provision_worker, "wled_provision", 6144,
                                        NULL, 4, &s_provision_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(provision_worker, "wled_provision", 6144,
                                       NULL, 4, &s_provision_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t provision_init(void)
{
    return provision_start(false);
}

esp_err_t provision_wifi_update(void)
{
    return provision_start(true);
}

static void link_health_worker(void *arg)
{
    (void)arg;
    while (1) {
        app_tuning_config_t tuning;
        if (app_config_tuning_load(&tuning) != ESP_OK) app_config_tuning_defaults(&tuning);

        if (services_network_bulk_active()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (cmd_tx_is_ready()) (void)cmd_tx_send_json("{\"v\":true}");

        services_status_t st;
        services_status_get(&st);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (st.wled_online && st.wled_last_rx_ms &&
            now_ms - st.wled_last_rx_ms > (uint32_t)tuning.wled_stale_s * 1000u) {
            status_note_wled_online(false);
        }
        uint32_t poll_ms = (uint32_t)tuning.wled_poll_s * 1000u;
        if (poll_ms < 1000u) poll_ms = 1000u;
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

static void wled_state_reconcile(const wled_state_t *ws, void *user)
{
    (void)user;
    if (!ws || !ws->valid) return;

    led_state_t current;
    led_state_get(&current);

    bool power = ws->on;
    uint8_t bri_pct = (uint8_t)(((uint32_t)ws->bri * 100u + 127u) / 255u);

    if (wled_local_echo_hold_active() &&
        (current.power != power || current.brightness_pct != bri_pct)) {
        ESP_LOGD(TAG, "ignoring stale WLED echo during local control window");
        return;
    }

    if (current.power != power) {
        if (!power && led_state_power_on_hold_active()) {
            ESP_LOGD(TAG, "holding idle wake lights on despite WLED off snapshot");
        } else {
            led_state_set_power(power);
        }
    }
    if (current.brightness_pct != bri_pct) led_state_set_brightness(bri_pct);
}

esp_err_t link_health_init(void)
{
    if (s_link_task) return ESP_OK;

    wled_state_subscribe(wled_state_reconcile, NULL);

    BaseType_t ok = xTaskCreateWithCaps(link_health_worker, "wled_link", 4096,
                                        NULL, 4, &s_link_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(link_health_worker, "wled_link", 4096,
                                       NULL, 4, &s_link_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* --------------------------------------------------------------------------
 * Audio pipeline (delegates to audio_in / audio_fft / sound_sync_tx modules)
 * -------------------------------------------------------------------------- */

#include "audio_in.h"
#include "audio_fft.h"
#include "sound_sync_tx.h"

esp_err_t audio_in_init(void)
{
    esp_err_t err = audio_in_start();
    if (err == ESP_OK) {
        status_note_audio(true, false, false);
    } else {
        status_note_audio(false, false, false);
        ESP_LOGW(TAG, "Audio input init: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t audio_fft_init(void)
{
    if (!audio_in_is_ready()) {
        ESP_LOGW(TAG, "Audio FFT deferred: audio input not ready");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = audio_fft_start();
    if (err == ESP_OK) {
        status_note_audio(true, true, false);
    } else {
        ESP_LOGW(TAG, "Audio FFT init: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sound_sync_tx_init(void)
{
    if (!audio_fft_is_ready()) {
        ESP_LOGW(TAG, "Sound Sync TX deferred: FFT not ready");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sound_sync_tx_start();
    if (err == ESP_OK) {
        status_note_audio(true, true, true);
    } else {
        ESP_LOGW(TAG, "Sound Sync TX init: %s", esp_err_to_name(err));
    }
    return err;
}
