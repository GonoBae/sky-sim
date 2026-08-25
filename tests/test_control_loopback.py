#!/usr/bin/env python3
"""End-to-end CLC2 loopback test against a built cloud_sim_server."""

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import send_interactor as control  # noqa: E402


def reserve_udp_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def make_upsert(session_id, sequence, interactor_id, client_time, position):
    return control.ControlCommand(
        session_id=session_id,
        sequence=sequence,
        interactor_id=interactor_id,
        client_time=client_time,
        opcode=control.OPCODE_UPSERT,
        shape=control.SHAPE_ELLIPSOID,
        flags=control.SUPPORTED_FLAGS,
        ttl_ms=2000,
        position=position,
        rotation=(0.0, 0.0, 0.0, 1.0),
        half_extent=(0.10, 0.04, 0.03),
        linear_velocity=(0.40, 0.0, 0.0),
        angular_velocity=(0.0, 0.0, 0.0),
        strength=2.0,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--grid", type=int, default=24)
    args = parser.parse_args()
    control_port = reserve_udp_port()
    process = subprocess.Popen(
        [
            args.server,
            "--grid", str(args.grid),
            "--hz", "20",
            "--send-hz", "10",
            "--warmup-seconds", "0",
            "--seconds", "3.6",
            "--no-send",
            "--control-port", str(control_port),
            "--max-interactors", "1",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    session_id = 0x51337A11
    sequence = 1
    started = time.monotonic()
    sent_updates = 0
    capacity_attempts = 40
    last_upsert_packet = None
    try:
        time.sleep(0.25)
        while time.monotonic() - started < 1.4:
            elapsed = time.monotonic() - started
            t = min(1.0, elapsed / 2.0)
            position = (0.20 + 0.60 * t, 0.50, 0.22)
            command = make_upsert(
                session_id, sequence, 0xA17C4AF7, elapsed, position
            )
            packet = control.pack_command(command)
            sender.sendto(packet, ("127.0.0.1", control_port))
            sent_updates += 1
            last_upsert_packet = packet
            if sent_updates == 10:
                sender.sendto(packet, ("127.0.0.1", control_port))
            if sent_updates == 11:
                damaged = bytearray(packet)
                damaged[80] ^= 0x01
                sender.sendto(bytes(damaged), ("127.0.0.1", control_port))
            sequence = control.next_sequence(sequence)
            time.sleep(1.0 / 40.0)

        # POSIX reports this as a truncated packet; Winsock reports
        # WSAEMSGSIZE. In either case it must be rejected without terminating
        # the server, as demonstrated by the valid commands that follow.
        sender.sendto(
            b"oversized-control-packet" * 100,
            ("127.0.0.1", control_port),
        )

        # A remove or clear for a session not yet present still establishes a
        # sequence tombstone. An upsert at the same or an older sequence must
        # not resurrect that session.
        tombstone_remove_session = 0x51337B01
        unknown_remove = control.ControlCommand(
            session_id=tombstone_remove_session,
            sequence=100,
            interactor_id=0xA17C4B01,
            client_time=time.monotonic() - started,
            opcode=control.OPCODE_REMOVE,
        )
        control.send_command(sender, ("127.0.0.1", control_port), unknown_remove)
        stale_after_remove = make_upsert(
            tombstone_remove_session,
            100,
            0xA17C4B01,
            time.monotonic() - started,
            (0.40, 0.50, 0.22),
        )
        control.send_command(sender, ("127.0.0.1", control_port), stale_after_remove)

        tombstone_clear_session = 0x51337B02
        unknown_clear = control.ControlCommand(
            session_id=tombstone_clear_session,
            sequence=200,
            interactor_id=0,
            client_time=time.monotonic() - started,
            opcode=control.OPCODE_CLEAR,
        )
        control.send_command(sender, ("127.0.0.1", control_port), unknown_clear)
        stale_after_clear = make_upsert(
            tombstone_clear_session,
            199,
            0xA17C4B02,
            time.monotonic() - started,
            (0.45, 0.50, 0.22),
        )
        control.send_command(sender, ("127.0.0.1", control_port), stale_after_clear)

        # The primary interactor fills the configured capacity. Rejected
        # upserts from many new sessions must not leave empty Session entries;
        # otherwise they exhaust the 32-session registry and prevent the
        # survivor session below from being admitted after capacity is freed.
        for attempt in range(capacity_attempts):
            rejected_upsert = make_upsert(
                0x60000000 + attempt,
                1,
                0xB0000000 + attempt,
                time.monotonic() - started,
                (0.50, 0.50, 0.22),
            )
            control.send_command(
                sender, ("127.0.0.1", control_port), rejected_upsert
            )

        remove = control.ControlCommand(
            session_id=session_id,
            sequence=sequence,
            interactor_id=0xA17C4AF7,
            client_time=time.monotonic() - started,
            opcode=control.OPCODE_REMOVE,
        )
        control.send_command(sender, ("127.0.0.1", control_port), remove)
        if last_upsert_packet is not None:
            sender.sendto(last_upsert_packet, ("127.0.0.1", control_port))

        time.sleep(0.15)
        survivor_session = 0x51337C01
        survivor = make_upsert(
            survivor_session,
            1,
            0xA17C4C01,
            time.monotonic() - started,
            (0.65, 0.50, 0.22),
        )
        control.send_command(sender, ("127.0.0.1", control_port), survivor)
        while time.monotonic() - started < 2.35:
            time.sleep(0.02)
        survivor_remove = control.ControlCommand(
            session_id=survivor_session,
            sequence=2,
            interactor_id=0xA17C4C01,
            client_time=time.monotonic() - started,
            opcode=control.OPCODE_REMOVE,
        )
        control.send_command(sender, ("127.0.0.1", control_port), survivor_remove)
        output, _ = process.communicate(timeout=6.0)
    except Exception:
        process.terminate()
        output, _ = process.communicate(timeout=3.0)
        print(output)
        raise
    finally:
        sender.close()

    if process.returncode != 0:
        print(output)
        raise RuntimeError(f"server exited with {process.returncode}")
    accepted = [int(value) for value in re.findall(r"control\[ok=(\d+)", output)]
    interactions = [
        tuple(int(value) for value in match)
        for match in re.findall(
            r"interaction\[active=(\d+), velocity=(\d+), scalar=(\d+)\]", output
        )
    ]
    stopped_times = [
        float(value) for value in re.findall(r"stopped at t=([0-9.]+)s", output)
    ]
    stale = [int(value) for value in re.findall(r"stale=(\d+)", output)]
    rejected = [int(value) for value in re.findall(r"rejected=(\d+)", output)]
    limited = [int(value) for value in re.findall(r"limited=(\d+)", output)]
    if not accepted or max(accepted) < 20:
        print(output)
        raise RuntimeError("server did not accept the CLC2 update stream")
    if not interactions or max(active for active, _, _ in interactions) < 1:
        print(output)
        raise RuntimeError("interactor never became active")
    if max(velocity for _, velocity, _ in interactions) == 0:
        print(output)
        raise RuntimeError("interactor did not affect velocity cells")
    if max(scalar for _, _, scalar in interactions) == 0:
        print(output)
        raise RuntimeError("interactor did not displace scalar cells")
    if not stale or max(stale) < 4:
        print(output)
        raise RuntimeError("unknown-session tombstones did not reject stale upserts")
    if not limited or max(limited) < capacity_attempts:
        print(output)
        raise RuntimeError("new-session capacity attempts were not rejected")
    if not rejected or max(rejected) < capacity_attempts + 6:
        print(output)
        raise RuntimeError("oversized, duplicate, stale, or corrupt packets were not rejected")
    if len(interactions) < 3 or interactions[-2][0] != 1:
        print(output)
        raise RuntimeError(
            "rejected capacity attempts left sessions behind and blocked a later session"
        )
    if interactions[-1][0] != 0:
        print(output)
        raise RuntimeError("remove did not deactivate the interactor")
    print(
        "CLC2 loopback passed:",
        f"accepted={max(accepted)}",
        f"max_interaction={max(interactions)}",
        f"stale={max(stale)}",
        f"rejected={max(rejected)}",
        f"limited={max(limited)}",
        f"simulation_time={max(stopped_times) if stopped_times else 'unknown'}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
