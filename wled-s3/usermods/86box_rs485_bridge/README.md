# 86Box RS-485 Bridge Usermod

This usermod is the first pass at the ESP32-S3 side of the P4 wall display link.

It reads newline-delimited JSON from RS-485 and forwards it into WLED's existing JSON APIs:

- `{"v":true}` sends a compact `state` + `info` snapshot plus a root `presets` list back to the P4.
- State JSON such as `{"on":true,"bri":128,"seg":[...]}` is applied with WLED's state deserializer.
- Preset, playlist, and reboot JSON such as `{"ps":1}`, `{"psave":1,"n":"Evening"}`, `{"pdel":1}`, `{"np":true}`, `{"playlist":{...}}`, and `{"rb":true}` is also applied with WLED's state deserializer.
- Config JSON such as `{"nw":...,"id":...,"if":...,"um":...}` is applied with WLED's config deserializer when available.

The P4 poll stream is treated as the link heartbeat. Once the bridge sees valid P4 JSON, it becomes request/response only and stops sending unsolicited periodic snapshots, which avoids half-duplex collisions with the P4's regular `{"v":true}` polls. If the P4 goes stale, the bridge falls back to quiet-bus reconnect announcements every 15 seconds. If the bridge boots but never sees an initial P4 frame, it periodically restarts the RS-485 UART/DE hardware instead of waiting forever for a USB-serial monitor reset. The bridge also drops stale partial lines, holds oversized lines until newline before reporting one error, and adds a small DE settle/hold delay around each transmit.

Preset readback uses WLED's saved preset names and returns compact `[id,name]` entries. Deleted presets are omitted, so the P4 Presets tab only shows panels for presets that actually exist in WLED.

See [P4_PARAMETER_CONTRACT.md](../../P4_PARAMETER_CONTRACT.md) for the complete P4-controlled parameter list.

Default pins and power limits in the sample PlatformIO override target the Waveshare ESP32-S3-Relay-1CH board:

- UART: `1`
- RS-485 TX: GPIO17
- RS-485 RX: GPIO18
- RS-485 EN/DE: GPIO21
- Baud: `115200`
- Onboard PSU relay: GPIO47, controlled by this usermod. Leave WLED's native Relay GPIO at `-1`; configure the PSU relay from the S3 WLED Usermods page under `86Box RS485 Bridge`.
- LED data: GPIO1 on the SH1.0 connector by default; set LED GPIO, count, type, color order, and current limit on the S3 WLED page.
- Audio Reactive: enabled with `UM_AUDIOREACTIVE_ENABLE`.
- Audio input: network receive only with `SR_DMTYPE=254`; the P4 sends WLED-MM audio-sync packets over UDP.
- Auto brightness limiter: `ABL_MILLIAMPS_DEFAULT=5000` is the first-boot default; change it on the S3 WLED page to match the real PSU.

## PSU Relay Logic

The Waveshare relay is intended to switch the LED power supply, so the usermod owns it instead of relying on WLED's basic relay GPIO. The defaults are conservative:

- Pin: GPIO47.
- Active level: active-high.
- Power-on lead: 750 ms before a P4 `on`, positive `bri`, or preset command is applied.
- Power-off hold: 10 seconds after WLED brightness/output reaches off.
- Minimum cycle time: 1.5 seconds between relay transitions.

The relay also follows WLED state changes made from the WLED web UI, but the P4 RS-485 path gets the strongest protection because the bridge can energize the PSU before applying the incoming LED-on JSON.

The P4 side uses an auto-direction RS-485 transceiver. Waveshare's product page describes RS-485 direction control as handled by main-controller hardware flow settings, and the provided pinout labels GPIO21 as RS485 EN. The sample therefore drives GPIO21 manually; remove `USERMOD_86BOX_RS485_DE=21` only if hardware testing proves the board works reliably without it.