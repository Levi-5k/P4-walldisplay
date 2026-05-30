# Serial Diagnostics

The P4 firmware starts a USB serial diagnostic console on the normal monitor port. It is useful for checking background downloads, timer audio assets, and the RS-485/WLED link without opening the LVGL UI.

Open the monitor at 115200 baud:

```sh
~/.platformio/penv/bin/pio device monitor --port /dev/cu.usbmodem5B610402391 --baud 115200
```

When the diagnostic task is running, boot output includes:

```text
[diag] serial diagnostics ready; type help
```

Type a command and press Enter. Diagnostic replies are prefixed with `[diag]`.

## Runtime Help

The command set is also discoverable from the device:

```text
help
bg help
audio help
rs485 help
```

Top-level aliases:

| Alias | Equivalent |
| --- | --- |
| `?` | `help` |
| `background` | `bg` |
| `sound` | `audio` |
| `wled` | `rs485` |

## Background Commands

| Command | Purpose |
| --- | --- |
| `bg status` | Show background download state, selected preset, and cached image count. |
| `bg download [preset-name\|number]` | Start downloading the selected background preset. If omitted, uses the current theme preset. Numbers are 1-based. |
| `bg delete` | Delete `/sdcard/walldisplay_theme` and reset the background state. |
| `bg help` | Print background command help. |

Aliases:

| Alias | Equivalent |
| --- | --- |
| `bg dl` | `bg download` |
| `bg clear` | `bg delete` |
| `bg rm` | `bg delete` |

## Audio Commands

| Command | Purpose |
| --- | --- |
| `audio status` | Show audio output readiness, playback state, current file, and last status. |
| `audio test` | Queue the built-in test chime. |
| `audio list` | List WAV files found in `/sdcard/audio`. |
| `audio assets` | Show the default timer-audio library assets and whether they are present. |
| `audio download` | Start downloading the default timer-audio assets. |
| `audio play <path\|name\|number>` | Queue a WAV file by absolute path, file name from `audio list`, or 1-based list number. |
| `audio stop` | Stop current audio playback. |
| `audio help` | Print audio command help. |

Aliases:

| Alias | Equivalent |
| --- | --- |
| `audio tone` | `audio test` |
| `audio chime` | `audio test` |
| `audio ls` | `audio list` |
| `audio library` | `audio assets` |
| `audio dl` | `audio download` |

## RS-485 / WLED Commands

| Command | Purpose |
| --- | --- |
| `rs485 status` | Show P4 RS-485 readiness, queue state, WLED online state, counters, and the latest WLED snapshot. |
| `rs485 ping` | Queue a WLED poll frame: `{"v":true}`. |
| `rs485 provision` | Send the P4's stored Wi-Fi and sound-sync settings to the S3/WLED node. |
| `rs485 send <json>` | Queue a raw WLED JSON command over RS-485. |
| `rs485 help` | Print RS-485 command help. |

Aliases:

| Alias | Equivalent |
| --- | --- |
| `rs485 stat` | `rs485 status` |
| `rs485 poll` | `rs485 ping` |
| `rs485 prov` | `rs485 provision` |
| `rs485 tx <json>` | `rs485 send <json>` |

Examples:

```text
rs485 status
rs485 ping
rs485 send {"on":true,"bri":128}
rs485 send {"seg":[{"fx":42}]}
```

The raw `rs485 send` command expects one complete JSON object on the same line.