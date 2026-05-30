#include "audio_out.h"

#include "audio_bus.h"
#include "sd_storage.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "audio_out";

#define AUDIO_OUT_TASK_STACK    6144
#define AUDIO_OUT_TASK_PRIO     5
#define AUDIO_OUT_BUFFER_BYTES  1024
#define AUDIO_OUT_CHIME_SAMPLES 256
#define AUDIO_OUT_MAX_WAV_RATE  96000

typedef enum {
    AUDIO_OUT_CMD_PLAY_WAV,
    AUDIO_OUT_CMD_PLAY_CHIME,
    AUDIO_OUT_CMD_STOP,
} audio_out_cmd_type_t;

typedef struct {
    audio_out_cmd_type_t type;
    char path[AUDIO_OUT_PATH_MAX];
    uint8_t volume_pct;
} audio_out_cmd_t;

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} wav_info_t;

static esp_codec_dev_handle_t s_spk_dev;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task;
static volatile bool s_ready;
static volatile bool s_playing;
static char s_current_path[AUDIO_OUT_PATH_MAX];
static char s_last_error[128] = "not initialized";

static void audio_out_set_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, args);
    va_end(args);
}

static bool audio_out_has_wav_extension(const char *name)
{
    if (!name) return false;
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".wav") == 0;
}

static int audio_out_file_compare(const void *a, const void *b)
{
    const audio_out_file_t *fa = (const audio_out_file_t *)a;
    const audio_out_file_t *fb = (const audio_out_file_t *)b;
    return strcasecmp(fa->name, fb->name);
}

static void *audio_out_alloc_buffer(size_t bytes)
{
    void *ptr = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ptr) ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = malloc(bytes);
    return ptr;
}

