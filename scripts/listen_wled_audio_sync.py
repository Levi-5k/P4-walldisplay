#!/usr/bin/env python3
"""Listen for WLED-MM AudioReactive UDP packets from the P4 firmware.

The P4 sends WLED v2 audioSyncPacket frames on UDP port 11988. Those packets
contain loudness and FFT bins, not raw PCM audio, so this script visualizes the
same reactive mic data WLED consumes instead of playing back intelligible sound.
"""

from __future__ import annotations

import argparse
import json
import shutil
import socket
import struct
import sys
import time
from dataclasses import asdict, dataclass
from typing import Iterable

DEFAULT_PORT = 11988
PACKET = struct.Struct("<6s2sffBB16sHff")
PACKET_SIZE = PACKET.size
HEADER_PREFIX = b"00002"


@dataclass
class AudioSyncPacket:
    source: str
    received_at: float
    sample_raw: float
    sample_smooth: float
    sample_peak: bool
    frame_counter: int
    fft_result: list[int]
    zero_crossing_count: int
    fft_magnitude: float
    fft_major_peak: float


class PacketStats:
    def __init__(self) -> None:
        self.started_at = time.monotonic()
        self.accepted = 0
        self.ignored = 0
        self.dropped_frames = 0
        self.out_of_order = 0
        self.last_frame: int | None = None

    def note_packet(self, frame_counter: int) -> None:
        self.accepted += 1
        if self.last_frame is not None:
            expected = (self.last_frame + 1) & 0xFF
            if frame_counter != expected:
                gap = (frame_counter - expected) & 0xFF
                if 0 < gap < 128:
                    self.dropped_frames += gap
                else:
                    self.out_of_order += 1
        self.last_frame = frame_counter

    def fps(self) -> float:
        elapsed = max(time.monotonic() - self.started_at, 0.001)
        return self.accepted / elapsed


def parse_packet(data: bytes, addr: tuple[str, int], received_at: float) -> AudioSyncPacket:
    if len(data) < PACKET_SIZE:
        raise ValueError(f"short packet: {len(data)} bytes")

    (
        header,
        _pressure,
        sample_raw,
        sample_smooth,
        sample_peak,
        frame_counter,
        fft_result,
        zero_crossing_count,
        fft_magnitude,
        fft_major_peak,
    ) = PACKET.unpack_from(data)

    if not header.startswith(HEADER_PREFIX):
        raise ValueError(f"unexpected header: {header!r}")

    return AudioSyncPacket(
        source=f"{addr[0]}:{addr[1]}",
        received_at=received_at,
        sample_raw=sample_raw,
        sample_smooth=sample_smooth,
        sample_peak=bool(sample_peak),
        frame_counter=frame_counter,
        fft_result=list(fft_result),
        zero_crossing_count=zero_crossing_count,
        fft_magnitude=fft_magnitude,
        fft_major_peak=fft_major_peak,
    )


