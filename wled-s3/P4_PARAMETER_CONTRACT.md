# P4 to WLED-S3 Parameter Contract

This document lists every WLED-side parameter the P4 wall display currently needs to set or read on the ESP32-S3 LED node.

The selected S3 board is Waveshare `ESP32-S3-Relay-1CH`, SKU `32152`, Part No. `ESP32-S3-Relay-1CH`. Waveshare documents the default module as `ESP32-S3R8` and the product page lists `16MB Flash`. The overlay now targets that board shape with a local PlatformIO board file, 16 MB flash settings, OPI PSRAM support, onboard RS-485 pins, and the onboard relay controlled by the 86Box bridge usermod as the LED PSU relay.

## Current S3 Board Hardware

| Item | Waveshare ESP32-S3-Relay-1CH value | Overlay decision |
| --- | --- | --- |
| Microcontroller/module | ESP32-S3, default module `ESP32-S3R8` | Build assumes ESP32-S3 with 8 MB OPI PSRAM. |
| Flash | 16 MB | Custom board file plus 16 MB WLED partition/flash settings. |
| RS-485 TX | GPIO17 | `USERMOD_86BOX_RS485_TX=17`. |
| RS-485 RX | GPIO18 | `USERMOD_86BOX_RS485_RX=18`. |
| RS-485 enable | GPIO21 on the pinout image | `USERMOD_86BOX_RS485_DE=21`; remove only if bench testing proves hardware auto-direction is enough. |
| Relay control | GPIO47 on the pinout image | Controlled by the 86Box RS-485 bridge usermod as the LED PSU relay; WLED's native Relay GPIO should stay disabled. |
| LED data default | GPIO1 on the SH1.0 connector | Web-configurable in WLED LED Preferences; first-boot default is GPIO1. |
| Expansion pins | GPIO1/GPIO2 on SH1.0, plus header GPIO3..14 and others | Avoid GPIO17/18/21 for RS-485, GPIO47 for relay, and GPIO19/20 for USB. |
| USB | Type-C, GPIO20 D+ and GPIO19 D- | USB CDC on boot is enabled in the sample for direct USB serial logs. |
| Relay contact | 1 normally-open channel, contact rating <=10A 250VAC/30VDC | Hardware capability only; relay switching must be intentionally enabled and wired safely. |
| RS-485 termination | 120R matching resistor reserved, NC by default, enabled by jumper | Move jumper only if the RS-485 bus needs termination. |

## Transport Limits

- Physical link: RS-485 UART, 115200 baud, 8N1, newline-delimited JSON.
- P4 RS-485 pins: UART1, TX GPIO47, RX GPIO48.
- S3 RS-485 pins: UART1, TX GPIO17, RX GPIO18, DE/RE GPIO21.
- Waveshare's product page describes RS-485 direction as controlled by main-controller hardware flow settings; the pinout image labels GPIO21 as RS485 EN. The overlay drives GPIO21 manually through the bridge usermod.
- P4 TX command payload limit: less than 512 bytes, before the newline.
- P4 RX line limit: 16384 bytes. S3 snapshots must stay comfortably below this; the larger budget is for the named WLED preset list.
- S3 bridge RX line limit: 1536 bytes by default.
- Poll cadence: P4 sends `{"v":true}` every 5 seconds.
- Online timeout: P4 marks WLED stale after 30 seconds without a valid response.
- The S3 bridge now treats the P4 poll/request stream as the primary link heartbeat. After it sees a valid P4 JSON frame, it stops sending periodic unsolicited snapshots and only responds to P4 requests/commands, preventing half-duplex collisions with the 5-second poll cadence. If the P4 goes stale for 30 seconds, the bridge returns to low-rate reconnect announcements every 15 seconds, only after the RS-485 bus has been quiet. If the bridge boots but never receives a valid P4 frame, it restarts the RS-485 UART/DE hardware on a cold-start watchdog so communication can recover without opening a USB monitor.
- The S3 bridge drives RS-485 DE with a short settle/hold delay around each transmitted line and discards partial RX lines after 1.2 seconds of idle time. Oversized lines are dropped until newline so one bad frame cannot repeatedly collide with new traffic.

## State Commands From P4

These JSON keys must be accepted by the S3 bridge and forwarded into WLED's state deserializer.

