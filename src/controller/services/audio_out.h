#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_OUT_DIR            "/sdcard/audio"
#define AUDIO_OUT_PATH_MAX       128
#define AUDIO_OUT_NAME_MAX       64
#define AUDIO_OUT_MAX_LISTED     36
#define AUDIO_OUT_DEFAULT_VOLUME 70

typedef struct {
    char name[AUDIO_OUT_NAME_MAX];
    char path[AUDIO_OUT_PATH_MAX];
} audio_out_file_t;

esp_err_t audio_out_init(void);
esp_err_t audio_out_play_wav(const char *path, uint8_t volume_pct);
esp_err_t audio_out_play_chime(uint8_t volume_pct);
esp_err_t audio_out_stop(void);
esp_err_t audio_out_list_wav(audio_out_file_t *files, size_t max_files, size_t *file_count);
bool audio_out_is_ready(void);
bool audio_out_is_playing(void);
const char *audio_out_current_path(void);
const char *audio_out_last_error(void);

#ifdef __cplusplus
}
#endif