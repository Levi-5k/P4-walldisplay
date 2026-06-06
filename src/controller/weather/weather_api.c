#include "weather_api.h"

#include "app_config.h"
#include "sd_storage.h"
#include "services.h"
#include "weather_history.h"
#include "weather_state.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "weather_api";

#ifndef WALLDISPLAY_WEATHER_API_KEY
#define WALLDISPLAY_WEATHER_API_KEY ""
#endif

#define WEATHER_HISTORY_SAVE_DELAY_MS (10u * 1000u)
#define WEATHER_TASK_STACK_BYTES      (12u * 1024u)
#define WEATHER_ENABLE_FORECAST       1

typedef struct {
    char api_key[96];
    int32_t lat_x1e6;
    int32_t lon_x1e6;
    bool has_cached_location;
} weather_config_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_body_t;

static TaskHandle_t s_weather_task;

static void weather_load_tuning(app_tuning_config_t *out)
{
    if (app_config_tuning_load(out) != ESP_OK) app_config_tuning_defaults(out);
}

static void format_coord(char *out, size_t out_len, int32_t value)
{
    bool neg = value < 0;
    int32_t abs_value = neg ? -value : value;
    snprintf(out, out_len, "%s%ld.%06ld", neg ? "-" : "",
             (long)(abs_value / 1000000), (long)(abs_value % 1000000));
}

static bool weather_location_valid(int32_t lat_x1e6, int32_t lon_x1e6)
{
    return (lat_x1e6 || lon_x1e6) &&
           lat_x1e6 >= -90000000 && lat_x1e6 <= 90000000 &&
           lon_x1e6 >= -180000000 && lon_x1e6 <= 180000000;
}

static bool read_config(weather_config_t *cfg)
{
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));

    app_weather_config_t saved;
    (void)app_config_weather_load(&saved);
    if (weather_location_valid(saved.lat_x1e6, saved.lon_x1e6)) {
        cfg->lat_x1e6 = saved.lat_x1e6;
        cfg->lon_x1e6 = saved.lon_x1e6;
        cfg->has_cached_location = true;
    }

    if (saved.configured) {
        snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", saved.api_key);
        return true;
    }

    if (WALLDISPLAY_WEATHER_API_KEY[0]) {
        snprintf(cfg->api_key, sizeof(cfg->api_key), "%s", WALLDISPLAY_WEATHER_API_KEY);
        return true;
    }

    return false;
}

static void weather_wait_ms(uint32_t delay_ms)
{
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
}

#define WEATHER_BODY_MAX (16u * 1024u)

