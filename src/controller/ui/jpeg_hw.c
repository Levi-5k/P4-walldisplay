#include "jpeg_hw.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "jpeg_hw";

static jpeg_decoder_handle_t s_decoder;

static const char *jpeg_hw_sample_name(jpeg_down_sampling_type_t sample_method)
{
    switch (sample_method) {
        case JPEG_DOWN_SAMPLING_YUV444: return "YUV444";
        case JPEG_DOWN_SAMPLING_YUV422: return "YUV422";
        case JPEG_DOWN_SAMPLING_YUV420: return "YUV420";
        case JPEG_DOWN_SAMPLING_GRAY: return "GRAY";
        default: return "unknown";
    }
}

static esp_err_t jpeg_hw_output_dimensions(const jpeg_decode_picture_info_t *info,
                                           uint32_t *out_w, uint32_t *out_h)
{
    if (!info || !out_w || !out_h || info->width == 0 || info->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t mcu_w = 8;
    uint32_t mcu_h = 8;
    switch (info->sample_method) {
        case JPEG_DOWN_SAMPLING_YUV444:
        case JPEG_DOWN_SAMPLING_GRAY:
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
            mcu_w = 16;
            break;
        case JPEG_DOWN_SAMPLING_YUV420:
            mcu_w = 16;
            mcu_h = 16;
            break;
        default:
            ESP_LOGE(TAG, "unsupported JPEG sampling method: %u", (unsigned)info->sample_method);
            return ESP_ERR_NOT_SUPPORTED;
    }

    *out_w = ((info->width + mcu_w - 1u) / mcu_w) * mcu_w;
    *out_h = ((info->height + mcu_h - 1u) / mcu_h) * mcu_h;
    return ESP_OK;
}

esp_err_t jpeg_hw_init(void)
{
    if (s_decoder) return ESP_OK;
    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    esp_err_t err = jpeg_new_decoder_engine(&cfg, &s_decoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create HW JPEG decoder: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t jpeg_hw_decode_to_file(const uint8_t *jpeg_data, size_t jpeg_len,
                                  const char *out_path)
{
    if (!jpeg_data || jpeg_len < 4 || !out_path) return ESP_ERR_INVALID_ARG;

    esp_err_t err = jpeg_hw_init();
    if (err != ESP_OK) return err;

    jpeg_decode_picture_info_t info;
    err = jpeg_decoder_get_info(jpeg_data, (uint32_t)jpeg_len, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to parse JPEG header: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t out_w = 0;
    uint32_t out_h = 0;
    err = jpeg_hw_output_dimensions(&info, &out_w, &out_h);
    if (err != ESP_OK) return err;
    uint32_t out_size = out_w * out_h * 2u;

    ESP_LOGI(TAG, "decoding %ux%u JPEG (sample=%s, padded %ux%u, %u KB)",
             (unsigned)info.width, (unsigned)info.height,
             jpeg_hw_sample_name(info.sample_method),
             (unsigned)out_w, (unsigned)out_h, (unsigned)(out_size / 1024));

    jpeg_decode_memory_alloc_cfg_t in_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t input_alloc_size = 0;
    uint8_t *input_buf = jpeg_alloc_decoder_mem(jpeg_len, &in_mem_cfg, &input_alloc_size);
    if (!input_buf) {
        ESP_LOGE(TAG, "failed to allocate %u bytes for JPEG input", (unsigned)jpeg_len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(input_buf, jpeg_data, jpeg_len);

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_size = 0;
    uint8_t *out_buf = jpeg_alloc_decoder_mem(out_size, &mem_cfg, &alloc_size);
    if (!out_buf) {
        ESP_LOGE(TAG, "failed to allocate %u bytes for decoded output", (unsigned)out_size);
        free(input_buf);
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t decoded_size = 0;
    err = jpeg_decoder_process(s_decoder, &decode_cfg,
                                input_buf, (uint32_t)jpeg_len,
                                out_buf, alloc_size, &decoded_size);
    free(input_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HW decode failed: %s", esp_err_to_name(err));
        free(out_buf);
        return err;
    }
    if (decoded_size != out_size) {
        ESP_LOGE(TAG, "HW decode size mismatch: got %u, expected %u",
                 (unsigned)decoded_size, (unsigned)out_size);
        free(out_buf);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Write raw file: [u16 width][u16 height][u16 padded_w][u16 padded_h][pixel data]
     * The real dimensions let the display code crop the padding. */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", out_path);
        free(out_buf);
        return ESP_FAIL;
    }

    uint16_t hdr[4] = {
        (uint16_t)info.width, (uint16_t)info.height,
        (uint16_t)out_w, (uint16_t)out_h
    };
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    if (ok) {
        size_t off = 0;
        while (off < decoded_size) {
            size_t chunk = decoded_size - off;
            if (chunk > 4096) chunk = 4096;
            if (fwrite(out_buf + off, 1, chunk, f) != chunk) { ok = false; break; }
            off += chunk;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    if (ok) ok = fflush(f) == 0;
    fclose(f);
    free(out_buf);

    if (!ok) {
        remove(out_path);
        ESP_LOGE(TAG, "write failed for %s", out_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %s (%u KB)", out_path, (unsigned)(decoded_size / 1024));
    return ESP_OK;
}
