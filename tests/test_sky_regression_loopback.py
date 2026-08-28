#!/usr/bin/env python3
"""Short end-to-end regressions for SKS1 evolution and cloud-layer compaction."""

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import receive_sky as state_protocol  # noqa: E402
from tools import send_sky_control as control  # noqa: E402


def reserve_udp_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def receive_state(receiver, deadline):
    while time.monotonic() < deadline:
        try:
            packet, _ = receiver.recvfrom(65535)
        except socket.timeout:
            continue
        return state_protocol.parse_packet(packet)
    raise RuntimeError("timed out waiting for a valid SKS1 state")


def send_and_wait_for_ack(
    sender, receiver, target, command, timeout=0.65, expected_result=1
):
    packet = control.pack_command(command)
    deadline = time.monotonic() + timeout
    next_send = 0.0
    states = []
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            sender.sendto(packet, target)
            next_send = now + 0.08
        try:
            state = receive_state(receiver, min(deadline, now + 0.10))
        except RuntimeError:
            continue
        states.append(state)
        ack = state["acknowledgement"]
        if (
            ack["last_control_session"] == command.session_id
            and ack["last_control_sequence"] == command.sequence
            and ack["last_control_result"] == expected_result
        ):
            return state, states
    raise RuntimeError(
        f"SKC1 sequence {command.sequence} did not return result {expected_result}"
    )


def collect_states(receiver, duration):
    deadline = time.monotonic() + duration
    states = []
    while time.monotonic() < deadline:
        try:
            state = receive_state(receiver, deadline)
        except RuntimeError:
            break
        states.append(state)
    if not states:
        raise RuntimeError("no SKS1 states arrived during weather transition")
    return states


