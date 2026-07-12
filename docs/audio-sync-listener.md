# WLED Audio Sync Listener

The P4 firmware sends WLED-MM AudioReactive v2 UDP packets from its ES7210 mic pipeline on port `11988`. These packets contain loudness, peak state, FFT bins, magnitude, and major peak frequency. They do not contain raw PCM audio, so the host listener visualizes the same mic-reactive data WLED uses rather than playing recognizable audio.

Run the listener from the repo root:

```sh
python3 scripts/listen_wled_audio_sync.py
```

Useful options:

```sh
python3 scripts/listen_wled_audio_sync.py --json
python3 scripts/listen_wled_audio_sync.py --source 192.168.50.10
python3 scripts/listen_wled_audio_sync.py --duration 10
python3 scripts/listen_wled_audio_sync.py --self-test
```

The expected packet format is the 44-byte WLED-MM v2 `audioSyncPacket`:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 6 | Header, ASCII `00002` plus NUL |
| 8 | 4 | `sampleRaw` float |
| 12 | 4 | `sampleSmooth` float |
| 16 | 1 | `samplePeak` flag |
| 17 | 1 | Frame counter |
| 18 | 16 | FFT bins |
| 36 | 4 | FFT magnitude float |
| 40 | 4 | FFT major peak float |

## Network Caveat

The current firmware sends packets to the WLED AudioReactive multicast group `239.0.0.1` on port `11988`. A side-by-side listener must join that multicast group; plain UDP broadcast listeners will not see the stream.
