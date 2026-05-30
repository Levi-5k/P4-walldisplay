#include "audio_in.h"

#include "audio_bus.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_in";

#define AUDIO_IN_TASK_STACK    4096
#define AUDIO_IN_TASK_PRIO     7
#define AUDIO_IN_DMA_DESC_NUM  4
#define AUDIO_IN_DMA_FRAME_NUM 128
#define MAX_SUBSCRIBERS        2

static esp_codec_dev_handle_t s_mic_dev;
static i2s_chan_handle_t s_i2s_rx_chan;
static const audio_codec_data_if_t *s_i2s_data_if;
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

static void audio_in_reset_i2s(void)
{
    if (s_i2s_rx_chan) {
        (void)i2s_channel_disable(s_i2s_rx_chan);
        (void)i2s_del_channel(s_i2s_rx_chan);
        s_i2s_rx_chan = NULL;
    }
    s_i2s_data_if = NULL;
}

static esp_err_t audio_in_init_mic_path(void)
{
    if (s_i2s_data_if) return ESP_OK;

    esp_err_t err = audio_bus_init();
    if (err == ESP_OK) {
        s_i2s_data_if = audio_bus_data_if();
        if (!s_i2s_data_if) return ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "Mic using shared I2S audio bus");
    } else {
        ESP_LOGW(TAG, "shared audio bus unavailable; falling back to mic-only RX: %s", audio_bus_last_error());

        err = bsp_i2c_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bsp_i2c_init failed: %s", esp_err_to_name(err));
            return err;
        }

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = AUDIO_IN_DMA_DESC_NUM;
        chan_cfg.dma_frame_num = AUDIO_IN_DMA_FRAME_NUM;

        err = i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_new_channel(RX) failed: %s", esp_err_to_name(err));
            return err;
        }

        i2s_std_config_t i2s_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_IN_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = BSP_I2S_MCLK,
                .bclk = BSP_I2S_SCLK,
                .ws   = BSP_I2S_LCLK,
                .dout = I2S_GPIO_UNUSED,
                .din  = BSP_I2S_DSIN,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };

        err = i2s_channel_init_std_mode(s_i2s_rx_chan, &i2s_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_std_mode(RX) failed: %s", esp_err_to_name(err));
            audio_in_reset_i2s();
            return err;
        }

        err = i2s_channel_enable(s_i2s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable(RX) failed: %s", esp_err_to_name(err));
            audio_in_reset_i2s();
            return err;
        }

        audio_codec_i2s_cfg_t i2s_data_cfg = {
            .port = CONFIG_BSP_I2S_NUM,
            .rx_handle = s_i2s_rx_chan,
            .tx_handle = NULL,
        };
        s_i2s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
        if (!s_i2s_data_if) {
            ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
            audio_in_reset_i2s();
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "Mic I2S RX ready: port %d, %d DMA buffers x %d frames",
                 CONFIG_BSP_I2S_NUM, AUDIO_IN_DMA_DESC_NUM, AUDIO_IN_DMA_FRAME_NUM);
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        if (!audio_bus_is_ready()) audio_in_reset_i2s();
        return ESP_ERR_NO_MEM;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    if (!es7210_dev) {
        ESP_LOGE(TAG, "es7210_codec_new failed");
        if (!audio_bus_is_ready()) audio_in_reset_i2s();
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = s_i2s_data_if,
    };
    s_mic_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (!s_mic_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new(mic) failed");
        if (!audio_bus_is_ready()) audio_in_reset_i2s();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t audio_in_start(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = audio_in_init_mic_path();
    if (err != ESP_OK) return err;

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
