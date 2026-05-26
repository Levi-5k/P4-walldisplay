#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOUND_SYNC_PORT     11988
#define SOUND_SYNC_GROUP    1

esp_err_t sound_sync_tx_start(void);
bool      sound_sync_tx_is_ready(void);

#ifdef __cplusplus
}
#endif