esp_err_t audio_out_list_wav(audio_out_file_t *files, size_t max_files, size_t *file_count)
{
    if (!file_count) return ESP_ERR_INVALID_ARG;
    *file_count = 0;
    if (max_files && !files) return ESP_ERR_INVALID_ARG;

    esp_err_t err = sd_storage_ensure_dir(AUDIO_OUT_DIR);
    if (err != ESP_OK) {
        audio_out_set_error("audio list: %s", esp_err_to_name(err));
        return err;
    }

    DIR *dir = opendir(AUDIO_OUT_DIR);
    if (!dir) {
        audio_out_set_error("open %s failed: errno %d", AUDIO_OUT_DIR, errno);
        return ESP_FAIL;
    }

    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!audio_out_has_wav_extension(name)) continue;

        char path[AUDIO_OUT_PATH_MAX];
        int written = snprintf(path, sizeof(path), "%s/%s", AUDIO_OUT_DIR, name);
        if (written <= 0 || written >= (int)sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        if (count < max_files) {
            snprintf(files[count].name, sizeof(files[count].name), "%s", name);
            snprintf(files[count].path, sizeof(files[count].path), "%s", path);
        }
        count++;
    }
    closedir(dir);

    size_t returned = count < max_files ? count : max_files;
    if (files && returned > 1) qsort(files, returned, sizeof(files[0]), audio_out_file_compare);
    *file_count = returned;
    return ESP_OK;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t wav_read_info(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) return ESP_ERR_INVALID_SIZE;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_fmt = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    memset(info, 0, sizeof(*info));

    while (!have_data) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
        uint32_t chunk_size = le32(chunk + 4);
        long chunk_data_pos = ftell(file);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            size_t to_read = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
            if (to_read < 16 || fread(fmt, 1, to_read, file) != to_read) return ESP_ERR_INVALID_SIZE;
            audio_format = le16(fmt);
            info->channels = le16(fmt + 2);
            info->sample_rate = le32(fmt + 4);
            info->bits_per_sample = le16(fmt + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = (uint32_t)chunk_data_pos;
            info->data_size = chunk_size;
            have_data = true;
        }

        if (!have_data) {
            long next_pos = chunk_data_pos + (long)chunk_size + (long)(chunk_size & 1u);
            if (fseek(file, next_pos, SEEK_SET) != 0) return ESP_FAIL;
        }
    }

    if (!have_fmt || !have_data) return ESP_ERR_NOT_FOUND;
    if (audio_format != 1 || info->bits_per_sample != 16) return ESP_ERR_NOT_SUPPORTED;
    if (info->channels != 1 && info->channels != 2) return ESP_ERR_NOT_SUPPORTED;
    if (info->sample_rate < AUDIO_BUS_SAMPLE_RATE || info->sample_rate > AUDIO_OUT_MAX_WAV_RATE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fseek(file, (long)info->data_offset, SEEK_SET) != 0) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t audio_out_open_codec(uint8_t volume_pct)
{
    if (!s_spk_dev) return ESP_ERR_INVALID_STATE;
    if (volume_pct > 100) volume_pct = 100;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_BUS_SAMPLE_RATE,
        .channel = AUDIO_BUS_CHANNELS,
        .bits_per_sample = AUDIO_BUS_BITS_PER_SAMPLE,
    };

    int ret = esp_codec_dev_set_out_vol(s_spk_dev, volume_pct);
    if (ret != 0) {
        audio_out_set_error("set volume failed: %d", ret);
        return ESP_FAIL;
    }
    ret = esp_codec_dev_open(s_spk_dev, &fs);
    if (ret != 0) {
        audio_out_set_error("open speaker codec failed: %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool audio_out_next_command(audio_out_cmd_t *pending)
{
    return s_cmd_queue && pending && xQueueReceive(s_cmd_queue, pending, 0) == pdTRUE;
}

static esp_err_t audio_out_play_wav_blocking(const audio_out_cmd_t *cmd, audio_out_cmd_t *pending, bool *have_pending)
{
    *have_pending = false;
    esp_err_t err = sd_storage_ensure_mounted();
    if (err != ESP_OK) {
        audio_out_set_error("SD unavailable: %s", esp_err_to_name(err));
        return err;
    }

    FILE *file = fopen(cmd->path, "rb");
    if (!file) {
        audio_out_set_error("open %s failed: errno %d", cmd->path, errno);
        return ESP_FAIL;
    }

    wav_info_t info;
    err = wav_read_info(file, &info);
    if (err != ESP_OK) {
        audio_out_set_error("unsupported WAV %s: %s", cmd->path, esp_err_to_name(err));
        fclose(file);
        return err;
    }

    int16_t *read_buf = (int16_t *)audio_out_alloc_buffer(AUDIO_OUT_BUFFER_BYTES);
    int16_t *mono_buf = (int16_t *)audio_out_alloc_buffer(AUDIO_OUT_BUFFER_BYTES / 2);
    if (!read_buf || !mono_buf) {
        free(read_buf);
        free(mono_buf);
        fclose(file);
        audio_out_set_error("audio buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    err = audio_out_open_codec(cmd->volume_pct);
    if (err != ESP_OK) {
        free(read_buf);
        free(mono_buf);
        fclose(file);
        return err;
    }

    s_playing = true;
    snprintf(s_current_path, sizeof(s_current_path), "%s", cmd->path);
    ESP_LOGI(TAG, "Playing %s (%lu Hz -> %d Hz, %u ch, %u bit)", cmd->path,
             (unsigned long)info.sample_rate, AUDIO_BUS_SAMPLE_RATE,
             (unsigned)info.channels, (unsigned)info.bits_per_sample);

    const bool resample = info.sample_rate != AUDIO_BUS_SAMPLE_RATE;
    const uint32_t step_q16 = resample
        ? (uint32_t)(((uint64_t)info.sample_rate << 16) / AUDIO_BUS_SAMPLE_RATE)
        : (1u << 16);
    uint32_t src_pos_q16 = 0;

    uint32_t remaining = info.data_size;
    while (remaining > 0) {
        if (audio_out_next_command(pending)) {
            *have_pending = true;
            break;
        }

        uint32_t max_read = info.channels == 2 ? AUDIO_OUT_BUFFER_BYTES : AUDIO_OUT_BUFFER_BYTES / 2;
        if (max_read > remaining) max_read = remaining;
        max_read -= max_read % (info.channels * sizeof(int16_t));
        if (max_read == 0) break;

        size_t got = fread(read_buf, 1, max_read, file);
        if (got == 0) break;
        remaining -= (uint32_t)got;

        size_t in_frames = got / (info.channels * sizeof(int16_t));
        int16_t *mono_samples = read_buf;
        if (info.channels == 2) {
            for (size_t i = 0; i < in_frames; i++) {
                int32_t left = read_buf[i * 2];
                int32_t right = read_buf[i * 2 + 1];
                mono_buf[i] = (int16_t)((left + right) / 2);
            }
            mono_samples = mono_buf;
        }

        int16_t *out_buf = mono_samples;
        size_t out_frames = in_frames;
        if (resample) {
            out_buf = mono_buf;
            out_frames = 0;
            while ((src_pos_q16 >> 16) < in_frames && out_frames < (AUDIO_OUT_BUFFER_BYTES / 2 / sizeof(int16_t))) {
                out_buf[out_frames++] = mono_samples[src_pos_q16 >> 16];
                src_pos_q16 += step_q16;
            }
            src_pos_q16 -= (uint32_t)in_frames << 16;
        }

        size_t out_bytes = out_frames * sizeof(int16_t);
        if (out_bytes == 0) continue;

        int ret = esp_codec_dev_write(s_spk_dev, out_buf, (int)out_bytes);
        if (ret != 0) {
            audio_out_set_error("speaker write failed: %d", ret);
            err = ESP_FAIL;
            break;
        }
    }

    esp_codec_dev_close(s_spk_dev);
    fclose(file);
    free(read_buf);
    free(mono_buf);
    s_playing = false;
    s_current_path[0] = '\0';
    if (err == ESP_OK && !*have_pending) audio_out_set_error("last played %s", cmd->path);
    return err;
}

static esp_err_t audio_out_play_chime_blocking(const audio_out_cmd_t *cmd, audio_out_cmd_t *pending, bool *have_pending)
{
    *have_pending = false;
    esp_err_t err = audio_out_open_codec(cmd->volume_pct);
    if (err != ESP_OK) return err;

    int16_t samples[AUDIO_OUT_CHIME_SAMPLES];
    const float notes[] = { 880.0f, 1174.66f, 1567.98f };
    const uint32_t note_samples = AUDIO_BUS_SAMPLE_RATE / 5;
    const float two_pi = 6.28318530718f;

    s_playing = true;
    snprintf(s_current_path, sizeof(s_current_path), "built-in chime");
    ESP_LOGI(TAG, "Playing built-in chime");

    for (size_t note = 0; note < sizeof(notes) / sizeof(notes[0]); note++) {
        for (uint32_t offset = 0; offset < note_samples;) {
            if (audio_out_next_command(pending)) {
                *have_pending = true;
                esp_codec_dev_close(s_spk_dev);
                s_playing = false;
                s_current_path[0] = '\0';
                return ESP_OK;
            }
            uint32_t batch = note_samples - offset;
            if (batch > AUDIO_OUT_CHIME_SAMPLES) batch = AUDIO_OUT_CHIME_SAMPLES;
            for (uint32_t i = 0; i < batch; i++) {
                float phase = two_pi * notes[note] * (float)(offset + i) / (float)AUDIO_BUS_SAMPLE_RATE;
                float fade = 1.0f;
                if (offset + i < 160) fade = (float)(offset + i) / 160.0f;
                if (note_samples - (offset + i) < 320) fade = (float)(note_samples - (offset + i)) / 320.0f;
                samples[i] = (int16_t)(sinf(phase) * fade * 12000.0f);
            }
            int ret = esp_codec_dev_write(s_spk_dev, samples, (int)(batch * sizeof(int16_t)));
            if (ret != 0) {
                audio_out_set_error("chime write failed: %d", ret);
                esp_codec_dev_close(s_spk_dev);
                s_playing = false;
                s_current_path[0] = '\0';
                return ESP_FAIL;
            }
            offset += batch;
        }
    }

    esp_codec_dev_close(s_spk_dev);
    s_playing = false;
    s_current_path[0] = '\0';
    audio_out_set_error("last played built-in chime");
    return ESP_OK;
}

static void audio_out_task(void *arg)
{
    (void)arg;
    audio_out_cmd_t cmd;
    audio_out_cmd_t pending;
    bool have_pending = false;

    while (1) {
        if (have_pending) {
            cmd = pending;
            have_pending = false;
        } else if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd.type == AUDIO_OUT_CMD_STOP) {
            audio_out_set_error("stopped");
            continue;
        }
        if (cmd.type == AUDIO_OUT_CMD_PLAY_CHIME) {
            (void)audio_out_play_chime_blocking(&cmd, &pending, &have_pending);
        } else if (cmd.type == AUDIO_OUT_CMD_PLAY_WAV) {
            (void)audio_out_play_wav_blocking(&cmd, &pending, &have_pending);
        }
    }
}

esp_err_t audio_out_init(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = audio_bus_init();
    if (err != ESP_OK) {
        audio_out_set_error("audio bus unavailable: %s", audio_bus_last_error());
        ESP_LOGE(TAG, "%s", s_last_error);
        return err;
    }

    const audio_codec_data_if_t *data_if = audio_bus_data_if();
    if (!data_if) return ESP_ERR_INVALID_STATE;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) {
        audio_out_set_error("audio_codec_new_gpio failed");
        return ESP_ERR_NO_MEM;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl_if) {
        audio_out_set_error("audio_codec_new_i2c_ctrl failed");
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (!es8311_dev) {
        audio_out_set_error("es8311_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = data_if,
    };
    s_spk_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (!s_spk_dev) {
        audio_out_set_error("esp_codec_dev_new(speaker) failed");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(1, sizeof(audio_out_cmd_t));
    if (!s_cmd_queue) {
        audio_out_set_error("audio command queue allocation failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(audio_out_task, "audio_out", AUDIO_OUT_TASK_STACK,
                                NULL, AUDIO_OUT_TASK_PRIO, &s_task, 0) != pdPASS) {
        s_task = NULL;
        audio_out_set_error("audio output task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    audio_out_set_error("ready");
    ESP_LOGI(TAG, "ES8311 speaker ready: %d Hz mono WAV/chime playback", AUDIO_BUS_SAMPLE_RATE);
    return ESP_OK;
}

static esp_err_t audio_out_send(audio_out_cmd_t *cmd)
{
    esp_err_t err = audio_out_init();
    if (err != ESP_OK) return err;
    if (cmd->volume_pct > 100) cmd->volume_pct = 100;
    if (xQueueOverwrite(s_cmd_queue, cmd) != pdPASS) {
        audio_out_set_error("audio command queue send failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_out_play_wav(const char *path, uint8_t volume_pct)
{
    if (!path || !path[0]) return ESP_ERR_INVALID_ARG;
    if (strncmp(path, "/sdcard/", 8) != 0) return ESP_ERR_INVALID_ARG;

    audio_out_cmd_t cmd = {
        .type = AUDIO_OUT_CMD_PLAY_WAV,
        .volume_pct = volume_pct,
    };
    snprintf(cmd.path, sizeof(cmd.path), "%s", path);
    return audio_out_send(&cmd);
}

esp_err_t audio_out_play_chime(uint8_t volume_pct)
{
    audio_out_cmd_t cmd = {
        .type = AUDIO_OUT_CMD_PLAY_CHIME,
        .volume_pct = volume_pct,
    };
    return audio_out_send(&cmd);
}

esp_err_t audio_out_stop(void)
{
    if (!s_cmd_queue) return ESP_OK;
    audio_out_cmd_t cmd = { .type = AUDIO_OUT_CMD_STOP };
    if (xQueueOverwrite(s_cmd_queue, &cmd) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}

bool audio_out_is_ready(void)
{
    return s_ready;
}

bool audio_out_is_playing(void)
{
    return s_playing;
}

const char *audio_out_current_path(void)
{
    return s_current_path;
}

const char *audio_out_last_error(void)
{
    return s_last_error;
}