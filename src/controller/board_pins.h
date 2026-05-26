/*
 * board_pins.h
 *
 * Pin map for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (Smart 86 Box).
 *
 * Source:
 *   - Waveshare wiki: https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4B
 *   - Waveshare demo zip "displays/" + "drivers/" for LCD reset / backlight /
 *     touch reset & INT pin numbers (these go through an on-board IO
 *     expander, NOT direct GPIOs — see "IO expander" section below).
 *
 * Phase status: pins listed below are CONFIRMED from the wiki for the
 * 86-Panel-ETH-2RO variant we have. Pins marked TODO are still to be
 * cross-checked against the Waveshare demo zip during phase 1.5
 * (display + LVGL bring-up).
 */

#pragma once

#include "driver/gpio.h"

/* ---------- I2C bus 0 (SHARED: codec + touch + IO expander) -------------- */
/* All three devices (ES8311, ES7210, GT911 touch, CH422G/TCA9554 expander) */
/* live on this single bus. Use the new i2c_master driver with a shared bus */
/* handle (esp_codec_dev + esp_lcd_touch_gt911 both accept one).            */
#define BSP_I2C_NUM             0
#define BSP_I2C_SDA             GPIO_NUM_7
#define BSP_I2C_SCL             GPIO_NUM_8
#define BSP_I2C_FREQ_HZ         400000

/* ---------- Audio (ES8311 codec + ES7210 AEC mic front-end) -------------- */
#define BSP_I2S_NUM             0
#define BSP_I2S_MCLK            GPIO_NUM_13
#define BSP_I2S_SCLK            GPIO_NUM_12
#define BSP_I2S_LCLK            GPIO_NUM_10   /* LRCK / WS */
#define BSP_I2S_ASDOUT          GPIO_NUM_11   /* ES7210 -> P4 (mic data in) */
#define BSP_I2S_DSDIN           GPIO_NUM_9    /* P4 -> ES8311 (speaker out) */
#define BSP_POWER_AMP_IO        GPIO_NUM_53   /* PA_Ctrl: speaker amp enable */

/* ES7210 mic capture rate for WLED Sound Sync (see audio_in.c) */
#define BSP_AUDIO_SAMPLE_RATE   16000
#define BSP_AUDIO_CHANNELS      1

/* ---------- MIPI-DSI display (4" 720x720 IPS) ---------------------------- */
/* MIPI-DSI uses dedicated D-PHY pins on the P4 — no GPIO numbers to set    */
/* here; the esp_lcd_mipi_dsi driver claims the PHY directly.               */
/* Backlight + panel reset are on the IO expander (verify in demo zip).     */
#define BSP_LCD_H_RES                   720
#define BSP_LCD_V_RES                   720
#define BSP_LCD_BIT_DEPTH               16    /* RGB565 in PSRAM framebuf  */
#define BSP_LCD_DSI_LANES               2     /* D-PHY 2-lane              */
#define BSP_LCD_DSI_LANE_BITRATE_MBPS   1500  /* per lane                  */

/* IO-expander-driven (set by display_bsp via the expander, not direct GPIO) */
/* TODO phase 1.5: read exact bit positions from Waveshare demo zip.        */
#define BSP_LCD_BACKLIGHT_EXP_BIT       0     /* placeholder */
#define BSP_LCD_RST_EXP_BIT             1     /* placeholder */
#define BSP_TOUCH_RST_EXP_BIT           2     /* placeholder */

/* ---------- Capacitive touch (assumed GT911 on bus 0) -------------------- */
/* Confirm IC identity via i2c-scan in phase 1.5 (could be FT-series).      */
#define BSP_TOUCH_I2C_ADDR_GT911_PRI    0x5D
#define BSP_TOUCH_I2C_ADDR_GT911_SEC    0x14
#define BSP_TOUCH_INT                   GPIO_NUM_NC /* TODO phase 1.5 */

/* ---------- IO expander (CH422G or TCA9554) ------------------------------ */
/* Owns: LCD backlight enable, LCD reset, touch reset, possibly other       */
/* board-control signals. Driver: 'esp_io_expander_tca9554' or 'ch422g'.    */
#define BSP_IO_EXPANDER_I2C_ADDR        0x24  /* CH422G default; verify    */

/* ---------- RS-485 (auto-direction transceiver on P4 side) --------------- */
/* No DE pin on P4 — wiki: "Automatic data transmission and reception".     */
/* LDO VO4 must be set to 3.3V before use (see display_bsp_init).           */
#define BSP_RS485_UART_NUM              1
#define BSP_RS485_TX                    GPIO_NUM_47
#define BSP_RS485_RX                    GPIO_NUM_48
#define BSP_RS485_BAUD                  115200

/* ---------- Panel backlight PWM (via IO expander OR direct LEDC) --------- */
/* If the demo zip drives BL via the IO expander only (digital on/off),     */
/* we'll need a software-PWM workaround for adaptive brightness. If there's */
/* a direct LEDC-capable GPIO, plug it here.                                */
#define BSP_LCD_BACKLIGHT_LEDC_CH       LEDC_CHANNEL_0
#define BSP_LCD_BACKLIGHT_LEDC_TIMER    LEDC_TIMER_0
#define BSP_LCD_BACKLIGHT_FREQ_HZ       5000

/* ---------- microSD (SDIO 3.0) ------------------------------------------- */
#define BSP_SD_CLK              GPIO_NUM_43
#define BSP_SD_CMD              GPIO_NUM_44
#define BSP_SD_D0               GPIO_NUM_39
#define BSP_SD_D1               GPIO_NUM_40
#define BSP_SD_D2               GPIO_NUM_41
#define BSP_SD_D3               GPIO_NUM_42

/* ---------- Ethernet (IP101 RMII PHY) ------------------------------------ */
#define BSP_ETH_PHY_RST         GPIO_NUM_NC
#define BSP_ETH_MDC             GPIO_NUM_31
#define BSP_ETH_MDIO            GPIO_NUM_52

/* ---------- ESP32-C6 Wi-Fi/BT co-processor (SDIO) ------------------------ */
/* esp_wifi_remote + esp_hosted talk to the on-board C6 over this bus.      */
#define BSP_C6_SDIO_CLK         GPIO_NUM_18
#define BSP_C6_SDIO_CMD         GPIO_NUM_19
#define BSP_C6_SDIO_D0          GPIO_NUM_14
#define BSP_C6_SDIO_D1          GPIO_NUM_15
#define BSP_C6_SDIO_D2          GPIO_NUM_16
#define BSP_C6_SDIO_D3          GPIO_NUM_17
#define BSP_C6_RST              GPIO_NUM_54

/* ---------- USB 2.0 OTG HS (Type-C #11) ---------------------------------- */
/* Native HS PHY - no GPIO assignment required. */

/* ---------- Buttons ------------------------------------------------------ */
#define BSP_BOOT_BUTTON         GPIO_NUM_35

/* ---------- Unused board relays on the 86-Panel (NOT used for LED PSU) -- */
#define BSP_BOARD_RELAY_1       GPIO_NUM_32
#define BSP_BOARD_RELAY_2       GPIO_NUM_46
