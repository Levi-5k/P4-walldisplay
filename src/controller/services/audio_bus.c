#include "audio_bus.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "audio_bus";

#define AUDIO_BUS_DMA_DESC_NUM  4
#define AUDIO_BUS_DMA_FRAME_NUM 128

static i2s_chan_handle_t s_i2s_tx_chan;
static i2s_chan_handle_t s_i2s_rx_chan;
static const audio_codec_data_if_t *s_i2s_data_if;
static char s_last_error[96] = "not initialized";

static void audio_bus_set_error(const char *label, esp_err_t err)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s: %s", label, esp_err_to_name(err));
}

static void audio_bus_reset_i2s(void)
{
    if (s_i2s_tx_chan) {
        (void)i2s_channel_disable(s_i2s_tx_chan);
        (void)i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
    }
    if (s_i2s_rx_chan) {
        (void)i2s_channel_disable(s_i2s_rx_chan);
        (void)i2s_del_channel(s_i2s_rx_chan);
        s_i2s_rx_chan = NULL;
    }
    s_i2s_data_if = NULL;
}

esp_err_t audio_bus_init(void)
{
    if (s_i2s_data_if) return ESP_OK;

    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        audio_bus_set_error("bsp_i2c_init", err);
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_BUS_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_BUS_DMA_FRAME_NUM;

    err = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan);
    if (err != ESP_OK) {
        audio_bus_set_error("i2s_new_channel(TX+RX)", err);
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }

    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_BUS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_tx_chan, &i2s_cfg);
    if (err != ESP_OK) {
        audio_bus_set_error("i2s_channel_init_std_mode(TX)", err);
        ESP_LOGE(TAG, "%s", s_last_error);
        audio_bus_reset_i2s();
        return err;
    }

    err = i2s_channel_init_std_mode(s_i2s_rx_chan, &i2s_cfg);
    if (err != ESP_OK) {
        audio_bus_set_error("i2s_channel_init_std_mode(RX)", err);
        ESP_LOGE(TAG, "%s", s_last_error);
        audio_bus_reset_i2s();
        return err;
    }

    err = i2s_channel_enable(s_i2s_tx_chan);
    if (err != ESP_OK) {
        audio_bus_set_error("i2s_channel_enable(TX)", err);
        ESP_LOGE(TAG, "%s", s_last_error);
        audio_bus_reset_i2s();
        return err;
    }

    err = i2s_channel_enable(s_i2s_rx_chan);
    if (err != ESP_OK) {
        audio_bus_set_error("i2s_channel_enable(RX)", err);
        ESP_LOGE(TAG, "%s", s_last_error);
        audio_bus_reset_i2s();
        return err;
    }

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!s_i2s_data_if) {
        snprintf(s_last_error, sizeof(s_last_error), "audio_codec_new_i2s_data failed");
        ESP_LOGE(TAG, "%s", s_last_error);
        audio_bus_reset_i2s();
        return ESP_ERR_NO_MEM;
    }

    snprintf(s_last_error, sizeof(s_last_error), "ready");
    ESP_LOGI(TAG, "Shared I2S ready: port %d, TX+RX, %d DMA buffers x %d frames @ %d Hz",
             CONFIG_BSP_I2S_NUM, AUDIO_BUS_DMA_DESC_NUM, AUDIO_BUS_DMA_FRAME_NUM,
             AUDIO_BUS_SAMPLE_RATE);
    return ESP_OK;
}

bool audio_bus_is_ready(void)
{
    return s_i2s_data_if != NULL;
}

const audio_codec_data_if_t *audio_bus_data_if(void)
{
    return s_i2s_data_if;
}

const char *audio_bus_last_error(void)
{
    return s_last_error;
}