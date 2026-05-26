#include "weather_state.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "weather_state";

static SemaphoreHandle_t s_mtx;
static weather_state_t   s_state;

esp_err_t weather_state_init(void)
{
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "weather state ready");
    return ESP_OK;
}

esp_err_t weather_state_get(weather_state_t *out)
{
    if (!out || !s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t weather_state_set(const weather_state_t *in)
{
    if (!in || !s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state = *in;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t weather_state_clear(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memset(&s_state, 0, sizeof(s_state));
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}
