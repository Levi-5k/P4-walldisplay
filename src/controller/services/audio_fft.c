#include "audio_fft.h"
#include "audio_in.h"

#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "audio_fft";

#define MAX_FFT_SUBS    2
#define SMOOTH_ALPHA    0.08f
#define PEAK_DECAY      0.92f
#define PEAK_THRESHOLD  1.4f
#define NOISE_FLOOR     80.0f

static float *s_fft_buf;
static float *s_window;
static float *s_magnitudes;
static bool s_ready;
static float s_smooth_level;
static float s_peak_level;

static struct {
    audio_fft_cb_t cb;
    void *user;
} s_subs[MAX_FFT_SUBS];
static uint8_t s_sub_count;

static const uint16_t BIN_EDGES[FFT_BINS + 1] = {
    2, 3, 4, 6, 8, 11, 16, 22, 32, 44, 64, 88, 128, 176, 224, 248, 256
};

static esp_err_t audio_fft_alloc_buffers(void)
{
    if (s_fft_buf && s_window && s_magnitudes) return ESP_OK;

    s_fft_buf = heap_caps_malloc(sizeof(float) * FFT_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_window = heap_caps_malloc(sizeof(float) * FFT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_magnitudes = heap_caps_malloc(sizeof(float) * (FFT_SIZE / 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fft_buf || !s_window || !s_magnitudes) {
        free(s_fft_buf);
        free(s_window);
        free(s_magnitudes);
        s_fft_buf = NULL;
        s_window = NULL;
        s_magnitudes = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_fft_subscribe(audio_fft_cb_t cb, void *user)
{
    if (!cb || s_sub_count >= MAX_FFT_SUBS) return ESP_ERR_INVALID_ARG;
    s_subs[s_sub_count].cb = cb;
    s_subs[s_sub_count].user = user;
    s_sub_count++;
    return ESP_OK;
}

bool audio_fft_is_ready(void)
{
    return s_ready;
}

static void compute_fft(const int16_t *samples, audio_fft_result_t *out)
{
    float sum_sq = 0.0f;
    float max_sample = 0.0f;

    for (int i = 0; i < FFT_SIZE; i++) {
        float s = (float)samples[i] / 32768.0f;
        s_fft_buf[i * 2]     = s * s_window[i];
        s_fft_buf[i * 2 + 1] = 0.0f;
        sum_sq += s * s;
        float abs_s = s < 0 ? -s : s;
        if (abs_s > max_sample) max_sample = abs_s;
    }

    out->sample_raw = max_sample * 255.0f;

    dsps_fft2r_fc32(s_fft_buf, FFT_SIZE);
    dsps_bit_rev_fc32(s_fft_buf, FFT_SIZE);

    for (int i = 0; i < FFT_SIZE / 2; i++) {
        float re = s_fft_buf[i * 2];
        float im = s_fft_buf[i * 2 + 1];
        s_magnitudes[i] = sqrtf(re * re + im * im);
    }

    float dominant_scaled = 0.0f;
    uint8_t dominant_band = 1;

    for (int b = 0; b < FFT_BINS; b++) {
        float bin_sum = 0.0f;
        int count = 0;
        for (int i = BIN_EDGES[b]; i < BIN_EDGES[b + 1] && i < FFT_SIZE / 2; i++) {
            bin_sum += s_magnitudes[i];
            count++;
        }
        float avg = count > 0 ? bin_sum / (float)count : 0.0f;
        float db = 20.0f * log10f(avg + 1e-10f);
        float scaled = (db + NOISE_FLOOR) * (255.0f / NOISE_FLOOR);
        if (scaled < 0.0f) scaled = 0.0f;
        if (scaled > 255.0f) scaled = 255.0f;
        out->bin[b] = (uint8_t)scaled;
        if (b > 0 && scaled > dominant_scaled) {
            dominant_scaled = scaled;
            dominant_band = (uint8_t)b;
        }
    }

    uint16_t dominant_start = BIN_EDGES[dominant_band];
    uint16_t dominant_stop = BIN_EDGES[dominant_band + 1] - 1;
    float dominant_center = ((float)dominant_start + (float)dominant_stop) * 0.5f;
    out->fft_major_peak = dominant_center * (float)AUDIO_IN_SAMPLE_RATE / (float)FFT_SIZE;
    out->fft_magnitude = fminf(dominant_scaled * 8.0f, 1020.0f);

    s_smooth_level = s_smooth_level * (1.0f - SMOOTH_ALPHA) + out->sample_raw * SMOOTH_ALPHA;
    out->sample_smooth = s_smooth_level;

    s_peak_level *= PEAK_DECAY;
    out->sample_peak = (out->sample_raw > s_peak_level * PEAK_THRESHOLD) && (out->sample_raw > 8.0f);
    if (out->sample_raw > s_peak_level) s_peak_level = out->sample_raw;

    out->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void on_audio_frame(const audio_frame_t *frame, void *user)
{
    (void)user;
    if (!s_ready || s_sub_count == 0) return;

    audio_fft_result_t result;
    compute_fft(frame->samples, &result);

    for (uint8_t i = 0; i < s_sub_count; i++) {
        s_subs[i].cb(&result, s_subs[i].user);
    }
}

esp_err_t audio_fft_start(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = audio_fft_alloc_buffers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT PSRAM buffer allocation failed: %s", esp_err_to_name(err));
        return err;
    }

    err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FFT table init failed: %s", esp_err_to_name(err));
        return err;
    }

    dsps_wind_hann_f32(s_window, FFT_SIZE);

    err = audio_in_subscribe(on_audio_frame, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to audio_in: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "Audio FFT ready: %d-point, %d bins, ~%d Hz frame rate",
             FFT_SIZE, FFT_BINS, AUDIO_IN_SAMPLE_RATE / FFT_SIZE);
    return ESP_OK;
}
