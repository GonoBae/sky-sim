#!/usr/bin/env python3
"""End-to-end SKS1/SKC1 loopback test against a built cloud_sim_server."""

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
    receiver.settimeout(0.25)
    process = subprocess.Popen(
        [
            args.server,
            "--grid", "24",
            "--hz", "20",
            "--send-hz", "10",
            "--warmup-seconds", "0",
            "--seconds", "2.2",
            "--no-send",
            "--control-host", "127.0.0.1",
            "--control-port", str(interactor_control_port),
            "--sky-host", "127.0.0.1",
            "--sky-port", str(state_port),
            "--sky-hz", "10",
            "--sky-control-host", "127.0.0.1",
            "--sky-control-port", str(control_port),
            "--weather", "clear",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    session_id = 0x514B5931
    layer = control.CloudLayer(
        kind="convective",
        base_altitude_amsl_m=1500.0,
        top_altitude_amsl_m=4500.0,
        coverage=0.71,
        optical_depth=32.0,
        liquid_fraction=0.85,
        precipitation_rate_mm_h=3.0,
        convective_activity=0.62,
    )
    command = control.SkyControlCommand(
        session_id=session_id,
        sequence=1,
        client_time_seconds=1.0,
        opcode=control.OPCODE_PATCH_OVERRIDE,
        transition_milliseconds=0,
        apply_mask=(
            control.APPLY_UTC
            | control.APPLY_LOCATION
            | control.APPLY_DOMAIN
            | control.APPLY_TIME_SCALE
            | control.APPLY_EVOLUTION
            | control.APPLY_THERMODYNAMICS
            | control.APPLY_VISIBILITY
            | control.APPLY_WIND
            | control.APPLY_PRECIPITATION
            | control.APPLY_CONVECTION
            | control.APPLY_CLOUD_LAYER_0
        ),
        weather_seed=33,
        utc_unix_seconds=1787648400.0,
        latitude_degrees=35.0,
        longitude_degrees=129.0,
        elevation_m=123.0,
        time_scale=0.0,
        horizontal_extent_m=20000.0,
        vertical_extent_m=12000.0,
        surface_temperature_kelvin=297.15,
        sea_level_pressure_pa=100400.0,
        relative_humidity=0.82,
        visibility_m=12000.0,
        aerosol_optical_depth_550nm=0.15,
        ozone_dobson_units=310.0,
        wind_enu_m_s=(8.0, 2.0, 0.0),
        gust_speed_m_s=13.0,
        precipitation_rate_mm_h=18.0,
        snow_fraction=0.25,
        surface_wetness=0.6,
        convective_activity=0.7,
        lightning_activity=0.2,
        cloud_layers=(layer, None, None, None),
    )
    command_packet = control.pack_command(command)
    deadline = time.monotonic() + 1.7
    next_send = time.monotonic() + 0.15
    valid_states = 0
    state_sequences = []
    acknowledged = None
    try:
        while time.monotonic() < deadline and acknowledged is None:
            now = time.monotonic()
            if now >= next_send:
                sender.sendto(command_packet, ("127.0.0.1", control_port))
                next_send = now + 0.10
            try:
                packet, _ = receiver.recvfrom(65535)
            except socket.timeout:
                continue
            state = state_protocol.parse_packet(packet)
            valid_states += 1
            state_sequences.append(state["sequence"])
            ack = state["acknowledgement"]
            if (
                ack["last_control_session"] == session_id
                and ack["last_control_sequence"] == 1
                and ack["last_control_result"] == 1
            ):
                acknowledged = state
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
    if valid_states == 0 or acknowledged is None:
        print(output)
        raise RuntimeError("no validated SKS1 state acknowledged the SKC1 command")
    sequence_steps = [
        (current - previous) & 0xFFFFFFFF
        for previous, current in zip(state_sequences, state_sequences[1:])
    ]
    minimum_consecutive = max(1, len(sequence_steps) // 2)
    if (
        not sequence_steps
        or sum(step == 1 for step in sequence_steps) < minimum_consecutive
    ):
        raise RuntimeError("SKS1 state_sequence is not advancing once per transmitted datagram")

    location = acknowledged["location"]
    weather = acknowledged["weather"]
    layers = acknowledged["cloud_layers"]
    if abs(location["latitude_degrees"] - 35.0) > 1.0e-8:
        raise RuntimeError("SKC1 latitude was not reflected in SKS1")
    if abs(location["longitude_degrees"] - 129.0) > 1.0e-8:
        raise RuntimeError("SKC1 longitude was not reflected in SKS1")
    if abs(location["elevation_m"] - 123.0) > 1.0e-4:
        raise RuntimeError("SKC1 elevation was not reflected in SKS1")
    if abs(weather["wind_speed_m_s"] - (68.0 ** 0.5)) > 1.0e-4:
        raise RuntimeError("SKC1 ENU wind was not reflected in SKS1")
    if abs(weather["surface_temperature_kelvin"] - 297.15) > 1.0e-3:
        raise RuntimeError("SKC1 thermodynamics were not reflected in SKS1")
    if abs(weather["relative_humidity"] - 0.82) > 1.0e-5:
        raise RuntimeError("SKC1 humidity was not reflected in SKS1")
    if abs(weather["precipitation_rate_mm_h"] - 18.0) > 1.0e-3:
        raise RuntimeError("SKC1 precipitation was not reflected in SKS1")
    if not layers:
        raise RuntimeError("SKC1 cloud layer was not reflected in SKS1")
    if abs(layers[0]["base_amsl_m"] - 1500.0) > 1.0e-3:
        raise RuntimeError("SKC1 AMSL cloud base changed across the server")
    if abs(layers[0]["top_amsl_m"] - 4500.0) > 1.0e-3:
        raise RuntimeError("SKC1 AMSL cloud top changed across the server")
    if abs(layers[0]["coverage"] - 0.71) > 1.0e-5:
        raise RuntimeError("SKC1 cloud coverage was not reflected in SKS1")

    accepted = [int(value) for value in re.findall(r"sky_control\[ok=(\d+)", output)]
    if not accepted or max(accepted) < 1:
        print(output)
        raise RuntimeError("server did not record an accepted SKC1 packet")
    print(
        "SKS1/SKC1 loopback passed:",
        f"states={valid_states}",
        f"accepted={max(accepted)}",
        f"ack_session=0x{session_id:08x}",
        f"cloud={layers[0]['base_amsl_m']:.0f}-{layers[0]['top_amsl_m']:.0f}m AMSL",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
