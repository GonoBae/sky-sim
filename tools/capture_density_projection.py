#!/usr/bin/env python3
"""Capture one live CLD2 density field and render a top-down diagnostic."""

import argparse
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

from PIL import Image


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY))

from tools import receive_probe as probe  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--grid", type=int, default=64)
    parser.add_argument("--warmup-seconds", type=float, default=12.0)
    args = parser.parse_args()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024)
    receiver.bind(("127.0.0.1", 0))
    receiver.settimeout(0.5)
    port = receiver.getsockname()[1]

    process = subprocess.Popen(
        [
            args.server,
            "--grid",
            str(args.grid),
            "--hz",
            "20",
            "--send-hz",
            "1",
            "--fields",
            "density",
            "--warmup-seconds",
            str(args.warmup_seconds),
            "--seconds",
            "60",
            "--weather",
            "natural",
            "--weather-seed",
            "55",
            "--no-control",
            "--no-sky",
            "--no-sky-control",
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    receive_args = SimpleNamespace(
        protocol="2",
        max_field_bytes=64 * 1024 * 1024,
        max_buffer_bytes=128 * 1024 * 1024,
    )
    frames = {}
    completed = None
    deadline = time.monotonic() + 45.0
    output = ""
    try:
        while time.monotonic() < deadline and completed is None:
            try:
                packet, source = receiver.recvfrom(2048)
            except socket.timeout:
                if process.poll() is not None:
                    break
                continue
            if packet.startswith(b"CLD2"):
                completed = probe.process_v2(packet, source, frames, receive_args)
        if completed is not None and process.poll() is None:
            process.terminate()
        output, _ = process.communicate(timeout=8.0)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3.0)
        receiver.close()

    if completed is None:
        raise RuntimeError(f"density capture failed\n{output}")

    field = completed["fields"][1]
    size_x, size_y, size_z = field["shape"]
    values = struct.unpack(f"<{size_x * size_y * size_z}H", field["decoded"])
    projection = []
    for y in range(size_y):
        for x in range(size_x):
            maximum = max(
                values[(z * size_y + y) * size_x + x]
                for z in range(size_z)
            )
            projection.append(maximum / 65535.0)

    maximum_projection = max(projection)
    active_threshold = maximum_projection * 0.14
    active_fraction = sum(value >= active_threshold for value in projection) / len(
        projection
    )
    mean_projection = sum(projection) / len(projection)
    variance = sum(
        (value - mean_projection) ** 2 for value in projection
    ) / len(projection)
    relative_deviation = variance**0.5 / max(mean_projection, 1.0e-8)

    pixels = []
    for value in projection:
        normalized = (value / max(maximum_projection, 1.0e-8)) ** 0.52
        pixels.append(
            (
                int(25 + 230 * normalized),
                int(75 + 180 * normalized),
                int(135 + 120 * normalized),
            )
        )
    image = Image.new("RGB", (size_x, size_y))
    image.putdata(pixels)
    image = image.resize((768, 768), Image.Resampling.BICUBIC)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path)
    print(
        f"Saved {output_path} frame={completed['frame_id']} "
        f"active_fraction={active_fraction:.4f} projection_cv={relative_deviation:.4f}"
    )


if __name__ == "__main__":
    main()
