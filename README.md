# P4-walldisplay

PlatformIO project for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B**
("Smart 86 Box" wall-mount development board).

* MCU: **ESP32-P4NRW32** – dual-core RISC-V HP @ 400 MHz + LP @ 40 MHz
* PSRAM: **32 MB** in-package (HEX, 200 MHz)
* Flash: **32 MB** external NOR (QIO, 80 MHz)
* Wi-Fi 6 / BT 5 LE: via on-board **ESP32-C6-MINI-1U-H8** (SDIO host)
* Display: **4" 720×720** IPS MIPI-DSI + capacitive touch
* Audio: **ES8311** codec + **ES7210** echo cancellation, dual mic, 8 Ω speaker
* Ethernet: 100 M **IP101** PHY
* USB 2.0 OTG HS, microSD (SDIO 3.0), RTC battery header, MIPI-CSI (4B variant)

Docs: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4B>

## Project layout

```
boards/waveshare_esp32p4_86box.json   custom PlatformIO board definition
platformio.ini                        build environment
partitions.csv                        32 MB flash partition table
sdkconfig.defaults                    ESP-IDF default config (PSRAM, flash, …)
CMakeLists.txt                        ESP-IDF top-level project
src/
  CMakeLists.txt                      main component
  main.c                              hello-world firmware
  board_pins.h                        pin map cheat-sheet
```

## Build & flash

```powershell
pio run                       # build
pio run -t upload             # flash via on-board USB-UART (port #12)
pio device monitor            # serial monitor @ 115200
```

The first build will download the ESP32-P4-capable Espressif platform
(`pioarduino/platform-espressif32@develop`) and ESP-IDF v5.3+, which are
required because ESP32-P4 support is not yet in the upstream stable platform.

## Wi-Fi / Bluetooth

The ESP32-P4 has no native radio. Wi-Fi 6 and Bluetooth 5 LE are provided by
the on-board ESP32-C6 module over SDIO. To use them, add the
[`esp_hosted`](https://github.com/espressif/esp-hosted) component and flash
the matching slave firmware to the C6 via the 4-pin header (#3 on the
silk-screen). Enable the related Kconfig options in
[sdkconfig.defaults](sdkconfig.defaults).

## Pin map

See [src/board_pins.h](src/board_pins.h). Always cross-check against the
latest schematic PDF on the Waveshare wiki before deploying hardware.