def preset_command(session_id, sequence, preset, transition_ms):
    return control.SkyControlCommand(
        session_id=session_id,
        sequence=sequence,
        client_time_seconds=time.monotonic(),
        opcode=control.OPCODE_LOAD_PRESET,
        evolution_mode=(
            control.EVOLUTION_NATURAL
            if preset == control.PRESET_NATURAL
            else control.EVOLUTION_MANUAL
        ),
        transition_milliseconds=transition_ms,
        preset=preset,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    state_port = reserve_udp_port()
    control_port = reserve_udp_port()
    while control_port == state_port:
        control_port = reserve_udp_port()
    interactor_control_port = reserve_udp_port()
    while interactor_control_port in (state_port, control_port):
        interactor_control_port = reserve_udp_port()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", state_port))
    receiver.settimeout(0.08)
    process = subprocess.Popen(
        [
            args.server,
            "--grid", "16",
            "--hz", "30",
            "--send-hz", "10",
            "--warmup-seconds", "0",
            "--seconds", "2.8",
            "--no-send",
            "--control-host", "127.0.0.1",
            "--control-port", str(interactor_control_port),
            "--sky-host", "127.0.0.1",
            "--sky-port", str(state_port),
            "--sky-hz", "30",
            "--sky-control-host", "127.0.0.1",
            "--sky-control-port", str(control_port),
            "--weather", "natural",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = ("127.0.0.1", control_port)
    session_id = 0x52454731
    parsed_states = 0
    transition_states = 0
    try:
        initial = receive_state(receiver, time.monotonic() + 0.75)
        parsed_states += 1
        if initial["model"]["evolution_mode_name"] != "natural":
            raise RuntimeError("server did not start in natural evolution mode")

        # The command carries the generic/manual default deliberately. A
        # keyframe request must not mutate the server's evolution mode.
        keyframe = control.SkyControlCommand(
            session_id=session_id,
            sequence=1,
            client_time_seconds=time.monotonic(),
            opcode=control.OPCODE_REQUEST_KEYFRAME,
            evolution_mode=control.EVOLUTION_MANUAL,
            transition_milliseconds=0,
            preset=control.PRESET_NATURAL,
        )
        state, seen = send_and_wait_for_ack(
            sender, receiver, target, keyframe
        )
        parsed_states += len(seen)
        if state["model"]["evolution_mode_name"] != "natural":
            raise RuntimeError("keyframe request changed natural evolution mode")

        utc_patch = control.SkyControlCommand(
            session_id=session_id,
            sequence=2,
            client_time_seconds=time.monotonic(),
            opcode=control.OPCODE_PATCH_OVERRIDE,
            evolution_mode=control.EVOLUTION_MANUAL,
            transition_milliseconds=0,
            apply_mask=control.APPLY_UTC,
            utc_unix_seconds=1787648400.0,
        )
        state, seen = send_and_wait_for_ack(
            sender, receiver, target, utc_patch
        )
        parsed_states += len(seen)
        if state["model"]["evolution_mode_name"] != "natural":
            raise RuntimeError("UTC-only patch changed natural evolution mode")

        clear = preset_command(session_id, 3, control.PRESET_CLEAR, 0)
        clear_state, seen = send_and_wait_for_ack(
            sender, receiver, target, clear
        )
        parsed_states += len(seen)
        if len(clear_state["cloud_layers"]) != 1:
            raise RuntimeError("clear preset no longer has the expected one layer")

        storm = preset_command(session_id, 4, control.PRESET_STORM, 300)
        storm_ack, seen = send_and_wait_for_ack(
            sender, receiver, target, storm
        )
        parsed_states += len(seen)
        storm_transition = [storm_ack, *collect_states(receiver, 0.42)]
        parsed_states += len(storm_transition) - 1
        transition_states += len(storm_transition)
        if len(storm_transition) < 5:
            raise RuntimeError("too few strict SKS1 samples during clear-to-storm")
        if len(storm_transition[-1]["cloud_layers"]) != 2:
            raise RuntimeError("clear-to-storm transition did not finish with two layers")

        back_to_clear = preset_command(session_id, 5, control.PRESET_CLEAR, 300)
        clear_ack, seen = send_and_wait_for_ack(
            sender, receiver, target, back_to_clear
        )
        parsed_states += len(seen)
        clear_transition = [clear_ack, *collect_states(receiver, 0.42)]
        parsed_states += len(clear_transition) - 1
        transition_states += len(clear_transition)
        if len(clear_transition) < 5:
            raise RuntimeError("too few strict SKS1 samples during storm-to-clear")
        if len(clear_transition[-1]["cloud_layers"]) != 1:
            raise RuntimeError("storm-to-clear transition did not finish with one layer")

        # Slot indices address the current contiguous list. With one active
        # clear-sky layer, sparse slot 3 must be rejected at apply time while
        # still producing a strict, parseable SKS1 acknowledgement.
        sparse_layer = control.CloudLayer(
            kind="convective",
            base_altitude_amsl_m=1600.0,
            top_altitude_amsl_m=4600.0,
            coverage=0.73,
            optical_depth=30.0,
            liquid_fraction=0.9,
            precipitation_rate_mm_h=1.5,
            convective_activity=0.58,
        )
        layer_patch = control.SkyControlCommand(
            session_id=session_id,
            sequence=6,
            client_time_seconds=time.monotonic(),
            opcode=control.OPCODE_PATCH_OVERRIDE,
            transition_milliseconds=0,
            apply_mask=control.APPLY_CLOUD_LAYER_3,
            cloud_layers=(None, None, None, sparse_layer),
        )
        layer_state, seen = send_and_wait_for_ack(
            sender, receiver, target, layer_patch, expected_result=2
        )
        parsed_states += len(seen)
        layers = layer_state["cloud_layers"]
        if len(layers) != 1:
            raise RuntimeError("rejected sparse layer patch changed the active layer count")
        if any(layer["top_amsl_m"] <= layer["base_amsl_m"] for layer in layers):
            raise RuntimeError("sparse layer patch left an empty active slot")
        if any(abs(layer["base_amsl_m"] - 1600.0) <= 1.0e-3 for layer in layers):
            raise RuntimeError("rejected sparse layer unexpectedly changed SKS1")

        # Replacing the same valid list index repeatedly must not append a
        # layer or otherwise increase cloud_layer_count.
        replacement_a = control.CloudLayer(
            kind="stratiform",
            base_altitude_amsl_m=1100.0,
            top_altitude_amsl_m=2600.0,
            coverage=0.41,
            optical_depth=18.0,
            liquid_fraction=0.95,
        )
        replace_once = control.SkyControlCommand(
            session_id=session_id,
            sequence=7,
            client_time_seconds=time.monotonic(),
            opcode=control.OPCODE_PATCH_OVERRIDE,
            transition_milliseconds=0,
            apply_mask=control.APPLY_CLOUD_LAYER_0,
            cloud_layers=(replacement_a, None, None, None),
        )
        first_replace_state, seen = send_and_wait_for_ack(
            sender, receiver, target, replace_once
        )
        parsed_states += len(seen)
        if len(first_replace_state["cloud_layers"]) != 1:
            raise RuntimeError("first valid layer replacement changed layer count")

        replacement_b = control.CloudLayer(
            kind="convective",
            base_altitude_amsl_m=1400.0,
            top_altitude_amsl_m=4200.0,
            coverage=0.67,
            optical_depth=28.0,
            liquid_fraction=0.88,
            convective_activity=0.52,
        )
        replace_again = control.SkyControlCommand(
            session_id=session_id,
            sequence=8,
            client_time_seconds=time.monotonic(),
            opcode=control.OPCODE_PATCH_OVERRIDE,
            transition_milliseconds=0,
            apply_mask=control.APPLY_CLOUD_LAYER_0,
            cloud_layers=(replacement_b, None, None, None),
        )
        second_replace_state, seen = send_and_wait_for_ack(
            sender, receiver, target, replace_again
        )
        parsed_states += len(seen)
        replaced_layers = second_replace_state["cloud_layers"]
        if len(replaced_layers) != 1:
            raise RuntimeError("repeated valid layer replacement increased layer count")
        if (
            abs(replaced_layers[0]["base_amsl_m"] - 1400.0) > 1.0e-3
            or abs(replaced_layers[0]["top_amsl_m"] - 4200.0) > 1.0e-3
            or abs(replaced_layers[0]["coverage"] - 0.67) > 1.0e-5
        ):
            raise RuntimeError("repeated valid layer replacement was not reflected in SKS1")

        output, _ = process.communicate(timeout=5.0)
    except Exception:
        process.terminate()
        output, _ = process.communicate(timeout=3.0)
        print(output)
        raise
    finally:
        sender.close()
        receiver.close()

    if process.returncode != 0:
        print(output)
        raise RuntimeError(f"server exited with {process.returncode}")
    accepted = [int(value) for value in re.findall(r"sky_control\[ok=(\d+)", output)]
    if not accepted or max(accepted) < 8:
        print(output)
        raise RuntimeError("server did not receive all eight regression commands")
    print(
        "SKS1 regression loopback passed:",
        f"parsed_states={parsed_states}",
        f"transition_states={transition_states}",
        f"accepted={max(accepted)}",
        "natural_mode=preserved",
        "sparse_layer=rejected",
        "repeated_replace=count-stable",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