| JSON key | Type/range | Current P4 source | Purpose |
| --- | --- | --- | --- |
| `on` | boolean | Lights power button and WLED reconciliation | Master LED output on/off. |
| `bri` | 0..255 | P4 brightness 0..100% converted to WLED scale | Master WLED brightness. |
| `transition` | WLED transition value; currently `7` | P4 LED state publisher | Fade time for power, brightness, and CCT updates. |
| `seg[0].id` | segment index; currently `0` | P4 CCT publisher | Targets segment 0 for color temperature updates. |
| `seg[0].cct` | 0..255 | P4 kelvin mapped through `kelvin_min..kelvin_max` | Segment color temperature. |
| `seg[0].fx` | 0..255 | Lights effect previous/next controls | Segment effect ID. |
| `seg[0].pal` | 0..255 | Lights palette previous/next controls | Segment palette ID. |
| `seg[0].sx` | 0..255 | Lights speed slider | Effect speed. |
| `seg[0].ix` | 0..255 | Lights intensity slider | Effect intensity. |
| `seg[0].c1` | 0..255 | Lights effect parameter controls | WLED effect custom slider 1. |
| `seg[0].c2` | 0..255 | Lights effect parameter controls | WLED effect custom slider 2. |
| `seg[0].c3` | 0..255 | Lights effect parameter controls | WLED effect custom slider 3. |
| `seg[0].o1` | boolean | Lights effect option controls | WLED effect option toggle 1. |
| `seg[0].o2` | boolean | Lights effect option controls | WLED effect option toggle 2. |
| `seg[0].o3` | boolean | Lights effect option controls | WLED effect option toggle 3. |
| `seg[0].col` | up to three RGB triples, each channel 0..255 | Lights primary and secondary color controls | Segment color slots; P4 currently edits `col[0]` and `col[1]` while preserving `col[2]` from readback. |
| `ps` | WLED preset ID; current UI sends `1..250` | Lights preset panels/command menu | Apply a saved WLED preset. |
| `psave` | WLED preset ID; current UI sends `1..250` | Lights preset panels/command menu | Save current WLED state into a preset slot. |
| `n` | string, used with `psave` | Lights preset panel name field | WLED preset name stored in `/presets.json`. |
| `pdel` | WLED preset ID; current UI sends `1..250` | Lights preset panel delete action/command menu | Delete a WLED preset slot. |
| `np` | boolean | Lights preset panels/command menu | Advance to WLED's next preset. |
| `playlist` | object with `ps`, `dur`, `transition`, `repeat`, `end` | Lights preset panels/command menu | Start a WLED playlist generated from selected preset options. |
| `rb` | boolean; current UI sends `true` | Settings > WLED > Reboot S3 | Reboot the WLED node. |

Example state update emitted by the P4 LED state publisher:

```json
{"on":true,"bri":128,"transition":7,"seg":[{"id":0,"cct":74}]}
```

Example direct controls from the Lights page:

```json
{"seg":[{"fx":42}]}
{"seg":[{"pal":5}]}
{"seg":[{"sx":180}]}
{"seg":[{"ix":96}]}
{"seg":[{"c1":64}]}
{"seg":[{"c2":128}]}
{"seg":[{"c3":16}]}
{"seg":[{"o1":true}]}
{"seg":[{"col":[[255,64,0],[0,64,255],[0,0,0]]}]}
{"ps":3}
{"psave":3,"n":"Evening","ib":true,"sb":true,"sc":true}
{"pdel":3}
{"np":true}
{"playlist":{"ps":[3,4],"dur":[100,100],"transition":[7,7],"repeat":0,"end":0}}
```

## Provisioning Config From P4

These JSON keys must be accepted by the S3 bridge and forwarded into WLED's config deserializer.

| JSON key | Type/range | Current P4 source | Purpose |
| --- | --- | --- | --- |
| `nw.ins[0].ssid` | string, up to 32 chars from P4 storage | P4 Wi-Fi settings | Wi-Fi network name for the S3 WLED node. |
| `nw.ins[0].psk` | string, up to 64 chars from P4 storage | P4 Wi-Fi settings | Wi-Fi password for the S3 WLED node. |
| `id.mdns` | string; currently `wled-86box` | P4 provisioning worker | mDNS name the P4 audio sender resolves as `wled-86box.local`. |
| `id.name` | string; currently `86Box LED` | P4 provisioning worker | Friendly WLED device name. |
| `if.sync.recv` | boolean; currently `true` | P4 provisioning worker | Enables WLED sync receive. |
| `if.sync.port` | UDP port; currently `11988` | P4 provisioning worker | WLED-MM sound sync receive port; ignored by the non-audio WLED 16 branch. |
| `if.sync.group` | integer; currently `1` | P4 provisioning worker | WLED-MM sync group; ignored by the non-audio WLED 16 branch. |

Current provisioning payload shape:

```json
{"nw":{"ins":[{"ssid":"<ssid>","psk":"<psk>"}]},"id":{"mdns":"wled-86box","name":"86Box LED"},"if":{"sync":{"recv":true,"port":11988,"group":1}}}
```

## Audio Sync From P4

Audio sync is UDP, not RS-485 JSON. On the normal MoonModules branch, the S3 firmware must use WLED-MM/MoonModules with `USERMOD_AUDIOREACTIVE` enabled.

