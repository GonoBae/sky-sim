#!/usr/bin/env python3
"""Receive, strictly validate, and describe one SKS1 sky-state datagram.

The implementation uses only the Python standard library and deliberately
decodes fields by their documented byte offsets.  It is therefore also a
small interoperability reference for an Unreal receiver.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import socket
import struct
import sys
import time
import zlib
from typing import Any, Sequence


MAGIC = b"SKS1"
VERSION = 1
PACKET_BYTES = 512
CRC_OFFSET = 508
SUPPORTED_CLOUD_FIELD_MASK = 0x001F
MAXIMUM_CLOUD_LAYERS = 4

EVOLUTION_NAMES = {
    1: "natural",
    2: "timeline",
    3: "manual",
    4: "replay",
}
CONTROL_RESULT_NAMES = {
    0: "none",
    1: "applied",
    2: "rejected",
}
CLOUD_KIND_NAMES = {
    1: "stratiform",
    2: "convective",
    3: "deep-convective",
    4: "cirrus",
    5: "other",
}
SKY_FLAG_NAMES = {
    1 << 0: "sun-above-horizon",
    1 << 1: "moon-above-horizon",
    1 << 2: "precipitation",
    1 << 3: "fog",
    1 << 4: "lightning-flash",
    1 << 5: "snow",
}
CLOUD_FLAG_NAMES = {
    1 << 0: "convective",
    1 << 1: "precipitating",
    1 << 2: "electrified",
}


def _u8(packet: bytes, offset: int) -> int:
    return packet[offset]


def _u16(packet: bytes, offset: int) -> int:
    return struct.unpack_from("<H", packet, offset)[0]


def _u32(packet: bytes, offset: int) -> int:
    return struct.unpack_from("<I", packet, offset)[0]


def _f32(packet: bytes, offset: int) -> float:
    return struct.unpack_from("<f", packet, offset)[0]


def _f64(packet: bytes, offset: int) -> float:
    return struct.unpack_from("<d", packet, offset)[0]


def _vec3(packet: bytes, offset: int) -> tuple[float, float, float]:
    return struct.unpack_from("<3f", packet, offset)


def _rgb(packet: bytes, offset: int) -> tuple[float, float, float]:
    return _vec3(packet, offset)


def _as_f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _length(vector: Sequence[float]) -> float:
    return _as_f32(math.sqrt(sum(float(value) * float(value) for value in vector)))


def _require_finite(name: str, values: Sequence[float]) -> None:
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"{name} contains a non-finite value")


def _require_range(name: str, value: float, minimum: float, maximum: float) -> None:
    if not minimum <= value <= maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")


def _flag_names(value: int, names: dict[int, str]) -> list[str]:
    result = [name for bit, name in names.items() if value & bit]
    known = sum(names)
    if value & ~known:
        result.append(f"unknown:0x{value & ~known:x}")
    return result


def _utc_iso8601(unix_seconds: float) -> str | None:
    try:
        value = dt.datetime.fromtimestamp(unix_seconds, tz=dt.timezone.utc)
    except (OverflowError, OSError, ValueError):
        return None
    return value.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _parse_cloud_layer(packet: bytes, index: int, elevation_m: float) -> dict[str, Any]:
    offset = 224 + index * 32
    base_amsl_m = _f32(packet, offset)
    top_amsl_m = _f32(packet, offset + 4)
    coverage = _f32(packet, offset + 8)
    condensate_kg_m3 = _f32(packet, offset + 12)
    ice_fraction = _f32(packet, offset + 16)
    precipitation_flux_kg_m2_s = _f32(packet, offset + 20)
    turbulence_m_s = _f32(packet, offset + 24)
    kind = _u8(packet, offset + 28)
    flags = _u8(packet, offset + 29)

    _require_finite(
        f"cloud layer {index}",
        (
            base_amsl_m,
            top_amsl_m,
            coverage,
            condensate_kg_m3,
            ice_fraction,
            precipitation_flux_kg_m2_s,
            turbulence_m_s,
        ),
    )
    if packet[offset + 30:offset + 32] != b"\0\0":
        raise ValueError(f"cloud layer {index} reserved bytes must be zero")
    if top_amsl_m <= base_amsl_m:
        raise ValueError(f"cloud layer {index} top must be above its base")
    if base_amsl_m < elevation_m or top_amsl_m > 200000.0:
        raise ValueError(
            f"cloud layer {index} must be above the volume floor and below 200 km"
        )
    _require_range(f"cloud layer {index} coverage", coverage, 0.0, 1.0)
    _require_range(
        f"cloud layer {index} condensate", condensate_kg_m3, 0.0, 0.01
    )
    _require_range(f"cloud layer {index} ice fraction", ice_fraction, 0.0, 1.0)
    _require_range(
        f"cloud layer {index} precipitation flux",
        precipitation_flux_kg_m2_s,
        0.0,
        _as_f32(300.0 / 3600.0),
    )
    _require_range(f"cloud layer {index} turbulence", turbulence_m_s, 0.25, 4.25)
    if kind not in CLOUD_KIND_NAMES:
        raise ValueError(f"cloud layer {index} has an invalid cloud kind")
    if flags & ~sum(CLOUD_FLAG_NAMES):
        raise ValueError(f"cloud layer {index} flags contain reserved bits")

    return {
        "index": index,
        "kind": CLOUD_KIND_NAMES[kind],
        "kind_id": kind,
        "flags": flags,
        "flag_names": _flag_names(flags, CLOUD_FLAG_NAMES),
        "base_amsl_m": base_amsl_m,
        "top_amsl_m": top_amsl_m,
        "base_agl_m": base_amsl_m - elevation_m,
        "top_agl_m": top_amsl_m - elevation_m,
        "coverage": coverage,
        "condensate_kg_m3": condensate_kg_m3,
        "ice_fraction": ice_fraction,
        "precipitation_flux_kg_m2_s": precipitation_flux_kg_m2_s,
        "precipitation_rate_mm_h": precipitation_flux_kg_m2_s * 3600.0,
        "turbulence_m_s": turbulence_m_s,
    }


def parse_packet(packet: bytes) -> dict[str, Any]:
    """Strictly validate and decode exactly one SKS1 packet."""
    if len(packet) != PACKET_BYTES:
        raise ValueError(f"SKS1 packet must be exactly {PACKET_BYTES} bytes")
    if packet[:4] != MAGIC or _u16(packet, 4) != VERSION or _u16(packet, 6) != PACKET_BYTES:
        raise ValueError("invalid SKS1 magic, version, or packet size")
    if packet[51] != 0 or packet[504:508] != bytes(4):
        raise ValueError("SKS1 reserved header/trailer bytes must be zero")
    expected_crc = zlib.crc32(packet[:CRC_OFFSET]) & 0xFFFFFFFF
    encoded_crc = _u32(packet, CRC_OFFSET)
    if encoded_crc != expected_crc:
        raise ValueError("SKS1 CRC32 mismatch")

    sequence = _u32(packet, 8)
    last_control_sequence = _u32(packet, 12)
    last_control_session = _u32(packet, 16)
    volume_frame_id = _u32(packet, 20)
    fluid_time_seconds = _f64(packet, 24)
    predicted_valid_time_seconds = _f64(packet, 32)
    flags = _u32(packet, 40)
    weather_seed = _u32(packet, 44)
    evolution_mode = _u8(packet, 48)
    last_control_result = _u8(packet, 49)
    cloud_layer_count = _u8(packet, 50)
    weather_model_revision = _u32(packet, 52)
    lightning_event_id = _u32(packet, 56)
    supported_cloud_field_mask = _u16(packet, 60)
    active_cloud_field_mask = _u16(packet, 62)

    utc_unix_seconds = _f64(packet, 64)
    latitude_degrees = _f64(packet, 72)
    longitude_degrees = _f64(packet, 80)
    elevation_m = _f32(packet, 88)
    time_scale = _f32(packet, 92)
    horizontal_extent_x_m = _f32(packet, 96)
    horizontal_extent_y_m = _f32(packet, 100)
    vertical_extent_m = _f32(packet, 104)
    temperature_kelvin = _f32(packet, 108)
    pressure_pa = _f32(packet, 112)
    relative_humidity = _f32(packet, 116)
    visibility_m = _f32(packet, 120)
    aerosol_optical_depth = _f32(packet, 124)
    ozone_dobson_units = _f32(packet, 128)
    wind_m_s = _vec3(packet, 132)
    gust_delta_m_s = _vec3(packet, 144)
    boundary_layer_height_m = _f32(packet, 156)
    precipitation_flux_kg_m2_s = _f32(packet, 160)
    rain_fraction = _f32(packet, 164)
    snow_fraction = _f32(packet, 168)
    hail_fraction = _f32(packet, 172)
    surface_wetness = _f32(packet, 176)
    snow_water_equivalent_kg_m2 = _f32(packet, 180)
    cape_j_kg = _f32(packet, 184)
    cin_j_kg = _f32(packet, 188)
    lightning_rate_hz = _f32(packet, 192)
    ground_albedo = _rgb(packet, 196)
    mie_anisotropy = _f32(packet, 208)
    mie_scale_height_m = _f32(packet, 212)
    rayleigh_scale_height_m = _f32(packet, 216)
    temperature_lapse_rate_k_m = _f32(packet, 220)

    sun_direction_enu = _vec3(packet, 352)
    sun_angular_radius_rad = _f32(packet, 364)
    sun_irradiance_w_m2 = _f32(packet, 368)
    sun_direct_illuminance_lux = _f32(packet, 372)
    moon_direction_enu = _vec3(packet, 376)
    moon_angular_radius_rad = _f32(packet, 388)
    moon_illuminated_fraction = _f32(packet, 392)
    moon_illuminance_lux = _f32(packet, 396)
    celestial_to_enu_xyzw = struct.unpack_from("<4f", packet, 400)
    local_sidereal_angle_rad = _f32(packet, 416)
    geomagnetic_kp = _f32(packet, 420)
    aurora_intensity = _f32(packet, 424)
    star_radiance_scale = _f32(packet, 428)
    rayleigh_scattering_per_m = _rgb(packet, 432)
    mie_scattering_per_m = _rgb(packet, 444)
    mie_absorption_per_m = _rgb(packet, 456)
    ozone_absorption_per_m = _rgb(packet, 468)
    sun_color_linear = _rgb(packet, 480)
    moon_color_linear = _rgb(packet, 492)

    all_scalars = (
        fluid_time_seconds,
        predicted_valid_time_seconds,
        utc_unix_seconds,
        latitude_degrees,
        longitude_degrees,
        elevation_m,
        time_scale,
        horizontal_extent_x_m,
        horizontal_extent_y_m,
        vertical_extent_m,
        temperature_kelvin,
        pressure_pa,
        relative_humidity,
        visibility_m,
        aerosol_optical_depth,
        ozone_dobson_units,
        boundary_layer_height_m,
        precipitation_flux_kg_m2_s,
        rain_fraction,
        snow_fraction,
        hail_fraction,
        surface_wetness,
        snow_water_equivalent_kg_m2,
        cape_j_kg,
        cin_j_kg,
        lightning_rate_hz,
        mie_anisotropy,
        mie_scale_height_m,
        rayleigh_scale_height_m,
        temperature_lapse_rate_k_m,
        sun_angular_radius_rad,
        sun_irradiance_w_m2,
        sun_direct_illuminance_lux,
        moon_angular_radius_rad,
        moon_illuminated_fraction,
        moon_illuminance_lux,
        local_sidereal_angle_rad,
        geomagnetic_kp,
        aurora_intensity,
        star_radiance_scale,
        *wind_m_s,
        *gust_delta_m_s,
        *ground_albedo,
        *sun_direction_enu,
        *moon_direction_enu,
        *celestial_to_enu_xyzw,
        *rayleigh_scattering_per_m,
        *mie_scattering_per_m,
        *mie_absorption_per_m,
        *ozone_absorption_per_m,
        *sun_color_linear,
        *moon_color_linear,
    )
    _require_finite("SKS1 environment", all_scalars)

    if evolution_mode not in EVOLUTION_NAMES:
        raise ValueError("SKS1 evolution mode is outside 1..4")
    if last_control_result not in CONTROL_RESULT_NAMES:
        raise ValueError("SKS1 control result is outside 0..2")
    if flags & ~sum(SKY_FLAG_NAMES):
        raise ValueError("SKS1 state flags contain reserved bits")
    if cloud_layer_count > MAXIMUM_CLOUD_LAYERS:
        raise ValueError("SKS1 cloud layer count exceeds four")
    if fluid_time_seconds < 0.0 or predicted_valid_time_seconds + 1.0e-9 < fluid_time_seconds:
        raise ValueError("SKS1 fluid/predicted times are invalid")
    _require_range("latitude", latitude_degrees, -90.0, 90.0)
    _require_range("longitude", longitude_degrees, -180.0, 180.0)
    _require_range("elevation", elevation_m, -500.0, 100000.0)
    _require_range("time scale", time_scale, -86400.0, 86400.0)
    _require_range("UTC Unix seconds", utc_unix_seconds, -2208988800.0, 4133980800.0)
    _require_range("horizontal X extent", horizontal_extent_x_m, 100.0, 2_000_000.0)
    _require_range("horizontal Y extent", horizontal_extent_y_m, 100.0, 2_000_000.0)
    if abs(horizontal_extent_x_m - horizontal_extent_y_m) > 0.01:
        raise ValueError("SKS1 horizontal domain extents must match")
    _require_range("vertical extent", vertical_extent_m, 100.0, 100000.0)
    _require_range("temperature", temperature_kelvin, 150.0, 350.0)
    _require_range("pressure", pressure_pa, 10000.0, 120000.0)
    _require_range("relative humidity", relative_humidity, 0.0, 1.0)
    _require_range("visibility", visibility_m, 1.0, 200000.0)
    _require_range("aerosol optical depth", aerosol_optical_depth, 0.0, 3.0)
    _require_range("ozone", ozone_dobson_units, 100.0, 600.0)
    if _length(wind_m_s) > 150.0 or _length(gust_delta_m_s) > 200.0:
        raise ValueError("SKS1 mean wind or gust delta exceeds its protocol limit")
    for name, direction in (("sun", sun_direction_enu), ("moon", moon_direction_enu)):
        length = _length(direction)
        if not 0.99 <= length <= 1.01:
            raise ValueError(f"SKS1 {name} direction is not normalized")
    if not 0.99 <= _length(celestial_to_enu_xyzw) <= 1.01:
        raise ValueError("SKS1 celestial-to-ENU quaternion is not normalized")
    for name, fraction in (
        ("rain fraction", rain_fraction),
        ("snow fraction", snow_fraction),
        ("hail fraction", hail_fraction),
        ("surface wetness", surface_wetness),
        ("moon illuminated fraction", moon_illuminated_fraction),
        ("aurora intensity", aurora_intensity),
    ):
        _require_range(name, fraction, 0.0, 1.0)
    phase_sum = rain_fraction + snow_fraction + hail_fraction
    if (
        precipitation_flux_kg_m2_s > 0.0
        and abs(phase_sum - 1.0) > 0.0001
    ) or (
        precipitation_flux_kg_m2_s == 0.0 and phase_sum > 0.0001
    ):
        raise ValueError("SKS1 precipitation phase fractions are inconsistent")
    if min(precipitation_flux_kg_m2_s, snow_water_equivalent_kg_m2, cape_j_kg, cin_j_kg, lightning_rate_hz) < 0.0:
        raise ValueError("SKS1 precipitation, snow, CAPE/CIN, or lightning value is negative")
    if (
        precipitation_flux_kg_m2_s > _as_f32(300.0 / 3600.0)
        or cape_j_kg > 3000.0
        or cin_j_kg > 3000.0
        or lightning_rate_hz > 10.0
    ):
        raise ValueError("SKS1 precipitation, CAPE/CIN, or lightning value exceeds its protocol limit")
    if supported_cloud_field_mask & ~SUPPORTED_CLOUD_FIELD_MASK:
        raise ValueError("SKS1 supported cloud field mask contains reserved bits")
    if active_cloud_field_mask & ~supported_cloud_field_mask:
        raise ValueError("SKS1 active cloud fields are not a supported subset")

    cloud_layers = [
        _parse_cloud_layer(packet, index, elevation_m)
        for index in range(cloud_layer_count)
    ]
    for index in range(cloud_layer_count, MAXIMUM_CLOUD_LAYERS):
        offset = 224 + index * 32
        if packet[offset:offset + 32] != bytes(32):
            raise ValueError(f"inactive cloud layer {index} must be zero-filled")

    wind_speed_m_s = _length(wind_m_s)
    gust_speed_m_s = wind_speed_m_s + _length(gust_delta_m_s)
    sun_elevation_degrees = math.degrees(
        math.asin(max(-1.0, min(1.0, sun_direction_enu[2])))
    )
    moon_elevation_degrees = math.degrees(
        math.asin(max(-1.0, min(1.0, moon_direction_enu[2])))
    )

    return {
        "protocol": {
            "magic": "SKS1",
            "version": VERSION,
            "packet_bytes": PACKET_BYTES,
            "crc32": encoded_crc,
        },
        "sequence": sequence,
        "flags": flags,
        "flag_names": _flag_names(flags, SKY_FLAG_NAMES),
        "acknowledgement": {
            "last_control_session": last_control_session,
            "last_control_sequence": last_control_sequence,
            "last_control_result": last_control_result,
            "last_control_result_name": CONTROL_RESULT_NAMES.get(
                last_control_result, f"unknown:{last_control_result}"
            ),
            "volume_frame_id": volume_frame_id,
        },
        "model": {
            "weather_seed": weather_seed,
            "weather_model_revision": weather_model_revision,
            "evolution_mode": evolution_mode,
            "evolution_mode_name": EVOLUTION_NAMES[evolution_mode],
            "lightning_event_id": lightning_event_id,
            "supported_cloud_field_mask": supported_cloud_field_mask,
            "active_cloud_field_mask": active_cloud_field_mask,
        },
        "time": {
            "fluid_time_seconds": fluid_time_seconds,
            "predicted_valid_time_seconds": predicted_valid_time_seconds,
            "utc_unix_seconds": utc_unix_seconds,
            "utc_iso8601": _utc_iso8601(utc_unix_seconds),
            "time_scale": time_scale,
        },
        "location": {
            "latitude_degrees": latitude_degrees,
            "longitude_degrees": longitude_degrees,
            "elevation_m": elevation_m,
        },
        "domain": {
            "horizontal_extent_m": horizontal_extent_x_m,
            "vertical_extent_m": vertical_extent_m,
        },
        "weather": {
            "surface_temperature_kelvin": temperature_kelvin,
            "surface_temperature_celsius": temperature_kelvin - 273.15,
            "sea_level_pressure_pa": pressure_pa,
            "relative_humidity": relative_humidity,
            "visibility_m": visibility_m,
            "aerosol_optical_depth_550nm": aerosol_optical_depth,
            "ozone_dobson_units": ozone_dobson_units,
            "wind_enu_m_s": list(wind_m_s),
            "wind_speed_m_s": wind_speed_m_s,
            "gust_delta_enu_m_s": list(gust_delta_m_s),
            "gust_speed_m_s": gust_speed_m_s,
            "boundary_layer_height_m": boundary_layer_height_m,
            "precipitation_flux_kg_m2_s": precipitation_flux_kg_m2_s,
            "precipitation_rate_mm_h": precipitation_flux_kg_m2_s * 3600.0,
            "rain_fraction": rain_fraction,
            "snow_fraction": snow_fraction,
            "hail_fraction": hail_fraction,
            "surface_wetness": surface_wetness,
            "snow_water_equivalent_kg_m2": snow_water_equivalent_kg_m2,
            "cape_j_kg": cape_j_kg,
            "cin_j_kg": cin_j_kg,
            "lightning_rate_hz": lightning_rate_hz,
        },
        "cloud_layers": cloud_layers,
        "celestial": {
            "sun_direction_enu": list(sun_direction_enu),
            "sun_elevation_degrees": sun_elevation_degrees,
            "sun_angular_radius_radians": sun_angular_radius_rad,
            "sun_irradiance_w_m2": sun_irradiance_w_m2,
            "sun_direct_illuminance_lux": sun_direct_illuminance_lux,
            "sun_color_linear": list(sun_color_linear),
            "moon_direction_enu": list(moon_direction_enu),
            "moon_elevation_degrees": moon_elevation_degrees,
            "moon_angular_radius_radians": moon_angular_radius_rad,
            "moon_illuminated_fraction": moon_illuminated_fraction,
            "moon_illuminance_lux": moon_illuminance_lux,
            "moon_color_linear": list(moon_color_linear),
            "celestial_to_enu_xyzw": list(celestial_to_enu_xyzw),
            "local_sidereal_angle_radians": local_sidereal_angle_rad,
            "geomagnetic_kp": geomagnetic_kp,
            "aurora_intensity": aurora_intensity,
            "star_radiance_scale": star_radiance_scale,
        },
        "atmosphere": {
            "ground_albedo": list(ground_albedo),
            "mie_anisotropy": mie_anisotropy,
            "mie_scale_height_m": mie_scale_height_m,
            "rayleigh_scale_height_m": rayleigh_scale_height_m,
            "temperature_lapse_rate_k_m": temperature_lapse_rate_k_m,
            "rayleigh_scattering_per_m": list(rayleigh_scattering_per_m),
            "mie_scattering_per_m": list(mie_scattering_per_m),
            "mie_absorption_per_m": list(mie_absorption_per_m),
            "ozone_absorption_per_m": list(ozone_absorption_per_m),
        },
    }


def format_summary(state: dict[str, Any], source: tuple[str, int] | None = None) -> str:
    location = state["location"]
    weather = state["weather"]
    celestial = state["celestial"]
    timing = state["time"]
    ack = state["acknowledgement"]
    model = state["model"]
    origin = f" from {source[0]}:{source[1]}" if source else ""
    lines = [
        f"SKS1 state{origin}: sequence={state['sequence']} volume_frame={ack['volume_frame_id']}",
        (
            f"  UTC={timing['utc_iso8601'] or timing['utc_unix_seconds']} "
            f"scale={timing['time_scale']:.3g}x location="
            f"{location['latitude_degrees']:.5f},"
            f"{location['longitude_degrees']:.5f} @ {location['elevation_m']:.1f}m"
        ),
        (
            f"  weather={model['evolution_mode_name']} "
            f"{weather['surface_temperature_celsius']:.1f}C "
            f"RH={weather['relative_humidity'] * 100.0:.1f}% "
            f"visibility={weather['visibility_m'] / 1000.0:.1f}km "
            f"wind={weather['wind_speed_m_s']:.1f}m/s "
            f"gust={weather['gust_speed_m_s']:.1f}m/s "
            f"precip={weather['precipitation_rate_mm_h']:.2f}mm/h"
        ),
        (
            f"  sun={celestial['sun_elevation_degrees']:.2f}deg "
            f"moon={celestial['moon_elevation_degrees']:.2f}deg "
            f"moon_phase={celestial['moon_illuminated_fraction']:.3f} "
            f"cloud_layers={len(state['cloud_layers'])}"
        ),
        (
            f"  control_ack=session 0x{ack['last_control_session']:08x} "
            f"sequence {ack['last_control_sequence']} "
            f"result={ack['last_control_result_name']}"
        ),
    ]
    for layer in state["cloud_layers"]:
        lines.append(
            f"    layer[{layer['index']}] {layer['kind']} "
            f"{layer['base_agl_m']:.0f}-{layer['top_agl_m']:.0f}m AGL "
            f"coverage={layer['coverage']:.2f} "
            f"precip={layer['precipitation_rate_mm_h']:.2f}mm/h"
        )
    return "\n".join(lines)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="local IPv4 bind address")
    parser.add_argument("--port", type=int, default=7779, help="local SKS1 UDP port")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--json", action="store_true", help="emit the validated state as JSON"
    )
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in [1, 65535]")
    if not math.isfinite(args.timeout) or args.timeout <= 0.0:
        parser.error("--timeout must be a positive finite number")
    return args


def run(args: argparse.Namespace) -> int:
    deadline = time.monotonic() + args.timeout
    rejected = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.host, args.port))
        sock.settimeout(min(0.5, args.timeout))
        if not args.json:
            print(f"Waiting for one SKS1 packet on {args.host}:{args.port} ...")
        while time.monotonic() < deadline:
            try:
                packet, source = sock.recvfrom(65535)
            except socket.timeout:
                continue
            try:
                state = parse_packet(packet)
            except (ValueError, struct.error):
                rejected += 1
                continue
            if args.json:
                print(json.dumps(state, indent=2, sort_keys=True, allow_nan=False))
            else:
                print(format_summary(state, source))
            return 0

    print(
        f"Timed out before receiving a valid SKS1 packet; rejected={rejected}",
        file=sys.stderr,
    )
    return 1


def main(argv: Sequence[str] | None = None) -> int:
    try:
        return run(parse_args(argv))
    except OSError as error:
        print(f"receive_sky: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
