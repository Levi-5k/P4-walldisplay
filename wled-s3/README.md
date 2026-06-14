# ESP32-S3 WLED Overlay

This folder is the starter overlay for the modified ESP32-S3 WLED node that pairs with the P4 wall display.

The current target board is Waveshare `ESP32-S3-Relay-1CH` (SKU `32152`, Part No. `ESP32-S3-Relay-1CH`). The official page lists a default `ESP32-S3R8` module and `16MB Flash`; the provided pinout maps RS-485 to GPIO17/GPIO18/GPIO21 and the onboard relay to GPIO47.

This branch targets official WLED `v16.0.0` as a non-audio-reactive LED node. The P4 firmware still expects the LED node to behave like WLED:

- RS-485 UART at 115200 baud, newline-delimited JSON.
- WLED state commands such as `{"on":true,"bri":128,"seg":[...]}`.
- Poll command `{"v":true}` returning a JSON object with `state` and `info`.
- mDNS name `wled-86box` and display name `86Box LED`.
- No WLED-MM/Sound Reactive UDP audio sync; this branch intentionally omits AudioReactive.

The complete P4-controlled parameter list is documented in [P4_PARAMETER_CONTRACT.md](P4_PARAMETER_CONTRACT.md).

Use this overlay with a separate official WLED checkout at tag `v16.0.0`. This is the non-audio branch; use the `alpha-next` branch's MoonModules overlay when AudioReactive/WLED-MM packet support is required.

## Layout

```text
wled-s3/
  boards/waveshare_esp32s3_relay_1ch.json
  P4_PARAMETER_CONTRACT.md
  platformio_override.sample.ini
  pio-scripts/86box_esptool_compat.py
  scripts/apply_overlay.sh
  usermods/86box_rs485_bridge/
    library.json
    usermod_86box_rs485_bridge.cpp
    usermod_86box_rs485_bridge.h
```

## First Build Shape

1. Clone official WLED outside the P4 firmware source, or into `wled-s3/upstream/`, and check out tag `v16.0.0` or the local `wled-16-non-audio` branch.
2. Copy `platformio_override.sample.ini` to the upstream root as `platformio_override.ini`.
3. Copy `boards/waveshare_esp32s3_relay_1ch.json` into upstream `boards/`, `usermods/86box_rs485_bridge/` into upstream `usermods/`, and `pio-scripts/86box_esptool_compat.py` into upstream `pio-scripts/`.
4. The 86Box bridge is registered through WLED 16's `custom_usermods` loader; no manual `wled00/usermods_list.cpp` edit is needed.
5. Adjust only board-level compile settings if the upstream env changes. LED pin/count/type/color order and LED PSU current limit are normal WLED web settings. The sample extends official WLED's `esp32s3dev_16MB_opi` env and overrides flash/partition settings for the Waveshare 16 MB board.
6. Build from the upstream checkout with `pio run -e S3_WLED_Host`.

You can run the helper from this directory with:

```sh
sh scripts/apply_overlay.sh ./upstream
```

The helper copies the overlay files, including a small esptool compatibility script for this PlatformIO toolchain. WLED 16 discovers the bridge through `custom_usermods = 86box_rs485_bridge` and the copied usermod library manifest.

## Protocol Notes

The bridge usermod is intentionally small. It reads one JSON object per line from RS-485, applies state JSON through WLED's JSON state API, applies config JSON through WLED's config API when available, and writes a compact state/info snapshot plus named preset list back to the P4. The snapshot is trimmed to stay under the P4 RS-485 receive line limit and includes small RS-485 counters for diagnostics.

The P4's `{"v":true}` poll is the normal heartbeat. After the S3 sees valid P4 traffic it only transmits in response to P4 requests/commands, which keeps the half-duplex bus from colliding with independent periodic snapshots. If the P4 link times out, the S3 sends low-rate reconnect announcements only after the bus has been quiet.

The bridge also keeps the S3 recoverable over RS-485 without manual USB resets: the UART is restarted if a previously-seen P4 link stays stale or if repeated malformed frames suggest the receive path is stuck. WLED requests a clean reboot only after repeated RS-485 UART resets fail to restore a previously working P4 link. The WLED `/json/info` usermod section reports this as `86Box RS485 Self-Heal`.

## Waveshare Pin Defaults

The sample override uses these board-specific defaults:

- RS-485 bridge UART1: TX GPIO17, RX GPIO18, enable GPIO21.
- LED data: GPIO1 on the SH1.0 connector by default. Change it later from the S3 WLED page if the strip is wired to GPIO2 or another free header pin.
- Onboard PSU relay: GPIO47, controlled by the 86Box RS-485 bridge usermod with a 750 ms power-on lead, 10 s power-off hold, and 1.5 s minimum cycle time by default. Leave WLED's native Relay GPIO disabled to avoid two relay controllers fighting over the LED power supply.
- Audio Reactive: disabled on this branch. The P4 may still emit WLED-MM UDP audio-sync packets, but official WLED 16 ignores them because the AudioReactive usermod is not compiled in.
- Flash/PSRAM: local board file declares 16 MB flash and OPI PSRAM support for the ESP32-S3R8-class board.

The P4 can already provision Wi-Fi and sound-sync settings over this link. LED strip GPIO, LED count, pixel type/color order, and LED PSU current limit are set on the S3 WLED page under Settings > LED Preferences. PSU relay pin, active level, power-on lead time, power-off hold time, and minimum cycle time are exposed on the S3 WLED Usermods page under `86Box RS485 Bridge`.