The `wled-16-non-audio` branch intentionally targets official WLED `v16.0.0` without AudioReactive. RS-485 control, presets, and bridge diagnostics still apply, but P4 audio-sync packets are ignored by that firmware.

| Parameter | Current value | Purpose |
| --- | --- | --- |
| Destination host | `wled-86box.local`, fallback broadcast | Target for P4 audio-sync packets. |
| UDP port | `11988` | WLED-MM audio-sync receive port. |
| Sync group | `1` | Must match WLED sync config. |
| Send interval | at least 22 ms between packets | About 43 packets per second. |
| Packet format | 44-byte WLED-MM v2 `audioSyncPacket` | Header `"00002"`, optional pressure/zero-crossing fields, raw/smoothed sample, frame counter, peak flag, 16 FFT bins, magnitude, and major peak. |

## Snapshot Fields Read By P4

The P4 polls with `{"v":true}` and parses these response fields. The S3 bridge intentionally compacts snapshots to this set so the response fits inside the P4 16384-byte receive line.

| JSON path | Type/range | P4 use |
| --- | --- | --- |
| `state.on` | boolean | Reconcile P4 power state. |
| `state.bri` | 0..255 | Reconcile P4 brightness percentage. |
| `state.transition` | WLED transition value | Stored in `wled_state_t` for status/readback. |
| `state.ps` | integer, `-1`/missing when no active preset | Lights preset active-panel state. |
| `state.pl` | integer, `-1`/missing when no active playlist | Lights preset panel playlist readback. |
| `state.seg[0].fx` | 0..255 | Lights page effect label and next/previous baseline. |
| `state.seg[0].pal` | 0..255 | Lights page palette label and next/previous baseline. |
| `state.seg[0].sx` | 0..255 | Lights page speed slider sync. |
| `state.seg[0].ix` | 0..255 | Lights page intensity slider sync. |
| `state.seg[0].c1` | 0..255 | Lights page custom slider 1 sync. |
| `state.seg[0].c2` | 0..255 | Lights page custom slider 2 sync. |
| `state.seg[0].c3` | 0..255 | Lights page custom slider 3 sync. |
| `state.seg[0].o1` | boolean | Lights page option toggle 1 sync. |
| `state.seg[0].o2` | boolean | Lights page option toggle 2 sync. |
| `state.seg[0].o3` | boolean | Lights page option toggle 3 sync. |
| `state.seg[0].cct` | 0..255 | Segment color temperature readback. |
| `state.seg[0].col` | up to three RGB triples | Primary/secondary color UI readback. |
| `info.ver` | string, stored in 24-byte P4 buffer | Info and Settings WLED status. |
| `info.leds.count` | integer, stored as `uint16_t` | Info and Settings WLED status. |
| `info.uptime` | seconds | Info/readback state. |
| `info.psu.pin` | integer; default `47` | LED PSU relay pin readback. |
| `info.psu.on` | boolean | Current relay output state. |
| `info.psu.ready` | boolean | Whether the usermod owns the relay pin and can drive it. |
| `info.psu.pendingOff` | boolean | Relay is holding power on after WLED went dark. |
| `info.psu.fault` | boolean | Relay pin allocation/configuration failed. |
| `info.rs485.online` | boolean | S3-side view of whether the P4 has sent a valid frame within the link timeout. |
| `info.rs485.seen` | boolean | Whether the S3 has ever seen a valid P4 frame since boot. |
| `info.rs485.rx` | integer | Count of valid/attempted complete RS-485 JSON lines received by the S3 bridge. |
| `info.rs485.err` | integer | Count of parse, unsupported command, line timeout, or overflow errors seen by the S3 bridge. |
| `info.rs485.tx` | integer | Count of compact snapshots transmitted by the S3 bridge. |
| `info.rs485.ageMs` | integer | Milliseconds since the last valid P4 frame, from the S3 bridge's perspective. |
| `presets` | array of compact `[id,name]` entries | Lights Presets tab renders one WLED-style expandable panel per saved preset. |
| `ptrunc` | boolean, optional | Indicates the bridge capped the returned preset list. |

## P4 Local Settings That Affect WLED

These are stored on the P4, but they either change outbound WLED JSON or are sent during provisioning.

| P4 setting | Range/default | WLED effect |
| --- | --- | --- |
| `led_state.power` | default off | Publishes `on`. |
| `led_state.brightness_pct` | 0..100, default 50 | Publishes `bri` after 0..255 conversion. |
| `led_state.kelvin` | clamped to `kelvin_min..kelvin_max`, default 3500 K | Publishes `seg[0].cct` after 0..255 conversion. |
| `led_state.kelvin_min` | 1000..`kelvin_max - 100`, default 2200 K | Defines warm end of P4 kelvin-to-WLED-CCT mapping. |
| `led_state.kelvin_max` | `kelvin_min + 100`..10000, default 6500 K | Defines cool end of P4 kelvin-to-WLED-CCT mapping. |
| Wi-Fi SSID/PSK | SSID 32 chars, PSK 64 chars | Sent to WLED during provisioning. |
| `screen_timeout_s` | 10..3600, default 60 | P4 display only; not sent to WLED. |
| `display_brightness_pct` | 5..100, default 60 | P4 display backlight only; not sent to WLED. |

