#include "jpeg_hw.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/jpeg_decode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

static uint8_t clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static int div16_round(int value)
{
    return value >= 0 ? (value + 8) / 16 : (value - 8) / 16;
}

static void add_rgb_error(int16_t *errors, uint32_t pixel, int er, int eg, int eb, int weight)
{
    uint32_t base = pixel * 3u;
    errors[base + 0] = (int16_t)(errors[base + 0] + er * weight);
    errors[base + 1] = (int16_t)(errors[base + 1] + eg * weight);
    errors[base + 2] = (int16_t)(errors[base + 2] + eb * weight);
}

static esp_err_t dither_rgb888_to_rgb565(const uint8_t *rgb, uint32_t width, uint32_t height,
                                         uint8_t **out_raw, size_t *out_bytes)
{
    if (!rgb || !out_raw || !out_bytes || width == 0 || height == 0) return ESP_ERR_INVALID_ARG;
    size_t raw_size = (size_t)width * height * 2u;
    uint8_t *raw = heap_caps_malloc(raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) raw = malloc(raw_size);
    if (!raw) return ESP_ERR_NO_MEM;

    size_t err_count = (size_t)width * 3u;
    int16_t *this_err = heap_caps_calloc(err_count, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *next_err = heap_caps_calloc(err_count, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!this_err) this_err = calloc(err_count, sizeof(int16_t));
    if (!next_err) next_err = calloc(err_count, sizeof(int16_t));
    if (!this_err || !next_err) {
        free(this_err);
        free(next_err);
        free(raw);
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t y = 0; y < height; y++) {
        memset(next_err, 0, err_count * sizeof(int16_t));
        for (uint32_t x = 0; x < width; x++) {
            size_t rgb_off = ((size_t)y * width + x) * 3u;
            size_t raw_off = ((size_t)y * width + x) * 2u;
            uint32_t err_base = x * 3u;

            uint8_t r = clamp_u8((int)rgb[rgb_off + 0] + div16_round(this_err[err_base + 0]));
            uint8_t g = clamp_u8((int)rgb[rgb_off + 1] + div16_round(this_err[err_base + 1]));
            uint8_t b = clamp_u8((int)rgb[rgb_off + 2] + div16_round(this_err[err_base + 2]));

            uint8_t r5 = r >> 3;
            uint8_t g6 = g >> 2;
            uint8_t b5 = b >> 3;
            uint8_t rq = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t gq = (uint8_t)((g6 << 2) | (g6 >> 4));
            uint8_t bq = (uint8_t)((b5 << 3) | (b5 >> 2));
            uint16_t rgb565 = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            raw[raw_off + 0] = (uint8_t)(rgb565 & 0xFFu);
            raw[raw_off + 1] = (uint8_t)(rgb565 >> 8);

            int er = (int)r - (int)rq;
            int eg = (int)g - (int)gq;
            int eb = (int)b - (int)bq;
            if (x + 1 < width) add_rgb_error(this_err, x + 1, er, eg, eb, 7);
            if (y + 1 < height) {
                if (x > 0) add_rgb_error(next_err, x - 1, er, eg, eb, 3);
                add_rgb_error(next_err, x, er, eg, eb, 5);
                if (x + 1 < width) add_rgb_error(next_err, x + 1, er, eg, eb, 1);
            }
        }
        int16_t *swap = this_err;
        this_err = next_err;
        next_err = swap;
    }

    free(this_err);
    free(next_err);
    *out_raw = raw;
    *out_bytes = raw_size;
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
    uint32_t rgb_size = out_w * out_h * 3u;

    ESP_LOGI(TAG, "decoding %ux%u JPEG (sample=%s, padded %ux%u, %u KB)",
             (unsigned)info.width, (unsigned)info.height,
             jpeg_hw_sample_name(info.sample_method),
             (unsigned)out_w, (unsigned)out_h, (unsigned)(rgb_size / 1024));

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
    uint8_t *out_buf = jpeg_alloc_decoder_mem(rgb_size, &mem_cfg, &alloc_size);
    if (!out_buf) {
        ESP_LOGE(TAG, "failed to allocate %u bytes for decoded output", (unsigned)rgb_size);
        free(input_buf);
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
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
    if (decoded_size != rgb_size) {
        ESP_LOGE(TAG, "HW decode size mismatch: got %u, expected %u",
                 (unsigned)decoded_size, (unsigned)rgb_size);
        free(out_buf);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *raw_buf = NULL;
    size_t raw_size = 0;
    err = dither_rgb888_to_rgb565(out_buf, out_w, out_h, &raw_buf, &raw_size);
    free(out_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB565 dither failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Write raw file: [u16 width][u16 height][u16 padded_w][u16 padded_h][pixel data]
     * The real dimensions let the display code crop the padding. */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", out_path);
        free(raw_buf);
        return ESP_FAIL;
    }

    uint16_t hdr[4] = {
        (uint16_t)info.width, (uint16_t)info.height,
        (uint16_t)out_w, (uint16_t)out_h
    };
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    if (ok) {
        size_t off = 0;
        while (off < raw_size) {
            size_t chunk = raw_size - off;
            if (chunk > 4096) chunk = 4096;
            if (fwrite(raw_buf + off, 1, chunk, f) != chunk) { ok = false; break; }
            off += chunk;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    if (ok) ok = fflush(f) == 0;
    fclose(f);
    free(raw_buf);

    if (!ok) {
        remove(out_path);
        ESP_LOGE(TAG, "write failed for %s", out_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %s (%u KB, dithered RGB565)", out_path, (unsigned)(raw_size / 1024));
    return ESP_OK;
}