static esp_err_t append_body(http_body_t *body, const char *data, size_t len)
{
    if (!body || !data || len == 0) return ESP_OK;
    if (body->len + len + 1 > body->cap) {
        size_t new_cap = body->cap ? body->cap * 2 : 4096;
        while (new_cap < body->len + len + 1) new_cap *= 2;
        if (new_cap > WEATHER_BODY_MAX) return ESP_ERR_NO_MEM;
        /* Prefer PSRAM so HTTP bodies do not pressure internal heap. */
        char *next = heap_caps_realloc(body->data, new_cap,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) next = realloc(body->data, new_cap);
        if (!next) return ESP_ERR_NO_MEM;
        body->data = next;
        body->cap = new_cap;
    }
    memcpy(body->data + body->len, data, len);
    body->len += len;
    body->data[body->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data && evt->data_len > 0) {
        return append_body((http_body_t *)evt->user_data, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static int32_t round_double_to_i32(double value)
{
    return (int32_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

static uint16_t clamp_u16(double value)
{
    if (value < 0) return 0;
    if (value > 65535.0) return 65535;
    return (uint16_t)(value + 0.5);
}

static void copy_string_field(char *dst, size_t dst_len, const cJSON *node)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (cJSON_IsString(node) && node->valuestring) {
        snprintf(dst, dst_len, "%s", node->valuestring);
    }
}

static esp_err_t parse_current(const char *json, size_t len, weather_state_t *state)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *main_obj    = cJSON_GetObjectItem(root, "main");
    cJSON *weather_arr = cJSON_GetObjectItem(root, "weather");
    cJSON *wind_obj    = cJSON_GetObjectItem(root, "wind");
    cJSON *clouds_obj  = cJSON_GetObjectItem(root, "clouds");
    cJSON *rain_obj    = cJSON_GetObjectItem(root, "rain");
    cJSON *snow_obj    = cJSON_GetObjectItem(root, "snow");
    cJSON *sys_obj     = cJSON_GetObjectItem(root, "sys");
    cJSON *temp        = cJSON_GetObjectItem(main_obj, "temp");
    cJSON *feels       = cJSON_GetObjectItem(main_obj, "feels_like");
    cJSON *hum         = cJSON_GetObjectItem(main_obj, "humidity");

    if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(feels) || !cJSON_IsNumber(hum)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    state->temp_f  = round_double_to_i32(cJSON_GetNumberValue(temp));
    state->feels_f = round_double_to_i32(cJSON_GetNumberValue(feels));

    double humidity = cJSON_GetNumberValue(hum);
    if (humidity < 0) humidity = 0;
    if (humidity > 100) humidity = 100;
    state->humidity_pct = (uint8_t)round_double_to_i32(humidity);

    cJSON *tmin = cJSON_GetObjectItem(main_obj, "temp_min");
    cJSON *tmax = cJSON_GetObjectItem(main_obj, "temp_max");
    if (cJSON_IsNumber(tmin)) state->temp_min_f = round_double_to_i32(cJSON_GetNumberValue(tmin));
    if (cJSON_IsNumber(tmax)) state->temp_max_f = round_double_to_i32(cJSON_GetNumberValue(tmax));

    cJSON *pressure = cJSON_GetObjectItem(main_obj, "pressure");
    if (cJSON_IsNumber(pressure)) state->pressure_hpa = clamp_u16(cJSON_GetNumberValue(pressure));
    cJSON *sea_level = cJSON_GetObjectItem(main_obj, "sea_level");
    if (cJSON_IsNumber(sea_level)) state->sea_level_hpa = clamp_u16(cJSON_GetNumberValue(sea_level));
    cJSON *grnd_level = cJSON_GetObjectItem(main_obj, "grnd_level");
    if (cJSON_IsNumber(grnd_level)) state->grnd_level_hpa = clamp_u16(cJSON_GetNumberValue(grnd_level));

    cJSON *wind_speed = cJSON_GetObjectItem(wind_obj, "speed");
    cJSON *wind_gust  = cJSON_GetObjectItem(wind_obj, "gust");
    cJSON *wind_deg   = cJSON_GetObjectItem(wind_obj, "deg");
    if (cJSON_IsNumber(wind_speed)) {
        double mph = cJSON_GetNumberValue(wind_speed);
        if (mph < 0) mph = 0;
        if (mph > 200) mph = 200;
        state->wind_mph_x10 = (uint16_t)round_double_to_i32(mph * 10.0);
    }
    if (cJSON_IsNumber(wind_gust)) {
        double mph = cJSON_GetNumberValue(wind_gust);
        if (mph < 0) mph = 0;
        if (mph > 250) mph = 250;
        state->wind_gust_mph_x10 = (uint16_t)round_double_to_i32(mph * 10.0);
        state->wind_gust_valid = true;
    }
    if (cJSON_IsNumber(wind_deg)) {
        int deg = round_double_to_i32(cJSON_GetNumberValue(wind_deg));
        deg %= 360;
        if (deg < 0) deg += 360;
        state->wind_deg = (uint16_t)deg;
    }

    cJSON *clouds_all = cJSON_GetObjectItem(clouds_obj, "all");
    if (cJSON_IsNumber(clouds_all)) {
        double v = cJSON_GetNumberValue(clouds_all);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        state->clouds_pct = (uint8_t)round_double_to_i32(v);
    }

    cJSON *vis = cJSON_GetObjectItem(root, "visibility");
    if (cJSON_IsNumber(vis)) state->visibility_m = clamp_u16(cJSON_GetNumberValue(vis));

    cJSON *rain_1h = cJSON_GetObjectItem(rain_obj, "1h");
    if (cJSON_IsNumber(rain_1h)) state->rain_1h_mm_x10 = clamp_u16(cJSON_GetNumberValue(rain_1h) * 10.0);
    cJSON *snow_1h = cJSON_GetObjectItem(snow_obj, "1h");
    if (cJSON_IsNumber(snow_1h)) state->snow_1h_mm_x10 = clamp_u16(cJSON_GetNumberValue(snow_1h) * 10.0);

    cJSON *first_weather = cJSON_IsArray(weather_arr) ? cJSON_GetArrayItem(weather_arr, 0) : NULL;
    cJSON *weather_id    = first_weather ? cJSON_GetObjectItem(first_weather, "id") : NULL;
    cJSON *condition    = first_weather ? cJSON_GetObjectItem(first_weather, "main") : NULL;
    cJSON *description  = first_weather ? cJSON_GetObjectItem(first_weather, "description") : NULL;
    cJSON *icon         = first_weather ? cJSON_GetObjectItem(first_weather, "icon") : NULL;
    const char *cond_text = cJSON_IsString(condition) ? condition->valuestring :
                            cJSON_IsString(description) ? description->valuestring : "Weather";
    snprintf(state->condition, sizeof(state->condition), "%s", cond_text);
    if (cJSON_IsNumber(weather_id)) state->weather_id = clamp_u16(cJSON_GetNumberValue(weather_id));
    copy_string_field(state->description, sizeof(state->description), description);
    copy_string_field(state->icon, sizeof(state->icon), icon);

    cJSON *city_name = cJSON_GetObjectItem(root, "name");
    copy_string_field(state->city, sizeof(state->city), city_name);
    cJSON *country = cJSON_GetObjectItem(sys_obj, "country");
    copy_string_field(state->country, sizeof(state->country), country);

    cJSON *dt = cJSON_GetObjectItem(root, "dt");
    if (cJSON_IsNumber(dt)) state->observed_utc = (uint32_t)cJSON_GetNumberValue(dt);
    cJSON *sunrise = cJSON_GetObjectItem(sys_obj, "sunrise");
    cJSON *sunset  = cJSON_GetObjectItem(sys_obj, "sunset");
    if (cJSON_IsNumber(sunrise)) state->sunrise_utc = (uint32_t)cJSON_GetNumberValue(sunrise);
    if (cJSON_IsNumber(sunset))  state->sunset_utc  = (uint32_t)cJSON_GetNumberValue(sunset);

    cJSON *tz = cJSON_GetObjectItem(root, "timezone");
    if (cJSON_IsNumber(tz)) state->tz_offset_s = (int32_t)cJSON_GetNumberValue(tz);

    state->valid = true;
    cJSON_Delete(root);
    return ESP_OK;
}

#if WEATHER_ENABLE_FORECAST
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool parse_iso_date_noon_utc(const char *text, int32_t tz_offset_s, uint32_t *out)
{
    if (!text || !out) return false;
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (sscanf(text, "%d-%u-%u", &year, &month, &day) != 3) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;

    int64_t seconds = days_from_civil(year, month, day) * 86400 + 12 * 3600 - tz_offset_s;
    if (seconds <= 0 || seconds > UINT32_MAX) return false;
    *out = (uint32_t)seconds;
    return true;
}

static bool parse_iso_hour_utc(const char *text, int32_t tz_offset_s, uint32_t *out)
{
    if (!text || !out) return false;
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    if (sscanf(text, "%d-%u-%uT%u:%u", &year, &month, &day, &hour, &minute) != 5) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return false;

    int64_t seconds = days_from_civil(year, month, day) * 86400 +
                      (int64_t)hour * 3600 + (int64_t)minute * 60 - tz_offset_s;
    if (seconds <= 0 || seconds > UINT32_MAX) return false;
    *out = (uint32_t)seconds;
    return true;
}

static void open_meteo_weather_code(int code, char *condition, size_t condition_len,
                                    char *icon, size_t icon_len)
{
    const char *cond = "Clouds";
    const char *owm_icon = "03d";

    if (code == 0) {
        cond = "Clear";
        owm_icon = "01d";
    } else if (code == 1 || code == 2) {
        cond = "Clouds";
        owm_icon = "02d";
    } else if (code == 3) {
        cond = "Clouds";
        owm_icon = "04d";
    } else if (code == 45 || code == 48) {
        cond = "Fog";
        owm_icon = "50d";
    } else if ((code >= 51 && code <= 57)) {
        cond = "Drizzle";
        owm_icon = "09d";
    } else if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) {
        cond = "Rain";
        owm_icon = "10d";
    } else if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
        cond = "Snow";
        owm_icon = "13d";
    } else if (code >= 95 && code <= 99) {
        cond = "Thunderstorm";
        owm_icon = "11d";
    }

    snprintf(condition, condition_len, "%s", cond);
    snprintf(icon, icon_len, "%s", owm_icon);
}

static esp_err_t parse_forecast(const char *json, size_t len, weather_state_t *state)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    int32_t tz = state->tz_offset_s;
    cJSON *utc_offset = cJSON_GetObjectItem(root, "utc_offset_seconds");
    if (cJSON_IsNumber(utc_offset)) {
        tz = (int32_t)cJSON_GetNumberValue(utc_offset);
        state->tz_offset_s = tz;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    cJSON *current_gust = cJSON_GetObjectItem(current, "wind_gusts_10m");
    if (cJSON_IsNumber(current_gust)) {
        double mph = cJSON_GetNumberValue(current_gust);
        if (mph < 0) mph = 0;
        if (mph > 250) mph = 250;
        state->wind_gust_mph_x10 = (uint16_t)round_double_to_i32(mph * 10.0);
        state->wind_gust_valid = true;
    }

    cJSON *current_uv = cJSON_GetObjectItem(current, "uv_index");
    if (cJSON_IsNumber(current_uv)) {
        int uv = round_double_to_i32(cJSON_GetNumberValue(current_uv));
        if (uv < 0) uv = 0;
        if (uv > UINT8_MAX) uv = UINT8_MAX;
        state->uv_index = (uint8_t)uv;
        state->uv_index_valid = true;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    cJSON *times = cJSON_GetObjectItem(daily, "time");
    cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
    cJSON *hi = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *lo = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *pop = cJSON_GetObjectItem(daily, "precipitation_probability_max");
    cJSON *uv_max = cJSON_GetObjectItem(daily, "uv_index_max");
    if (!cJSON_IsArray(times) || !cJSON_IsArray(codes) || !cJSON_IsArray(hi) || !cJSON_IsArray(lo)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!state->uv_index_valid && cJSON_IsArray(uv_max)) {
        cJSON *today_uv = cJSON_GetArrayItem(uv_max, 0);
        if (cJSON_IsNumber(today_uv)) {
            int uv = round_double_to_i32(cJSON_GetNumberValue(today_uv));
            if (uv < 0) uv = 0;
            if (uv > UINT8_MAX) uv = UINT8_MAX;
            state->uv_index = (uint8_t)uv;
            state->uv_index_valid = true;
        }
    }

    memset(state->days, 0, sizeof(state->days));
    state->day_count = 0;
    memset(state->hours, 0, sizeof(state->hours));
    state->hour_count = 0;

    int count = cJSON_GetArraySize(times);
    if (count > cJSON_GetArraySize(codes)) count = cJSON_GetArraySize(codes);
    if (count > cJSON_GetArraySize(hi)) count = cJSON_GetArraySize(hi);
    if (count > cJSON_GetArraySize(lo)) count = cJSON_GetArraySize(lo);
    if (cJSON_IsArray(pop) && count > cJSON_GetArraySize(pop)) count = cJSON_GetArraySize(pop);
    if (count > WEATHER_FORECAST_DAYS) count = WEATHER_FORECAST_DAYS;

    for (int i = 0; i < count; i++) {
        cJSON *date = cJSON_GetArrayItem(times, i);
        cJSON *code = cJSON_GetArrayItem(codes, i);
        cJSON *high = cJSON_GetArrayItem(hi, i);
        cJSON *low = cJSON_GetArrayItem(lo, i);
        if (!cJSON_IsString(date) || !cJSON_IsNumber(code) || !cJSON_IsNumber(high) || !cJSON_IsNumber(low)) continue;

        weather_day_t *day = &state->days[state->day_count];
        if (!parse_iso_date_noon_utc(date->valuestring, tz, &day->dt_utc)) continue;
        day->hi_f = (int16_t)round_double_to_i32(cJSON_GetNumberValue(high));
        day->lo_f = (int16_t)round_double_to_i32(cJSON_GetNumberValue(low));
        day->pop_pct = 0;
        if (cJSON_IsArray(pop)) {
            cJSON *chance = cJSON_GetArrayItem(pop, i);
            if (cJSON_IsNumber(chance)) {
                int pct = round_double_to_i32(cJSON_GetNumberValue(chance));
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                day->pop_pct = (uint8_t)pct;
            }
        }
        open_meteo_weather_code((int)cJSON_GetNumberValue(code), day->condition, sizeof(day->condition),
                                day->icon, sizeof(day->icon));
        state->day_count++;
    }

    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    cJSON *hour_times = cJSON_GetObjectItem(hourly, "time");
    cJSON *hour_temp = cJSON_GetObjectItem(hourly, "temperature_2m");
    cJSON *hour_humidity = cJSON_GetObjectItem(hourly, "relative_humidity_2m");
    cJSON *hour_pressure = cJSON_GetObjectItem(hourly, "pressure_msl");
    cJSON *hour_pop = cJSON_GetObjectItem(hourly, "precipitation_probability");
    cJSON *hour_precip = cJSON_GetObjectItem(hourly, "precipitation");
    cJSON *hour_rain = cJSON_GetObjectItem(hourly, "rain");
    cJSON *hour_snow = cJSON_GetObjectItem(hourly, "snowfall");
    if (cJSON_IsArray(hour_times) && cJSON_IsArray(hour_temp) &&
        cJSON_IsArray(hour_humidity) && cJSON_IsArray(hour_pressure)) {
        int hour_count = cJSON_GetArraySize(hour_times);
        if (hour_count > cJSON_GetArraySize(hour_temp)) hour_count = cJSON_GetArraySize(hour_temp);
        if (hour_count > cJSON_GetArraySize(hour_humidity)) hour_count = cJSON_GetArraySize(hour_humidity);
        if (hour_count > cJSON_GetArraySize(hour_pressure)) hour_count = cJSON_GetArraySize(hour_pressure);
        if (cJSON_IsArray(hour_pop) && hour_count > cJSON_GetArraySize(hour_pop)) hour_count = cJSON_GetArraySize(hour_pop);
        if (cJSON_IsArray(hour_precip) && hour_count > cJSON_GetArraySize(hour_precip)) hour_count = cJSON_GetArraySize(hour_precip);
        if (cJSON_IsArray(hour_rain) && hour_count > cJSON_GetArraySize(hour_rain)) hour_count = cJSON_GetArraySize(hour_rain);
        if (cJSON_IsArray(hour_snow) && hour_count > cJSON_GetArraySize(hour_snow)) hour_count = cJSON_GetArraySize(hour_snow);
        if (hour_count > WEATHER_FORECAST_HOURS) hour_count = WEATHER_FORECAST_HOURS;

        for (int i = 0; i < hour_count; i++) {
            cJSON *time_node = cJSON_GetArrayItem(hour_times, i);
            cJSON *temp_node = cJSON_GetArrayItem(hour_temp, i);
            cJSON *humidity_node = cJSON_GetArrayItem(hour_humidity, i);
            cJSON *pressure_node = cJSON_GetArrayItem(hour_pressure, i);
            if (!cJSON_IsString(time_node) || !cJSON_IsNumber(temp_node) ||
                !cJSON_IsNumber(humidity_node) || !cJSON_IsNumber(pressure_node)) continue;

            weather_hour_t *hour = &state->hours[state->hour_count];
            if (!parse_iso_hour_utc(time_node->valuestring, tz, &hour->dt_utc)) continue;
            hour->temp_f = (int16_t)round_double_to_i32(cJSON_GetNumberValue(temp_node));

            int humidity = round_double_to_i32(cJSON_GetNumberValue(humidity_node));
            if (humidity < 0) humidity = 0;
            if (humidity > 100) humidity = 100;
            hour->humidity_pct = (uint8_t)humidity;
            hour->pressure_hpa = clamp_u16(cJSON_GetNumberValue(pressure_node));

            if (cJSON_IsArray(hour_pop)) {
                cJSON *pop_node = cJSON_GetArrayItem(hour_pop, i);
                if (cJSON_IsNumber(pop_node)) {
                    int pct = round_double_to_i32(cJSON_GetNumberValue(pop_node));
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    hour->pop_pct = (uint8_t)pct;
                }
            }
            if (cJSON_IsArray(hour_precip)) {
                cJSON *precip_node = cJSON_GetArrayItem(hour_precip, i);
                if (cJSON_IsNumber(precip_node)) hour->precip_in_x100 = clamp_u16(cJSON_GetNumberValue(precip_node) * 100.0);
            }
            if (cJSON_IsArray(hour_rain)) {
                cJSON *rain_node = cJSON_GetArrayItem(hour_rain, i);
                if (cJSON_IsNumber(rain_node)) hour->rain_in_x100 = clamp_u16(cJSON_GetNumberValue(rain_node) * 100.0);
            }
            if (cJSON_IsArray(hour_snow)) {
                cJSON *snow_node = cJSON_GetArrayItem(hour_snow, i);
                if (cJSON_IsNumber(snow_node)) hour->snow_in_x100 = clamp_u16(cJSON_GetNumberValue(snow_node) * 100.0);
            }
            state->hour_count++;
        }
    }

    cJSON_Delete(root);
    return state->day_count ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
#endif

static esp_err_t http_get(const char *url, http_body_t *body, int *out_status,
                          uint32_t timeout_ms)
{
    if (services_network_bulk_active()) return ESP_ERR_INVALID_STATE;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_cb,
        .user_data = body,
        .max_redirection_count = 2,
        .user_agent = "P4-WallDisplay/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return ESP_ERR_NO_MEM;
    services_network_bulk_begin();
    services_https_lock();
    sd_storage_set_network_busy(true);
    esp_err_t err = esp_http_client_perform(client);
    if (out_status) *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    sd_storage_set_network_busy(false);
    services_https_unlock();
    services_network_bulk_end();
    return err;
}

static void weather_format_error(char *out, size_t out_len, esp_err_t err)
{
    if (!out || out_len == 0) return;
    if (err == ESP_ERR_TIMEOUT) {
        snprintf(out, out_len, "Weather timed out");
    } else if (err == ESP_ERR_HTTP_CONNECT) {
        snprintf(out, out_len, "Weather connect failed");
    } else if (err == ESP_ERR_NO_MEM) {
        snprintf(out, out_len, "Weather memory tight");
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        snprintf(out, out_len, "Weather response invalid");
    } else if (err >= ESP_ERR_HTTP_BASE + 100 && err < ESP_ERR_HTTP_BASE + 600) {
        int status = (int)(err - ESP_ERR_HTTP_BASE);
        if (status == 401) snprintf(out, out_len, "Weather API key rejected");
        else if (status == 404) snprintf(out, out_len, "Weather location not found");
        else if (status == 429) snprintf(out, out_len, "Weather rate limited");
        else snprintf(out, out_len, "Weather HTTP %d", status);
    } else {
        snprintf(out, out_len, "%s", esp_err_to_name(err));
    }
}

static esp_err_t publish_weather_state(const weather_state_t *state)
{
    return weather_state_set(state);
}

#if WEATHER_ENABLE_FORECAST
static void preserve_previous_forecast(weather_state_t *state, const weather_state_t *previous)
{
    if (!state || !previous || !previous->valid) return;

    if (previous->day_count) {
        memcpy(state->days, previous->days, sizeof(state->days));
        state->day_count = previous->day_count > WEATHER_FORECAST_DAYS ?
                           WEATHER_FORECAST_DAYS : previous->day_count;
    }
    if (previous->hour_count) {
        memcpy(state->hours, previous->hours, sizeof(state->hours));
        state->hour_count = previous->hour_count > WEATHER_FORECAST_HOURS ?
                            WEATHER_FORECAST_HOURS : previous->hour_count;
    }
    if (!state->wind_gust_valid && previous->wind_gust_valid) {
        state->wind_gust_mph_x10 = previous->wind_gust_mph_x10;
        state->wind_gust_valid = true;
    }
    if (!state->uv_index_valid && previous->uv_index_valid) {
        state->uv_index = previous->uv_index;
        state->uv_index_valid = true;
    }
}
#endif

static void save_history_after_network_quiet(void)
{
    weather_state_t state;
    if (weather_state_get(&state) != ESP_OK || !state.valid) return;

    vTaskDelay(pdMS_TO_TICKS(WEATHER_HISTORY_SAVE_DELAY_MS));

    ESP_LOGI(TAG, "weather history save");
    esp_err_t hist_err = weather_history_record(&state);
    if (hist_err != ESP_OK) {
        ESP_LOGW(TAG, "weather history not saved: %s", esp_err_to_name(hist_err));
    }
}

static esp_err_t fetch_once(const weather_config_t *cfg)
{
    app_tuning_config_t tuning;
    weather_load_tuning(&tuning);

    char lat[20];
    char lon[20];
    char url[640];
    format_coord(lat, sizeof(lat), cfg->lat_x1e6);
    format_coord(lon, sizeof(lon), cfg->lon_x1e6);

    weather_state_t state = {0};
#if WEATHER_ENABLE_FORECAST
    weather_state_t previous = {0};
    bool have_previous = weather_state_get(&previous) == ESP_OK && previous.valid;
#endif

    /* --- current observation --- */
    services_status_note_weather(true, false, "Fetching current");
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s&appid=%s&units=imperial",
             lat, lon, cfg->api_key);

    ESP_LOGI(TAG, "weather fetch current");
    http_body_t body = {0};
    int status = 0;
    esp_err_t err = http_get(url, &body, &status,
                             (uint32_t)tuning.weather_http_timeout_s * 1000u);
    if (err != ESP_OK) {
        free(body.data);
        ESP_LOGW(TAG, "weather (current) fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "weather (current) HTTP %d", status);
        free(body.data);
        return ESP_ERR_HTTP_BASE + status;
    }
    if (!body.data || body.len == 0) {
        free(body.data);
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = parse_current(body.data, body.len, &state);
    free(body.data);
    if (err != ESP_OK) return err;
    services_note_weather_timezone(cfg->lat_x1e6, cfg->lon_x1e6,
                                   state.tz_offset_s, state.city, state.country);

#if WEATHER_ENABLE_FORECAST
    if (have_previous) preserve_previous_forecast(&state, &previous);
#endif

    state.fetched_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    err = publish_weather_state(&state);
    if (err != ESP_OK) return err;

#if WEATHER_ENABLE_FORECAST
    vTaskDelay(pdMS_TO_TICKS((uint32_t)tuning.weather_forecast_gap_s * 1000u));
    services_status_note_weather(true, false, "Fetching forecast");
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=wind_gusts_10m,uv_index&hourly=temperature_2m,relative_humidity_2m,pressure_msl,precipitation_probability,precipitation,rain,snowfall&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,uv_index_max&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch&timezone=auto&forecast_days=%u&forecast_hours=%u",
             lat, lon, (unsigned)WEATHER_FORECAST_DAYS, (unsigned)WEATHER_FORECAST_HOURS);

    ESP_LOGI(TAG, "weather fetch forecast");
    http_body_t fbody = {0};
    int fstatus = 0;
    esp_err_t ferr = http_get(url, &fbody, &fstatus,
                              (uint32_t)tuning.weather_http_timeout_s * 1000u);
    if (ferr == ESP_OK && fstatus == 200 && fbody.data && fbody.len > 0) {
        ferr = parse_forecast(fbody.data, fbody.len, &state);
        if (ferr == ESP_OK) {
            char gust[16];
            char uv[8];
            if (state.wind_gust_valid) {
                snprintf(gust, sizeof(gust), "%u.%u mph",
                         state.wind_gust_mph_x10 / 10, state.wind_gust_mph_x10 % 10);
            } else {
                snprintf(gust, sizeof(gust), "n/a");
            }
            if (state.uv_index_valid) snprintf(uv, sizeof(uv), "%u", state.uv_index);
            else                      snprintf(uv, sizeof(uv), "n/a");
            ESP_LOGI(TAG, "weather forecast updated: %u days, %u hours, gust=%s, uv=%s",
                     state.day_count, state.hour_count, gust, uv);
        }
    } else {
        if (ferr == ESP_OK) ferr = ESP_ERR_INVALID_RESPONSE;
    }
    if (ferr != ESP_OK) {
        ESP_LOGW(TAG, "weather (forecast) skipped: err=%s status=%d",
                 esp_err_to_name(ferr), fstatus);
    }
    free(fbody.data);

    state.fetched_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    err = publish_weather_state(&state);
    return err;
#else
    return ESP_OK;
#endif
}

static void weather_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "weather worker started");
    while (1) {
        app_tuning_config_t tuning;
        weather_load_tuning(&tuning);

        weather_config_t cfg;
        if (!read_config(&cfg)) {
            ESP_LOGW(TAG, "weather config missing: no saved or build-time API key");
            (void)weather_state_clear();
            services_status_note_weather(false, false, "Not configured");
            weather_wait_ms((uint32_t)tuning.weather_retry_s * 1000u);
            continue;
        }

        services_status_t status;
        services_status_get(&status);
        if (!status.wifi_connected) {
            ESP_LOGI(TAG, "weather waiting for Wi-Fi");
            services_status_note_weather(true, false, "Waiting for Wi-Fi");
            weather_wait_ms((uint32_t)tuning.weather_wifi_wait_s * 1000u);
            continue;
        }
        if (!status.location_has_coords) {
            if (cfg.has_cached_location) {
                ESP_LOGI(TAG, "weather using cached location (%ld,%ld)",
                         (long)cfg.lat_x1e6, (long)cfg.lon_x1e6);
                services_status_note_weather(true, false, "Using saved location");
            } else {
                ESP_LOGI(TAG, "weather waiting for IP location");
                (void)services_location_request_refresh();
                services_status_note_weather(true, false, "Waiting for IP location");
                weather_wait_ms((uint32_t)tuning.weather_wifi_wait_s * 1000u);
                continue;
            }
        } else {
            cfg.lat_x1e6 = status.location_lat_x1e6;
            cfg.lon_x1e6 = status.location_lon_x1e6;
        }

        services_status_note_weather(true, false, "Fetching");
        esp_err_t err = fetch_once(&cfg);
        if (err == ESP_OK) {
            services_status_note_weather(true, true, "Fresh");
            ESP_LOGI(TAG, "weather updated");
            save_history_after_network_quiet();
            weather_wait_ms((uint32_t)tuning.weather_refresh_min * 60u * 1000u);
        } else {
            char detail[56];
            weather_format_error(detail, sizeof(detail), err);
            services_status_note_weather(true, false, detail);
            weather_wait_ms((uint32_t)tuning.weather_retry_s * 1000u);
        }
    }
}

esp_err_t weather_api_init(void)
{
    if (s_weather_task) return ESP_OK;
    (void)weather_history_init();
    weather_config_t cfg;
    bool configured = read_config(&cfg);
    ESP_LOGI(TAG, "weather init: configured=%d cached_location=%d", configured, cfg.has_cached_location);
    if (!configured) {
        services_status_note_weather(false, false, "Not configured");
    } else {
        services_status_note_weather(true, false, "Weather queued");
    }

    BaseType_t ok = xTaskCreateWithCaps(weather_worker, "weather_api", WEATHER_TASK_STACK_BYTES,
                                        NULL, 4, &s_weather_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) ok = xTaskCreate(weather_worker, "weather_api", WEATHER_TASK_STACK_BYTES,
                                       NULL, 4, &s_weather_task);
    if (ok == pdPASS) {
        return ESP_OK;
    }
    s_weather_task = NULL;
    ESP_LOGE(TAG, "weather task create failed: largest_internal=%u largest_psram=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    services_status_note_weather(cfg.api_key[0], false, "Weather task failed");
    return ESP_ERR_NO_MEM;
}

esp_err_t weather_api_request_refresh(void)
{
    esp_err_t err = weather_api_init();
    if (err != ESP_OK) return err;
    if (s_weather_task) xTaskNotifyGive(s_weather_task);
    return ESP_OK;
}
