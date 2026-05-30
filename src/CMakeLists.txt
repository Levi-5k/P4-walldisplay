# Main component for the P4 controller firmware.
#
# Layout (see plan v6 repo-layout section):
#   controller/main.c
#   controller/board_pins.h
#   controller/ui/        — LVGL screens + display BSP + backlight + idle mgr
#   controller/weather/   — OpenWeatherMap client + cached weather state
#   controller/services/  — cross-cutting helpers (NVS wrappers, etc.)

idf_component_register(
    SRCS
        "controller/main.c"
        "controller/board_power.c"
        "controller/ui/display_bsp.c"
        "controller/ui/ui_init.c"
        "controller/ui/theme.c"
        "controller/ui/wled_effect_catalog.c"
        "controller/ui/ui_background.c"
        "controller/ui/jpeg_conv.c"
        "controller/ui/jpeg_hw.c"
        "controller/ui/screens.c"
        "controller/ui/screen_timer.c"
        "controller/ui/idle_manager.c"
        "controller/ui/backlight_pwm.c"
        "controller/ui/backlight_manager.c"
        "controller/ui/status_bar.c"
        "controller/ui/nav_bar.c"
        "controller/ui/toast.c"
        "controller/services/services.c"
        "controller/services/app_config.c"
        "controller/services/sd_storage.c"
        "controller/services/serial_debug.c"
        "controller/services/led_state.c"
        "controller/services/audio_bus.c"
        "controller/services/audio_in.c"
        "controller/services/audio_fft.c"
        "controller/services/audio_out.c"
        "controller/services/audio_library.c"
        "controller/services/sound_sync_tx.c"
        "controller/services/wled_state.c"
        "controller/services/slave_ota.c"
        "controller/weather/weather_state.c"
        "controller/weather/weather_history.c"
        "controller/weather/weather_api.c"
        "controller/ui/fonts/weather_icons_28.c"
        "controller/ui/fonts/weather_icons_60.c"
        "controller/ui/fonts/weather_icons_96.c"
        "controller/ui/fonts/weather_icons_120.c"
        "controller/ui/fonts/weather_digits_72.c"
        "controller/ui/fonts/weather_digits_96.c"
        "controller/ui/fonts/weather_digits_150.c"
    INCLUDE_DIRS
        "controller"
        "controller/ui"
        "controller/ui/fonts"
        "controller/weather"
        "controller/services"
    PRIV_REQUIRES
        app_update
        esp_lcd
        esp_partition
        esp_psram
        esp_driver_jpeg
        nvs_flash
        driver
        esp_event
        esp_http_client
        esp_netif
        esp_timer
        fatfs
        espressif__esp_hosted
        espressif__esp_wifi_remote
        espressif__esp_codec_dev
        espressif__esp-dsp
        json
        sdmmc
        waveshare__esp32_p4_wifi6_touch_lcd_4b
        lvgl__lvgl
        espressif__esp_lvgl_port
)
