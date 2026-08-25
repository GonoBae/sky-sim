#!/usr/bin/env python3
"""Send dynamic cloud-interactor commands to the simulation server.

The wire format is the fixed-size, little-endian CLC2 control protocol.  This
tool intentionally uses only the Python standard library so it can also serve
as a compact interoperability reference for an Unreal implementation.
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import secrets
import socket
import struct
import sys
import time
import zlib
from typing import Iterable, Sequence


MAGIC = b"CLC2"
VERSION = 2
PACKET_BYTES = 128
CRC_OFFSET = 124
CONTROL_PACKET = struct.Struct("<4sHHIIQdBBHI3f4f3f3f3ff16sI")

OPCODE_UPSERT = 1
OPCODE_REMOVE = 2
OPCODE_CLEAR = 3

SHAPE_SPHERE = 1
SHAPE_BOX = 2
SHAPE_CAPSULE_Z = 3
SHAPE_ELLIPSOID = 4

FLAG_SOLID = 1 << 0
FLAG_DISPLACEMENT = 1 << 1
FLAG_WAKE = 1 << 2
SUPPORTED_FLAGS = FLAG_SOLID | FLAG_DISPLACEMENT | FLAG_WAKE

UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1

SHAPE_NAMES = {
    "sphere": SHAPE_SPHERE,
    "box": SHAPE_BOX,
    "capsule-z": SHAPE_CAPSULE_Z,
    "ellipsoid": SHAPE_ELLIPSOID,
}
FLAG_NAMES = {
    "solid": FLAG_SOLID,
    "displacement": FLAG_DISPLACEMENT,
    "wake": FLAG_WAKE,
}


@dataclasses.dataclass(frozen=True)
class ControlCommand:
    session_id: int
    sequence: int
    interactor_id: int
    client_time: float
    opcode: int
    shape: int = 0
    flags: int = 0
    ttl_ms: int = 0
    position: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    half_extent: tuple[float, float, float] = (0.0, 0.0, 0.0)
    linear_velocity: tuple[float, float, float] = (0.0, 0.0, 0.0)
    angular_velocity: tuple[float, float, float] = (0.0, 0.0, 0.0)
    strength: float = 0.0


def _require_unsigned(name: str, value: int, maximum: int, *, allow_zero: bool = True) -> None:
    minimum = 0 if allow_zero else 1
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")


def _require_finite(name: str, values: Iterable[float]) -> tuple[float, ...]:
    result = tuple(values)
    if not all(math.isfinite(value) for value in result):
        raise ValueError(f"{name} must contain only finite values")
    return result


def normalize_quaternion(values: Sequence[float]) -> tuple[float, float, float, float]:
    if len(values) != 4:
        raise ValueError("rotation must contain four values (X Y Z W)")
    rotation = _require_finite("rotation", values)
    length = math.sqrt(sum(value * value for value in rotation))
    if length < 1.0e-6:
        raise ValueError("rotation quaternion must not be zero")
    return tuple(value / length for value in rotation)  # type: ignore[return-value]


def parse_flags(text: str) -> int:
    normalized = text.strip().lower()
    if normalized == "all":
        return SUPPORTED_FLAGS
    try:
        numeric = int(normalized, 0)
    except ValueError:
        numeric = None
    if numeric is not None:
        if numeric <= 0 or numeric & ~SUPPORTED_FLAGS:
            raise argparse.ArgumentTypeError("flags must use only bits 0..2 and cannot be zero")
        return numeric

    result = 0
    for name in (part.strip() for part in normalized.split(",")):
        if name not in FLAG_NAMES:
            choices = ", ".join(FLAG_NAMES)
            raise argparse.ArgumentTypeError(f"unknown flag '{name}'; choose from {choices}")
        result |= FLAG_NAMES[name]
    if result == 0:
        raise argparse.ArgumentTypeError("at least one interactor flag is required")
    return result


def default_half_extent(shape: int) -> tuple[float, float, float]:
    if shape == SHAPE_SPHERE:
        return (0.03, 0.03, 0.03)
    if shape == SHAPE_CAPSULE_Z:
        return (0.025, 0.025, 0.06)
    return (0.06, 0.025, 0.025)


def validate_command(command: ControlCommand) -> None:
    _require_unsigned("session_id", command.session_id, UINT32_MAX, allow_zero=False)
    _require_unsigned("sequence", command.sequence, UINT32_MAX)
    _require_unsigned("interactor_id", command.interactor_id, UINT64_MAX)
    _require_unsigned("ttl_ms", command.ttl_ms, UINT32_MAX)
    if not math.isfinite(command.client_time) or command.client_time < 0.0:
        raise ValueError("client_time must be a nonnegative finite value")
    if command.opcode not in (OPCODE_UPSERT, OPCODE_REMOVE, OPCODE_CLEAR):
        raise ValueError("opcode must be upsert, remove, or clear")

    if command.opcode == OPCODE_CLEAR:
        if command.interactor_id != 0:
            raise ValueError("clear requires interactor_id 0")
    elif command.interactor_id == 0:
        raise ValueError("upsert and remove require a nonzero interactor_id")

    if command.opcode != OPCODE_UPSERT:
        canonical_tail = (
            command.shape == 0
            and command.flags == 0
            and command.ttl_ms == 0
            and command.position == (0.0, 0.0, 0.0)
            and command.rotation == (0.0, 0.0, 0.0, 0.0)
            and command.half_extent == (0.0, 0.0, 0.0)
            and command.linear_velocity == (0.0, 0.0, 0.0)
            and command.angular_velocity == (0.0, 0.0, 0.0)
            and command.strength == 0.0
        )
        if not canonical_tail:
            raise ValueError("remove and clear must zero all interactor-only fields")
        return

    if command.shape not in SHAPE_NAMES.values():
        raise ValueError("unsupported interactor shape")
    if command.flags <= 0 or command.flags & ~SUPPORTED_FLAGS:
        raise ValueError("flags must use only bits 0..2 and cannot be zero")
    if not 50 <= command.ttl_ms <= 2_000:
        raise ValueError("ttl_ms must be in [50, 2000]")

    position = _require_finite("position", command.position)
    rotation = _require_finite("rotation", command.rotation)
    half_extent = _require_finite("half_extent", command.half_extent)
    linear_velocity = _require_finite("linear_velocity", command.linear_velocity)
    angular_velocity = _require_finite("angular_velocity", command.angular_velocity)
    if len(position) != 3 or len(rotation) != 4 or len(half_extent) != 3:
        raise ValueError("position/half_extent need 3 values and rotation needs 4")
    if len(linear_velocity) != 3 or len(angular_velocity) != 3:
        raise ValueError("linear_velocity and angular_velocity need 3 values")
    if not all(0.0 <= value <= 1.0 for value in position):
        raise ValueError("position must be normalized to [0, 1]")
    if not all(value > 0.0 for value in half_extent):
        raise ValueError("half_extent values must be positive")
    bounding_extent = (
        math.sqrt(sum(value * value for value in half_extent))
        if command.shape == SHAPE_BOX
        else max(half_extent)
    )
    if bounding_extent > 0.45:
        raise ValueError("shape bounding extent must be at most 0.45 normalized units")
    if abs(sum(value * value for value in rotation) - 1.0) > 1.0e-3:
        raise ValueError("rotation quaternion must be normalized")
    if math.sqrt(sum(value * value for value in linear_velocity)) > 2.0:
        raise ValueError("linear_velocity magnitude must be at most 2 normalized units/s")
    if not all(abs(value) <= 100.0 for value in angular_velocity):
        raise ValueError("angular_velocity components must be in [-100, 100] rad/s")
    if math.sqrt(sum(value * value for value in angular_velocity)) * bounding_extent > 2.0:
        raise ValueError("angular tip speed must be at most 2 normalized units/s")
    if not math.isfinite(command.strength) or not 0.0 < command.strength <= 4.0:
        raise ValueError("strength must be in (0, 4]")

    tolerance = 1.0e-5
    if command.shape == SHAPE_SPHERE and not (
        abs(half_extent[0] - half_extent[1]) <= tolerance
        and abs(half_extent[0] - half_extent[2]) <= tolerance
    ):
        raise ValueError("sphere requires equal X, Y, and Z half extents")
    if command.shape == SHAPE_CAPSULE_Z and not (
        abs(half_extent[0] - half_extent[1]) <= tolerance
        and half_extent[2] + tolerance >= half_extent[0]
    ):
        raise ValueError("capsule-z requires X=Y radius and Z half-height >= radius")


def pack_command(command: ControlCommand) -> bytes:
    """Validate and serialize one canonical CLC2 datagram."""
    validate_command(command)
    values = (
        MAGIC,
        VERSION,
        PACKET_BYTES,
        command.session_id,
        command.sequence,
        command.interactor_id,
        command.client_time,
        command.opcode,
        command.shape,
        command.flags,
        command.ttl_ms,
        *command.position,
        *command.rotation,
        *command.half_extent,
        *command.linear_velocity,
        *command.angular_velocity,
        command.strength,
        bytes(16),
    )
    without_crc = CONTROL_PACKET.pack(*values, 0)
    checksum = zlib.crc32(without_crc[:CRC_OFFSET]) & UINT32_MAX
    return CONTROL_PACKET.pack(*values, checksum)


def unpack_command(packet: bytes) -> ControlCommand:
    """Strictly validate and decode a CLC2 datagram (primarily for tests/tools)."""
    if len(packet) != PACKET_BYTES:
        raise ValueError(f"CLC2 packet must be exactly {PACKET_BYTES} bytes")
    values = CONTROL_PACKET.unpack(packet)
    if values[0] != MAGIC or values[1] != VERSION or values[2] != PACKET_BYTES:
        raise ValueError("invalid CLC2 magic, version, or packet size")
    if values[-2] != bytes(16):
        raise ValueError("CLC2 reserved bytes must be zero")
    expected_crc = zlib.crc32(packet[:CRC_OFFSET]) & UINT32_MAX
    if values[-1] != expected_crc:
        raise ValueError("CLC2 CRC32 mismatch")
    command = ControlCommand(
        session_id=values[3],
        sequence=values[4],
        interactor_id=values[5],
        client_time=values[6],
        opcode=values[7],
        shape=values[8],
        flags=values[9],
        ttl_ms=values[10],
        position=tuple(values[11:14]),
        rotation=tuple(values[14:18]),
        half_extent=tuple(values[18:21]),
        linear_velocity=tuple(values[21:24]),
        angular_velocity=tuple(values[24:27]),
        strength=values[27],
    )
    validate_command(command)
    return command


def normalized_velocity(
    start: Sequence[float], end: Sequence[float], duration: float
) -> tuple[float, float, float]:
    if len(start) != 3 or len(end) != 3:
        raise ValueError("start and end positions need three values")
    if not math.isfinite(duration) or duration <= 0.0:
        raise ValueError("duration must be a positive finite number")
    return tuple((end[index] - start[index]) / duration for index in range(3))  # type: ignore[return-value]


def next_sequence(sequence: int) -> int:
    return (sequence + 1) & UINT32_MAX


def make_upsert(args: argparse.Namespace, sequence: int, position: Sequence[float], velocity: Sequence[float]) -> ControlCommand:
    shape = SHAPE_NAMES[args.shape]
    half_extent = tuple(args.half_extent) if args.half_extent is not None else default_half_extent(shape)
    return ControlCommand(
        session_id=args.session,
        sequence=sequence,
        interactor_id=args.interactor_id,
        client_time=time.monotonic(),
        opcode=OPCODE_UPSERT,
        shape=shape,
        flags=args.flags,
        ttl_ms=args.ttl_ms,
        position=tuple(position),
        rotation=normalize_quaternion(args.rotation),
        half_extent=half_extent,
        linear_velocity=tuple(velocity),
        angular_velocity=tuple(args.angular_velocity),
        strength=args.strength,
    )


def make_remove_or_clear(args: argparse.Namespace, opcode: int, sequence: int) -> ControlCommand:
    return ControlCommand(
        session_id=args.session,
        sequence=sequence,
        interactor_id=0 if opcode == OPCODE_CLEAR else args.interactor_id,
        client_time=time.monotonic(),
        opcode=opcode,
    )


def send_command(sock: socket.socket, target: tuple[str, int], command: ControlCommand) -> None:
    packet = pack_command(command)
    sent = sock.sendto(packet, target)
    if sent != len(packet):
        raise OSError(f"short UDP send: {sent}/{len(packet)} bytes")


def add_interactor_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--id", dest="interactor_id", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--shape", choices=tuple(SHAPE_NAMES), default="ellipsoid")
    parser.add_argument(
        "--half-extent", type=float, nargs=3, metavar=("X", "Y", "Z"),
        help="normalized half extent; defaults depend on shape",
    )
    parser.add_argument(
        "--rotation", type=float, nargs=4, default=(0.0, 0.0, 0.0, 1.0),
        metavar=("X", "Y", "Z", "W"), help="local-to-volume quaternion",
    )
    parser.add_argument(
        "--angular-velocity", type=float, nargs=3, default=(0.0, 0.0, 0.0),
        metavar=("X", "Y", "Z"), help="radians per second",
    )
    parser.add_argument("--flags", type=parse_flags, default=SUPPORTED_FLAGS)
    parser.add_argument("--ttl-ms", type=int, default=300)
    parser.add_argument("--strength", type=float, default=1.0)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7778)
    parser.add_argument(
        "--source-port", type=int, default=0,
        help="fixed local UDP port; required to continue a session in a later process",
    )
    parser.add_argument(
        "--session", type=lambda value: int(value, 0),
        help="uint32 sender session; default is a new random nonzero value",
    )
    parser.add_argument("--sequence", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--quiet", action="store_true")
    subparsers = parser.add_subparsers(dest="action", required=True)

    upsert = subparsers.add_parser("upsert", help="send one object state")
    add_interactor_arguments(upsert)
    upsert.add_argument(
        "--position", type=float, nargs=3, default=(0.5, 0.5, 0.5),
        metavar=("X", "Y", "Z"), help="normalized volume coordinates",
    )
    upsert.add_argument(
        "--velocity", type=float, nargs=3, default=(0.0, 0.0, 0.0),
        metavar=("X", "Y", "Z"), help="normalized volume units per second",
    )

    animate = subparsers.add_parser("animate", help="send a linear moving-object path")
    add_interactor_arguments(animate)
    animate.add_argument(
        "--start", type=float, nargs=3, default=(0.15, 0.5, 0.5),
        metavar=("X", "Y", "Z"),
    )
    animate.add_argument(
        "--end", type=float, nargs=3, default=(0.85, 0.5, 0.5),
        metavar=("X", "Y", "Z"),
    )
    animate.add_argument("--duration", type=float, default=3.0)
    animate.add_argument("--rate", type=float, default=30.0, help="updates per second")
    animate.add_argument(
        "--keep", action="store_true",
        help="do not send remove at the end (the object still expires by TTL)",
    )

    remove = subparsers.add_parser("remove", help="remove one object immediately")
    remove.add_argument("--id", dest="interactor_id", type=lambda value: int(value, 0), default=1)
    subparsers.add_parser("clear", help="remove all objects in this session")

    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in [1, 65535]")
    if not 0 <= args.source_port <= 65535:
        parser.error("--source-port must be in [0, 65535]")
    if args.session is None:
        args.session = secrets.randbelow(UINT32_MAX) + 1
    try:
        _require_unsigned("session", args.session, UINT32_MAX, allow_zero=False)
        _require_unsigned("sequence", args.sequence, UINT32_MAX)
        if args.action == "animate":
            if not math.isfinite(args.duration) or args.duration <= 0.0:
                raise ValueError("--duration must be a positive finite number")
            if not math.isfinite(args.rate) or not 1.0 <= args.rate <= 240.0:
                raise ValueError("--rate must be in [1, 240]")
    except ValueError as error:
        parser.error(str(error))
    return args


def run(args: argparse.Namespace) -> int:
    target = (args.host, args.port)
    sequence = args.sequence
    packet_count = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if args.source_port:
            sock.bind(("0.0.0.0", args.source_port))
        if args.action == "upsert":
            command = make_upsert(args, sequence, args.position, args.velocity)
            send_command(sock, target, command)
            packet_count = 1
        elif args.action == "remove":
            command = make_remove_or_clear(args, OPCODE_REMOVE, sequence)
            send_command(sock, target, command)
            packet_count = 1
        elif args.action == "clear":
            command = make_remove_or_clear(args, OPCODE_CLEAR, sequence)
            send_command(sock, target, command)
            packet_count = 1
        else:
            start = tuple(args.start)
            end = tuple(args.end)
            velocity = normalized_velocity(start, end, args.duration)
            sample_times = [index / args.rate for index in range(math.ceil(args.duration * args.rate) + 1)]
            sample_times[-1] = args.duration
            start_clock = time.monotonic()
            for sample_time in sample_times:
                delay = start_clock + sample_time - time.monotonic()
                if delay > 0.0:
                    time.sleep(delay)
                fraction = sample_time / args.duration
                position = tuple(
                    start[index] + (end[index] - start[index]) * fraction for index in range(3)
                )
                command = make_upsert(args, sequence, position, velocity)
                send_command(sock, target, command)
                packet_count += 1
                sequence = next_sequence(sequence)
            if not args.keep:
                command = make_remove_or_clear(args, OPCODE_REMOVE, sequence)
                send_command(sock, target, command)
                packet_count += 1

    if not args.quiet:
        print(
            f"sent {packet_count} CLC2 packet(s) to {args.host}:{args.port} "
            f"session={args.session} first_sequence={args.sequence}"
        )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        return run(args)
    except (OSError, ValueError) as error:
        print(f"send_interactor: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
