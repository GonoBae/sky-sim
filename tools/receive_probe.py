#!/usr/bin/env python3
"""Receive and validate one complete cloud density frame using only Python stdlib."""

import argparse
import socket
import struct
import time
from pathlib import Path


HEADER = struct.Struct("<4sHHIfHHHBBHHHHII")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=7777, type=int)
    parser.add_argument("--timeout", default=10.0, type=float)
    parser.add_argument("--output", type=Path, help="Optional path for the received .raw file")
    return parser.parse_args()


def main():
    args = parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    sock.settimeout(0.5)
    deadline = time.monotonic() + args.timeout
    frames = {}

    print(f"Waiting for cloud packets on {args.host}:{args.port} ...")
    while time.monotonic() < deadline:
        try:
            packet, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(packet) < HEADER.size:
            continue

        fields = HEADER.unpack_from(packet)
        (magic, version, header_bytes, frame_id, sim_time, nx, ny, nz,
         voxel_format, field_id, chunk_index, chunk_count, payload_bytes,
         _reserved, payload_offset, frame_bytes) = fields
        if magic != b"CLD1" or version != 1 or header_bytes != HEADER.size:
            continue
        if voxel_format != 1 or field_id != 1:
            continue
        payload = packet[header_bytes:header_bytes + payload_bytes]
        if len(payload) != payload_bytes:
            continue

        frame = frames.setdefault(frame_id, {
            "data": bytearray(frame_bytes),
            "chunks": set(),
            "count": chunk_count,
            "shape": (nx, ny, nz),
            "time": sim_time,
        })
        frame["data"][payload_offset:payload_offset + payload_bytes] = payload
        frame["chunks"].add(chunk_index)

        if len(frame["chunks"]) == frame["count"]:
            data = bytes(frame["data"])
            mean = sum(data) / len(data)
            print(
                f"Frame {frame_id} complete: shape={frame['shape']} "
                f"time={frame['time']:.3f}s bytes={len(data)} "
                f"density[min={min(data)}, max={max(data)}, mean={mean:.2f}]"
            )
            if args.output:
                args.output.write_bytes(data)
                print(f"Wrote {args.output}")
            return 0

        for old_id in list(frames):
            if old_id + 3 < frame_id:
                del frames[old_id]

    print("Timed out before receiving a complete frame.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
