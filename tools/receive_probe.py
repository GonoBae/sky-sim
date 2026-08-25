#!/usr/bin/env python3
"""Receive and strictly validate one complete cloud frame using only Python stdlib."""

import argparse
import json
import math
import socket
import struct
import sys
import time
import zlib
from array import array
from pathlib import Path


V1_HEADER = struct.Struct("<4sHHIfHHHBBHHHHII")
V2_HEADER = struct.Struct("<4sHHIIdHHHBBBBHHHHHIIIff")
MAX_PAYLOAD = 1200
MAX_FRAMES = 8
FIELD_KEYFRAME_FLAG = 1 << 0
OCCUPANCY_BRICK_SHIFT = 8
SUPPORTED_OCCUPANCY_BRICKS = frozenset((2, 4, 8))

FIELD_NAMES = {
    1: "density",
    2: "velocity",
    3: "temperature",
    4: "vapor",
    5: "occupancy",
}
FORMAT_NAMES = {1: "UNorm8", 2: "UNorm16", 3: "SNorm8", 4: "SNorm16"}
BYTES_PER_CHANNEL = {1: 1, 2: 2, 3: 1, 4: 2}
FIELD_LAYOUTS = {
    1: (2, 1),
    2: (4, 3),
    3: (1, 1),
    4: (1, 1),
    5: (1, 1),
}
SUPPORTED_FIELD_MASK = sum(1 << (field_id - 1) for field_id in FIELD_NAMES)
MACRO_FIELD_IDS = frozenset((1, 2, 3, 4))
MACRO_FIELD_MASK = sum(1 << (field_id - 1) for field_id in MACRO_FIELD_IDS)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=7777, type=int)
    parser.add_argument("--timeout", default=10.0, type=float)
    parser.add_argument(
        "--protocol", choices=("auto", "1", "2"), default="auto",
        help="Accepted protocol version (default: auto)",
    )
    parser.add_argument(
        "--max-field-mb", default=64.0, type=float,
        help="Maximum encoded or decoded field allocation (default: 64 MiB)",
    )
    parser.add_argument(
        "--max-buffer-mb", default=128.0, type=float,
        help="Maximum memory retained across incomplete frames (default: 128 MiB)",
    )
    parser.add_argument("--output", type=Path, help="Write the completed density field")
    parser.add_argument(
        "--output-dir", type=Path,
        help="Write every v2 field and a JSON metadata file into this directory",
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a positive finite number")
    if not math.isfinite(args.max_field_mb) or args.max_field_mb <= 0:
        parser.error("--max-field-mb must be a positive finite number")
    if not math.isfinite(args.max_buffer_mb) or args.max_buffer_mb <= 0:
        parser.error("--max-buffer-mb must be a positive finite number")
    args.max_field_bytes = int(args.max_field_mb * 1024 * 1024)
    args.max_buffer_bytes = int(args.max_buffer_mb * 1024 * 1024)
    return args


def expected_chunk_count(encoded_bytes):
    return (encoded_bytes + MAX_PAYLOAD - 1) // MAX_PAYLOAD


def validate_chunk(chunk_index, chunk_count, payload_bytes, payload_offset, encoded_bytes):
    if encoded_bytes <= 0 or chunk_count != expected_chunk_count(encoded_bytes):
        raise ValueError("invalid encoded size or chunk count")
    if chunk_index >= chunk_count or payload_bytes > MAX_PAYLOAD:
        raise ValueError("chunk index or payload size is outside the protocol")
    expected_offset = chunk_index * MAX_PAYLOAD
    expected_payload = min(MAX_PAYLOAD, encoded_bytes - expected_offset)
    if payload_offset != expected_offset or payload_bytes != expected_payload:
        raise ValueError("chunk offset or size does not match its index")


def validate_field_flags(field_id, flags):
    reserved_low_bits = (flags & 0xFF) & ~FIELD_KEYFRAME_FLAG
    if reserved_low_bits:
        raise ValueError("v2 field flags contain reserved low bits")

    occupancy_brick = flags >> OCCUPANCY_BRICK_SHIFT
    if field_id == 5:
        if occupancy_brick not in SUPPORTED_OCCUPANCY_BRICKS:
            raise ValueError("occupancy field must declare brick size 2, 4, or 8")
    elif occupancy_brick != 0:
        raise ValueError("only the occupancy field may declare a brick size")
    return occupancy_brick


def validate_field_relationship(frame, field_id, shape, flags):
    existing_fields = frame["fields"]
    if field_id in MACRO_FIELD_IDS:
        for existing_id, existing in existing_fields.items():
            if existing_id in MACRO_FIELD_IDS and existing["shape"] != shape:
                raise ValueError("v2 macro fields must use the same grid shape")

        occupancy = existing_fields.get(5)
        if occupancy is not None:
            brick = occupancy["flags"] >> OCCUPANCY_BRICK_SHIFT
            expected_shape = tuple((dimension + brick - 1) // brick for dimension in shape)
            if occupancy["shape"] != expected_shape:
                raise ValueError("occupancy shape does not match the macro grid and brick size")
        return

    if field_id != 5:
        return
    if (frame["field_mask"] & MACRO_FIELD_MASK) == 0:
        raise ValueError("occupancy field requires at least one macro field")

    brick = flags >> OCCUPANCY_BRICK_SHIFT
    for existing_id, existing in existing_fields.items():
        if existing_id not in MACRO_FIELD_IDS:
            continue
        expected_shape = tuple(
            (dimension + brick - 1) // brick for dimension in existing["shape"]
        )
        if shape != expected_shape:
            raise ValueError("occupancy shape does not match the macro grid and brick size")
        break


def insert_chunk(field, chunk_index, payload_offset, payload):
    if field["received"][chunk_index]:
        existing = field["encoded"][payload_offset:payload_offset + len(payload)]
        if existing != payload:
            raise ValueError("duplicate chunk has different data")
        return
    field["encoded"][payload_offset:payload_offset + len(payload)] = payload
    field["received"][chunk_index] = True
    field["received_count"] += 1


def rle_decompress(encoded, expected_size):
    output = bytearray()
    position = 0
    while position < len(encoded):
        control = encoded[position]
        position += 1
        if control & 0x80:
            if position >= len(encoded):
                raise ValueError("truncated RLE repeat")
            length = (control & 0x7F) + 3
            if len(output) + length > expected_size:
                raise ValueError("RLE output exceeds decoded size")
            output.extend([encoded[position]] * length)
            position += 1
        else:
            length = control + 1
            if position + length > len(encoded) or len(output) + length > expected_size:
                raise ValueError("truncated or oversized RLE literal")
            output.extend(encoded[position:position + length])
            position += length
    if len(output) != expected_size:
        raise ValueError("RLE output size does not match header")
    return output


def decode_completed_field(field):
    encoded = field["encoded"]
    if field["compression"] == 0:
        decoded = encoded
    elif field["compression"] == 1:
        decoded = rle_decompress(encoded, field["decoded_bytes"])
    else:
        raise ValueError("unsupported compression")
    if len(decoded) != field["decoded_bytes"]:
        raise ValueError("decoded byte count does not match header")
    if zlib.crc32(decoded) & 0xFFFFFFFF != field["crc32"]:
        raise ValueError("field CRC32 mismatch")
    return decoded


def ensure_frame_capacity(frames):
    if len(frames) < MAX_FRAMES:
        return
    oldest_key = min(frames, key=lambda key: frames[key]["last_received"])
    del frames[oldest_key]


def buffered_bytes(frames):
    total = 0
    for frame in frames.values():
        if frame["version"] == 1:
            total += len(frame["data"])
            continue
        for field in frame["fields"].values():
            if "encoded" in field:
                total += len(field["encoded"])
            if field["decoded"] is not None:
                total += len(field["decoded"])
    return total


def require_buffer_capacity(frames, additional_bytes, args):
    current = buffered_bytes(frames)
    if current > args.max_buffer_bytes or additional_bytes > args.max_buffer_bytes - current:
        raise ValueError("receiver buffer limit would be exceeded")


def process_v1(packet, source, frames, args):
    if args.protocol == "2" or len(packet) < V1_HEADER.size:
        return None
    fields = V1_HEADER.unpack_from(packet)
    (magic, version, header_bytes, frame_id, sim_time, nx, ny, nz,
     voxel_format, field_id, chunk_index, chunk_count, payload_bytes,
     _reserved, payload_offset, frame_bytes) = fields
    if magic != b"CLD1" or version != 1 or header_bytes != V1_HEADER.size:
        return None
    if voxel_format != 1 or field_id != 1 or not math.isfinite(sim_time):
        raise ValueError("invalid v1 density metadata")
    if min(nx, ny, nz) <= 0 or frame_bytes != nx * ny * nz:
        raise ValueError("v1 dimensions do not match frame size")
    if frame_bytes > args.max_field_bytes:
        raise ValueError("v1 frame exceeds allocation limit")
    if len(packet) != header_bytes + payload_bytes:
        raise ValueError("v1 datagram length does not match payload size")
    validate_chunk(chunk_index, chunk_count, payload_bytes, payload_offset, frame_bytes)

    key = (source, 1, frame_id)
    if key not in frames:
        ensure_frame_capacity(frames)
        require_buffer_capacity(frames, frame_bytes, args)
        frames[key] = {
            "version": 1,
            "frame_id": frame_id,
            "time": sim_time,
            "shape": (nx, ny, nz),
            "data": bytearray(frame_bytes),
            "received": [False] * chunk_count,
            "received_count": 0,
            "last_received": time.monotonic(),
        }
    frame = frames[key]
    if frame["shape"] != (nx, ny, nz) or len(frame["received"]) != chunk_count:
        raise ValueError("inconsistent metadata inside v1 frame")
    payload = packet[header_bytes:]
    if frame["received"][chunk_index]:
        if frame["data"][payload_offset:payload_offset + payload_bytes] != payload:
            raise ValueError("v1 duplicate chunk differs")
    else:
        frame["data"][payload_offset:payload_offset + payload_bytes] = payload
        frame["received"][chunk_index] = True
        frame["received_count"] += 1
    frame["last_received"] = time.monotonic()
    return frame if frame["received_count"] == chunk_count else None


def process_v2(packet, source, frames, args):
    if args.protocol == "1" or len(packet) < V2_HEADER.size:
        return None
    fields = V2_HEADER.unpack_from(packet)
    (magic, version, header_bytes, frame_id, field_crc32, sim_time,
     nx, ny, nz, voxel_format, field_id, channel_count, compression,
     chunk_index, chunk_count, payload_bytes, field_mask, flags,
     payload_offset, encoded_bytes, decoded_bytes, value_scale, value_bias) = fields
    if magic != b"CLD2" or version != 2 or header_bytes != V2_HEADER.size:
        return None
    if field_id not in FIELD_NAMES or voxel_format not in BYTES_PER_CHANNEL:
        raise ValueError("unsupported v2 field or voxel format")
    if (voxel_format, channel_count) != FIELD_LAYOUTS[field_id]:
        raise ValueError("v2 field format or channel count does not match its definition")
    validate_field_flags(field_id, flags)
    if compression not in (0, 1) or channel_count <= 0:
        raise ValueError("unsupported compression or channel count")
    if not all(math.isfinite(value) for value in (sim_time, value_scale, value_bias)):
        raise ValueError("non-finite v2 metadata")
    if min(nx, ny, nz) <= 0:
        raise ValueError("zero-sized v2 field")
    expected_decoded = nx * ny * nz * channel_count * BYTES_PER_CHANNEL[voxel_format]
    if decoded_bytes != expected_decoded:
        raise ValueError("v2 decoded size does not match field shape")
    if max(encoded_bytes, decoded_bytes) > args.max_field_bytes:
        raise ValueError("v2 field exceeds allocation limit")
    field_bit = 1 << (field_id - 1)
    if field_mask == 0 or field_mask & ~SUPPORTED_FIELD_MASK or not field_mask & field_bit:
        raise ValueError("invalid v2 field mask")
    if len(packet) != header_bytes + payload_bytes:
        raise ValueError("v2 datagram length does not match payload size")
    validate_chunk(chunk_index, chunk_count, payload_bytes, payload_offset, encoded_bytes)

    frame_key = (source, 2, frame_id)
    if frame_key not in frames:
        ensure_frame_capacity(frames)
        frames[frame_key] = {
            "version": 2,
            "frame_id": frame_id,
            "time": sim_time,
            "field_mask": field_mask,
            "fields": {},
            "last_received": time.monotonic(),
        }
    frame = frames[frame_key]
    if frame["field_mask"] != field_mask or frame["time"] != sim_time:
        raise ValueError("inconsistent metadata inside v2 frame")

    validate_field_relationship(frame, field_id, (nx, ny, nz), flags)

    metadata = (
        (nx, ny, nz), voxel_format, channel_count, compression, chunk_count,
        encoded_bytes, decoded_bytes, field_crc32, flags, value_scale, value_bias,
    )
    if field_id not in frame["fields"]:
        require_buffer_capacity(frames, encoded_bytes, args)
        frame["fields"][field_id] = {
            "metadata": metadata,
            "shape": (nx, ny, nz),
            "voxel_format": voxel_format,
            "channels": channel_count,
            "compression": compression,
            "encoded_bytes": encoded_bytes,
            "decoded_bytes": decoded_bytes,
            "crc32": field_crc32,
            "flags": flags,
            "scale": value_scale,
            "bias": value_bias,
            "encoded": bytearray(encoded_bytes),
            "received": [False] * chunk_count,
            "received_count": 0,
            "decoded": None,
        }
    field = frame["fields"][field_id]
    if field["metadata"] != metadata:
        raise ValueError("inconsistent packet metadata inside v2 field")
    if field["decoded"] is None:
        insert_chunk(field, chunk_index, payload_offset, packet[header_bytes:])
        if field["received_count"] == chunk_count:
            require_buffer_capacity(frames, field["decoded_bytes"], args)
            field["decoded"] = decode_completed_field(field)
            del field["encoded"]
            del field["received"]

    frame["last_received"] = time.monotonic()
    complete_mask = sum(
        1 << (completed_id - 1)
        for completed_id, completed in frame["fields"].items()
        if completed["decoded"] is not None
    )
    return frame if complete_mask == field_mask else None


def scalar_stats(field):
    data = field["decoded"]
    fmt = field["voxel_format"]
    if fmt == 2:
        values = array("H")
        values.frombytes(data)
        if sys.byteorder != "little":
            values.byteswap()
        denominator = 65535.0
        minimum_encoded, maximum_encoded = min(values), max(values)
        mean_encoded = sum(values) / len(values)
    elif fmt == 3:
        values = [(byte if byte < 128 else byte - 256) for byte in data]
        denominator = 127.0
        minimum_encoded, maximum_encoded = min(values), max(values)
        mean_encoded = sum(values) / len(values)
    else:
        denominator = 255.0
        minimum_encoded, maximum_encoded = min(data), max(data)
        mean_encoded = sum(data) / len(data)
    scale, bias = field["scale"], field["bias"]
    return (
        minimum_encoded / denominator * scale + bias,
        maximum_encoded / denominator * scale + bias,
        mean_encoded / denominator * scale + bias,
    )


def describe_completed_frame(frame, source):
    if frame["version"] == 1:
        data = bytes(frame["data"])
        mean = sum(data) / len(data)
        print(
            f"Frame {frame['frame_id']} complete from {source[0]}:{source[1]}: "
            f"protocol=v1 shape={frame['shape']} time={frame['time']:.3f}s "
            f"bytes={len(data)} density[min={min(data)}, max={max(data)}, mean={mean:.2f}]"
        )
        return

    print(
        f"Frame {frame['frame_id']} complete from {source[0]}:{source[1]}: "
        f"protocol=v2 time={frame['time']:.6f}s fields={len(frame['fields'])}"
    )
    for field_id in sorted(frame["fields"]):
        field = frame["fields"][field_id]
        compression = "rle" if field["compression"] == 1 else "none"
        ratio = field["encoded_bytes"] / field["decoded_bytes"]
        line = (
            f"  {FIELD_NAMES[field_id]:11s} shape={field['shape']} "
            f"format={FORMAT_NAMES[field['voxel_format']]}x{field['channels']} "
            f"bytes={field['decoded_bytes']} compression={compression}({ratio:.3f}) "
            f"scale={field['scale']:.6g} bias={field['bias']:.6g}"
        )
        if field["channels"] == 1:
            minimum, maximum, mean = scalar_stats(field)
            line += f" physical[min={minimum:.5g}, max={maximum:.5g}, mean={mean:.5g}]"
        if field_id == 5:
            line += f" brick={field['flags'] >> 8}"
        print(line)


def write_outputs(frame, args):
    if frame["version"] == 1:
        if args.output:
            args.output.write_bytes(bytes(frame["data"]))
            print(f"Wrote {args.output}")
        return

    density = frame["fields"].get(1)
    if args.output and density:
        args.output.write_bytes(density["decoded"])
        print(f"Wrote {args.output}")
    if not args.output_dir:
        return

    args.output_dir.mkdir(parents=True, exist_ok=True)
    metadata = {
        "protocol": 2,
        "frame_id": frame["frame_id"],
        "simulation_time": frame["time"],
        "fields": {},
    }
    for field_id, field in frame["fields"].items():
        name = FIELD_NAMES[field_id]
        filename = f"frame-{frame['frame_id']}-{name}.raw"
        (args.output_dir / filename).write_bytes(field["decoded"])
        metadata["fields"][name] = {
            "file": filename,
            "shape": field["shape"],
            "format": FORMAT_NAMES[field["voxel_format"]],
            "channels": field["channels"],
            "value_scale": field["scale"],
            "value_bias": field["bias"],
            "flags": field["flags"],
            "crc32": f"{field['crc32']:08x}",
        }
    metadata_path = args.output_dir / f"frame-{frame['frame_id']}.json"
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"Wrote {len(frame['fields'])} fields and {metadata_path}")


def main():
    args = parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind((args.host, args.port))
    sock.settimeout(0.5)
    deadline = time.monotonic() + args.timeout
    frames = {}
    rejected = 0

    print(f"Waiting for cloud packets on {args.host}:{args.port} ...")
    while time.monotonic() < deadline:
        try:
            packet, source = sock.recvfrom(2048)
        except socket.timeout:
            continue

        try:
            completed = None
            if packet.startswith(b"CLD2"):
                completed = process_v2(packet, source, frames, args)
            elif packet.startswith(b"CLD1"):
                completed = process_v1(packet, source, frames, args)
            if completed:
                describe_completed_frame(completed, source)
                write_outputs(completed, args)
                return 0
        except (MemoryError, ValueError, struct.error):
            rejected += 1

        now = time.monotonic()
        for key in list(frames):
            if now - frames[key]["last_received"] > 2.0:
                del frames[key]

    print(f"Timed out before receiving a complete frame. Rejected packets: {rejected}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
