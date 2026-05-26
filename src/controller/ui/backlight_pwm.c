#include "backlight_pwm.h"
#include "display_bsp.h"

static uint8_t s_current_percent = 60;

esp_err_t backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    esp_err_t err = display_bsp_set_backlight(percent);
    if (err == ESP_OK) s_current_percent = percent;
    return err;
}

uint8_t backlight_get(void)
{
    return s_current_percent;
}
