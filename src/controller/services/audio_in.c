#include "audio_in.h"
#include "board_pins.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "audio_in";

#define AUDIO_IN_TASK_STACK   4096
#define AUDIO_IN_TASK_PRIO    7
#define MAX_SUBSCRIBERS       2

static esp_codec_dev_handle_t s_mic_dev;
static TaskHandle_t s_task;
static bool s_ready;

static struct {
    audio_frame_cb_t cb;
    void *user;
} s_subs[MAX_SUBSCRIBERS];
static uint8_t s_sub_count;

esp_err_t audio_in_subscribe(audio_frame_cb_t cb, void *user)
{
    if (!cb || s_sub_count >= MAX_SUBSCRIBERS) return ESP_ERR_INVALID_ARG;
    s_subs[s_sub_count].cb = cb;
    s_subs[s_sub_count].user = user;
    s_sub_count++;
    return ESP_OK;
}

bool audio_in_is_ready(void)
{
    return s_ready;
}

static void audio_capture_task(void *arg)
{
    (void)arg;
    audio_frame_t frame;
    const int read_bytes = AUDIO_IN_FRAME_SAMPLES * sizeof(int16_t);

    while (1) {
        int ret = esp_codec_dev_read(s_mic_dev, frame.samples, read_bytes);
        if (ret != 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        frame.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

        for (uint8_t i = 0; i < s_sub_count; i++) {
            s_subs[i].cb(&frame, s_subs[i].user);
        }
    }
}

esp_err_t audio_in_start(void)
{
    if (s_ready) return ESP_OK;

    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_IN_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DSDIN,
            .din  = BSP_I2S_ASDOUT,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mic_dev = bsp_audio_codec_microphone_init();
    if (!s_mic_dev) {
        ESP_LOGE(TAG, "Microphone codec init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_IN_SAMPLE_RATE,
        .channel = AUDIO_IN_CHANNELS,
        .bits_per_sample = 16,
    };
    int codec_err = esp_codec_dev_open(s_mic_dev, &fs);
    if (codec_err != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open(mic) failed: %d", codec_err);
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCore(audio_capture_task, "audio_in",
                                 AUDIO_IN_TASK_STACK, NULL,
                                 AUDIO_IN_TASK_PRIO, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio capture task");
        esp_codec_dev_close(s_mic_dev);
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Audio capture started: %d Hz mono, %d-sample frames",
             AUDIO_IN_SAMPLE_RATE, AUDIO_IN_FRAME_SAMPLES);
    return ESP_OK;
}
