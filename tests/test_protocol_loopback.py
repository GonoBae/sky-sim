#!/usr/bin/env python3
"""End-to-end CLD2 five-field loopback test against cloud_sim_server."""

import argparse
import math
import socket
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import receive_probe as probe  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--grid", type=int, default=16)
    args = parser.parse_args()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    receiver.bind(("127.0.0.1", 0))
    receiver.settimeout(0.5)
    port = receiver.getsockname()[1]

    process = subprocess.Popen(
        [
            args.server,
            "--grid", str(args.grid),
            "--hz", "20",
            "--send-hz", "10",
            "--warmup-seconds", "0",
            "--seconds", "1.2",
            "--no-control",
            "--no-sky",
            "--no-sky-control",
            "--host", "127.0.0.1",
            "--port", str(port),
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
    deadline = time.monotonic() + 6.0
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
        output, _ = process.communicate(timeout=5.0)
    except Exception:
        if process.poll() is None:
            process.terminate()
        output, _ = process.communicate(timeout=3.0)
        print(output)
        raise
    finally:
        receiver.close()

    if process.returncode != 0:
        print(output)
        raise RuntimeError(f"server exited with {process.returncode}")
    if completed is None:
        print(output)
        raise RuntimeError("no complete CLD2 frame was received")

    expected_mask = probe.SUPPORTED_FIELD_MASK
    expected_macro_shape = (args.grid, args.grid, args.grid)
    expected_occupancy_shape = tuple(
        (dimension + 3) // 4 for dimension in expected_macro_shape
    )
    if completed["field_mask"] != expected_mask:
        raise RuntimeError(f"unexpected field mask: {completed['field_mask']:#x}")
    if set(completed["fields"]) != set(probe.FIELD_NAMES):
        raise RuntimeError("server frame did not contain all five CLD2 fields")

    for field_id in probe.MACRO_FIELD_IDS:
        field = completed["fields"][field_id]
        if field["shape"] != expected_macro_shape:
            raise RuntimeError(f"field {field_id} has the wrong macro grid shape")
        if field["flags"] != probe.FIELD_KEYFRAME_FLAG:
            raise RuntimeError(f"field {field_id} has unexpected flags")

    occupancy = completed["fields"][5]
    occupancy_brick = occupancy["flags"] >> probe.OCCUPANCY_BRICK_SHIFT
    if occupancy_brick != 4 or occupancy["shape"] != expected_occupancy_shape:
        raise RuntimeError("occupancy brick metadata does not match its grid shape")

    density = completed["fields"][1]
    velocity = completed["fields"][2]
    temperature = completed["fields"][3]
    vapor = completed["fields"][4]
    if (density["voxel_format"], density["channels"]) != (2, 1):
        raise RuntimeError("density is not R16 UNorm")
    if (velocity["voxel_format"], velocity["channels"]) != (4, 3):
        raise RuntimeError("velocity is not RGB16 SNorm")
    if not math.isclose(temperature["scale"], 4.0) or not math.isclose(
        temperature["bias"], -1.0
    ):
        raise RuntimeError("temperature decode scale/bias is incorrect")
    if not math.isclose(occupancy["scale"], density["scale"]):
        raise RuntimeError("occupancy and density do not share the cloud-density scale")
    if density["bias"] != 0.0 or velocity["bias"] != 0.0 or vapor["bias"] != 0.0:
        raise RuntimeError("density, velocity, or vapor has an unexpected value bias")

    print(
        "CLD2 loopback passed:",
        f"frame={completed['frame_id']}",
        f"time={completed['time']:.3f}",
        f"fields={len(completed['fields'])}",
        f"macro={expected_macro_shape}",
        f"occupancy={expected_occupancy_shape}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
