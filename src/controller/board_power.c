#include "board_power.h"

#include "esp_ldo_regulator.h"

#define BOARD_POWER_VO4_LDO_CHAN 4
#define BOARD_POWER_VO4_MV       3300

esp_err_t board_power_enable_vo4_3v3(void)
{
    static esp_ldo_channel_handle_t s_vo4_ldo;
    if (s_vo4_ldo) return ESP_OK;

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BOARD_POWER_VO4_LDO_CHAN,
        .voltage_mv = BOARD_POWER_VO4_MV,
    };
    return esp_ldo_acquire_channel(&ldo_cfg, &s_vo4_ldo);
}