## S3 Build Defaults And Web Setup

These are not runtime settings the P4 changes over RS-485. The board-level items must be correct in `platformio_override.ini`, while the LED hardware items are first-boot defaults that can be changed from the S3 WLED web page under Settings > LED Preferences.

| Build flag/setting | Current sample | What to verify |
| --- | --- | --- |
| `board` | `waveshare_esp32s3_relay_1ch` | Local board file in `wled-s3/boards/`; keep in sync with upstream if MoonModules adds an official ID later. |
| `extends` | `env:esp32s3dev_16MB_opi` | Official WLED 16 parent for ESP32-S3 with 16 MB flash and OPI PSRAM. |
| `board_build.arduino.memory_type` | `qio_opi` via local board file | Matches external QIO flash plus OPI PSRAM assumption for ESP32-S3R8-class hardware. |
| `board_upload.flash_size` / partitions | `16MB`, `${esp32.extreme_partitions}` | Confirm OTA/filesystem layout is acceptable before production flashing. |
| `custom_usermods` | `86box_rs485_bridge` | WLED 16 dynamic usermod loader; AudioReactive is intentionally omitted on this branch. |
| `USERMOD_86BOX_RS485_UART` | `1` | S3 UART connected to RS-485 transceiver. |
| `USERMOD_86BOX_RS485_TX` | `17` | S3 UART TX GPIO. |
| `USERMOD_86BOX_RS485_RX` | `18` | S3 UART RX GPIO. |
| `USERMOD_86BOX_RS485_DE` | `21` | GPIO21 is labeled RS485 EN on the provided pinout. |
| `USERMOD_86BOX_RS485_BAUD` | `115200` | Must match P4 `BSP_RS485_BAUD`. |
| `ARDUINO_USB_CDC_ON_BOOT` | `0` | Keep the S3 WLED app headless; enabling CDC-on-boot can make startup depend on opening a USB serial monitor. |
| `LEDPIN` / `DATA_PINS` | `1` | First-boot LED output GPIO; WLED web field maps to `hw.led.ins[0].pin`. |
| `PIXEL_COUNTS` | `300` | First-boot LED count; WLED web field maps to `hw.led.ins[0].len`. |
| `PIXEL_TYPE` | WLED default unless overridden | Set LED chipset/type on the WLED web page; config maps to `hw.led.ins[0].type`. |
| Color order | WLED default unless overridden | Set color order on the WLED web page; config maps to `hw.led.ins[0].order`. |
| `ABL_MILLIAMPS_DEFAULT` | `5000` | First-boot auto brightness limiter in mA; WLED web field maps to `hw.led.maxpwr`. |
| `BTNPIN` | `-1` | Keep disabled unless a physical WLED button is wired. |
| `RLYPIN` | `-1`; onboard relay is GPIO47 | Keep WLED's native relay disabled so the 86Box usermod is the only relay owner. |
| `USERMOD_86BOX_PSU_RELAY_PIN` | `47` | GPIO for the Waveshare onboard relay that switches the LED power supply. |
| `USERMOD_86BOX_PSU_RELAY_ACTIVE_HIGH` | `1` | Relay output active level; change from the S3 WLED Usermods page if hardware testing shows inversion is needed. |
| `USERMOD_86BOX_PSU_RELAY_ON_LEAD_MS` | `750` | Delay before applying P4 LED-on JSON after energizing the PSU relay. |
| `USERMOD_86BOX_PSU_RELAY_OFF_HOLD_MS` | `10000` | Time to keep the PSU energized after WLED output reaches off. |
| `USERMOD_86BOX_PSU_RELAY_MIN_CYCLE_MS` | `1500` | Minimum interval between relay output transitions to avoid chatter. |
| I2S audio pins | `-1` | Disabled to avoid conflicts with relay GPIO47 and unused mic pins. |
| `WLED_RELEASE_NAME` | `86Box-S3-Waveshare-Relay1CH` | Cosmetic build name shown by WLED. |

The practical setup flow is: flash this S3 firmware, join the WLED page, open Settings > LED Preferences, then set LED output GPIO, LED type, color order, LED count, and current limit. Leave the native WLED Relay GPIO at `-1`. Open the Usermods page and adjust `86Box RS485 Bridge` PSU relay settings only if the GPIO, active level, lead time, hold time, or minimum cycle time need to change. Those values are saved in WLED config and should survive normal reboots without rebuilding.