def make_socket(bind: str, port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except OSError:
            pass
    sock.bind((bind, port))
    sock.settimeout(0.25)
    return sock


def bar(value: int, width: int) -> str:
    value = max(0, min(255, value))
    filled = int((value * width + 127) / 255)
    return "#" * filled + "." * (width - filled)


def format_bins(values: Iterable[int], width: int) -> str:
    return " ".join(bar(value, width) for value in values)


def terminal_width(default: int = 120) -> int:
    return shutil.get_terminal_size((default, 24)).columns


def format_status(packet: AudioSyncPacket, stats: PacketStats, bin_width: int) -> str:
    peak = "*" if packet.sample_peak else " "
    return (
        f"src={packet.source} fps={stats.fps():5.1f} frame={packet.frame_counter:3d} "
        f"raw={packet.sample_raw:6.1f} smooth={packet.sample_smooth:6.1f} peak={peak} "
        f"mag={packet.fft_magnitude:7.3f} major={packet.fft_major_peak:7.1f}Hz "
        f"drop={stats.dropped_frames} ooo={stats.out_of_order} | {format_bins(packet.fft_result, bin_width)}"
    )


def print_status(line: str, interactive: bool) -> None:
    if interactive:
        width = max(20, terminal_width() - 1)
        sys.stdout.write("\r" + line[:width].ljust(width))
        sys.stdout.flush()
    else:
        print(line, flush=True)


def packet_to_json(packet: AudioSyncPacket) -> str:
    return json.dumps(asdict(packet), separators=(",", ":"))


def self_test() -> int:
    bins = bytes([0, 4, 9, 16, 25, 36, 49, 64, 81, 100, 121, 144, 169, 196, 225, 255])
    sample = PACKET.pack(b"00002\0", b"\0\0", 42.0, 24.5, 1, 7, bins, 0, 3.25, 437.5)
    packet = parse_packet(sample, ("127.0.0.1", DEFAULT_PORT), time.time())
    stats = PacketStats()
    stats.note_packet(packet.frame_counter)
    print(packet_to_json(packet))
    print(format_status(packet, stats, 4))
    return 0


def listen(args: argparse.Namespace) -> int:
    stats = PacketStats()
    start = time.monotonic()
    last_status = 0.0
    last_hint = 0.0
    interactive = sys.stdout.isatty() and not args.json
    wanted_source = args.source.strip() if args.source else ""

    with make_socket(args.bind, args.port) as sock:
        if not args.json:
            print(f"Listening for WLED audio sync UDP on {args.bind}:{args.port}")
            print("Packets are WLED AudioReactive FFT/level data, not playable PCM audio.")
            print("If nothing appears while WLED is online, the P4 may be unicasting to wled-86box.local instead of broadcasting.")

        try:
            while True:
                now = time.monotonic()
                if args.duration > 0 and now - start >= args.duration:
                    break

                try:
                    data, addr = sock.recvfrom(2048)
                except socket.timeout:
                    if not args.json and now - last_hint >= args.hint_seconds and stats.accepted == 0:
                        print("No packets yet. Keep this Mac on the same Wi-Fi/VLAN as the P4 and WLED.", flush=True)
                        last_hint = now
                    continue

                if wanted_source and addr[0] != wanted_source:
                    stats.ignored += 1
                    continue

                try:
                    packet = parse_packet(data, addr, time.time())
                except ValueError:
                    stats.ignored += 1
                    if args.show_ignored:
                        print(f"ignored {len(data)} bytes from {addr[0]}:{addr[1]}", flush=True)
                    continue

                stats.note_packet(packet.frame_counter)

                if args.json:
                    print(packet_to_json(packet), flush=True)
                    continue

                if now - last_status >= 1.0 / max(args.update_hz, 1.0):
                    print_status(format_status(packet, stats, args.bin_width), interactive)
                    last_status = now

        except KeyboardInterrupt:
            pass

    if interactive:
        print()
    if not args.json:
        print(
            f"Received {stats.accepted} WLED audio packets"
            f" ({stats.fps():.1f} fps avg), ignored {stats.ignored},"
            f" dropped {stats.dropped_frames}, out-of-order {stats.out_of_order}."
        )
    return 0 if stats.accepted > 0 or args.duration == 0 else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Listen for WLED-MM v2 AudioReactive UDP packets from the P4 firmware."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="local address to bind (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP port to listen on (default: 11988)")
    parser.add_argument("--source", default="", help="only accept packets from this source IP")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to run; 0 means run until Ctrl-C")
    parser.add_argument("--update-hz", type=float, default=10.0, help="status refresh rate for the live view")
    parser.add_argument("--bin-width", type=int, default=4, help="characters per FFT-bin bar")
    parser.add_argument("--json", action="store_true", help="print one JSON object per packet")
    parser.add_argument("--show-ignored", action="store_true", help="log non-WLED packets received on the port")
    parser.add_argument("--hint-seconds", type=float, default=5.0, help="seconds between no-packet hints")
    parser.add_argument("--self-test", action="store_true", help="decode one synthetic packet and exit")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.self_test:
        return self_test()
    return listen(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
