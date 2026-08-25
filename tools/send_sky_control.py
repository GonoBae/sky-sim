#!/usr/bin/env python3
"""Send one canonical SKC1 sky/weather control command over UDP.

Examples:
  send_sky_control.py --transition 8 preset storm
  send_sky_control.py time 2026-08-25T09:00:00Z
  send_sky_control.py location 37.5665 126.9780 38
  send_sky_control.py time-scale 60
  send_sky_control.py wind 8 2 0 --gust 13
  send_sky_control.py weather --temperature-c 24 --pressure-hpa 1004 --humidity 0.82

Only the Python standard library is used.  Serialization is explicit,
fixed-size, little-endian, zero-filled, and CRC-protected so this file can
also serve as an interoperability reference.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import math
import secrets
import socket
import struct
import sys
import time
import zlib
from typing import Iterable, Sequence


MAGIC = b"SKC1"
VERSION = 1
PACKET_BYTES = 512
CRC_OFFSET = 508
UINT16_MAX = (1 << 16) - 1
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1

OPCODE_PATCH_OVERRIDE = 1
OPCODE_RELEASE_OVERRIDE = 2
OPCODE_LOAD_PRESET = 3
OPCODE_REQUEST_KEYFRAME = 4

EVOLUTION_NATURAL = 1
EVOLUTION_TIMELINE = 2
EVOLUTION_MANUAL = 3
EVOLUTION_REPLAY = 4

PRESET_NATURAL = 0
PRESET_CLEAR = 1
PRESET_CUMULUS = 2
PRESET_OVERCAST = 3
PRESET_RAIN = 4
PRESET_STORM = 5
PRESET_SNOW = 6
PRESET_FOG = 7
PRESET_CUSTOM = 8
PRESET_NAMES = {
    "natural": PRESET_NATURAL,
    "clear": PRESET_CLEAR,
    "cumulus": PRESET_CUMULUS,
    "overcast": PRESET_OVERCAST,
    "rain": PRESET_RAIN,
    "storm": PRESET_STORM,
    "snow": PRESET_SNOW,
    "fog": PRESET_FOG,
}

APPLY_UTC = 1 << 0
APPLY_LOCATION = 1 << 1
APPLY_DOMAIN = 1 << 2
APPLY_TIME_SCALE = 1 << 3
APPLY_EVOLUTION = 1 << 4
APPLY_THERMODYNAMICS = 1 << 5
APPLY_VISIBILITY = 1 << 6
APPLY_WIND = 1 << 7
APPLY_PRECIPITATION = 1 << 8
APPLY_CONVECTION = 1 << 9
APPLY_CLOUD_LAYER_0 = 1 << 11
APPLY_CLOUD_LAYER_1 = 1 << 12
APPLY_CLOUD_LAYER_2 = 1 << 13
APPLY_CLOUD_LAYER_3 = 1 << 14
APPLY_ATMOSPHERE_OPTICS = 1 << 19
SUPPORTED_APPLY_MASK = (
    APPLY_UTC
    | APPLY_LOCATION
    | APPLY_DOMAIN
    | APPLY_TIME_SCALE
    | APPLY_EVOLUTION
    | APPLY_THERMODYNAMICS
    | APPLY_VISIBILITY
    | APPLY_WIND
    | APPLY_PRECIPITATION
    | APPLY_CONVECTION
    | APPLY_CLOUD_LAYER_0
    | APPLY_CLOUD_LAYER_1
    | APPLY_CLOUD_LAYER_2
    | APPLY_CLOUD_LAYER_3
)
WEATHER_APPLY_MASK = (
    APPLY_THERMODYNAMICS
    | APPLY_VISIBILITY
    | APPLY_WIND
    | APPLY_PRECIPITATION
    | APPLY_CONVECTION
    | APPLY_CLOUD_LAYER_0
    | APPLY_CLOUD_LAYER_1
    | APPLY_CLOUD_LAYER_2
    | APPLY_CLOUD_LAYER_3
)
SUPPORTED_CLOUD_FIELD_MASK = 0x001F

CLOUD_KIND_STRATIFORM = 1
CLOUD_KIND_CONVECTIVE = 2
CLOUD_KIND_DEEP_CONVECTIVE = 3
CLOUD_KIND_CIRRUS = 4
CLOUD_KIND_OTHER = 5


@dataclasses.dataclass(frozen=True)
class CloudLayer:
    kind: str = "stratiform"
    base_altitude_amsl_m: float = 1000.0
    top_altitude_amsl_m: float = 2500.0
    coverage: float = 0.5
    optical_depth: float = 20.0
    liquid_fraction: float = 1.0
    precipitation_rate_mm_h: float = 0.0
    convective_activity: float = 0.0


@dataclasses.dataclass(frozen=True)
class AtmosphereOptics:
    ground_albedo: tuple[float, float, float] = (0.18, 0.18, 0.18)
    mie_anisotropy: float = 0.8
    mie_scale_height_m: float = 1200.0
    rayleigh_scale_height_m: float = 8000.0
    rayleigh_scattering_per_m: tuple[float, float, float] = (
        5.802e-6,
        13.558e-6,
        33.100e-6,
    )
    mie_scattering_per_m: tuple[float, float, float] = (
        3.996e-6,
        3.996e-6,
        3.996e-6,
    )
    mie_absorption_per_m: tuple[float, float, float] = (
        4.4e-7,
        4.4e-7,
        4.4e-7,
    )
    ozone_absorption_per_m: tuple[float, float, float] = (
        0.650e-6,
        1.881e-6,
        0.085e-6,
    )


@dataclasses.dataclass(frozen=True)
class SkyControlCommand:
    session_id: int
    sequence: int
    client_time_seconds: float
    opcode: int
    evolution_mode: int = EVOLUTION_MANUAL
    transition_milliseconds: int = 5000
    hold_milliseconds: int = 0
    preset: int = PRESET_CUSTOM
    apply_mask: int = 0
    clear_mask: int = 0
    weather_seed: int = 1
    requested_cloud_field_mask: int = 0
    requested_state_hz: int = 0

    utc_unix_seconds: float = 0.0
    latitude_degrees: float = 0.0
    longitude_degrees: float = 0.0
    elevation_m: float = 0.0
    time_scale: float = 0.0
    horizontal_extent_m: float = 0.0
    vertical_extent_m: float = 0.0

    surface_temperature_kelvin: float = 0.0
    sea_level_pressure_pa: float = 0.0
    relative_humidity: float = 0.0
    visibility_m: float = 0.0
    aerosol_optical_depth_550nm: float = 0.0
    ozone_dobson_units: float = 0.0
    wind_enu_m_s: tuple[float, float, float] = (0.0, 0.0, 0.0)
    gust_speed_m_s: float = 0.0
    precipitation_rate_mm_h: float = 0.0
    snow_fraction: float = 0.0
    surface_wetness: float = 0.0
    convective_activity: float = 0.0
    lightning_activity: float = 0.0
    cloud_layers: tuple[CloudLayer | None, ...] = (None, None, None, None)
    atmosphere_optics: AtmosphereOptics = dataclasses.field(
        default_factory=AtmosphereOptics
    )


def _as_f32(value: float) -> float:
    """Round once to IEEE-754 binary32, matching C++ float storage."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _f32_add(left: float, right: float) -> float:
    return _as_f32(_as_f32(left) + _as_f32(right))


def _f32_mul(left: float, right: float) -> float:
    return _as_f32(_as_f32(left) * _as_f32(right))


def _f32_length(vector: Sequence[float]) -> float:
    x, y, z = (_as_f32(value) for value in vector)
    return _as_f32(math.sqrt(x * x + y * y + z * z))


def _require_unsigned(
    name: str, value: int, maximum: int, *, allow_zero: bool = True
) -> None:
    minimum = 0 if allow_zero else 1
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")


def _require_finite(name: str, values: Iterable[float]) -> tuple[float, ...]:
    result = tuple(values)
    if not all(math.isfinite(value) for value in result):
        raise ValueError(f"{name} must contain only finite values")
    return result


def _require_range(name: str, value: float, minimum: float, maximum: float) -> None:
    if not math.isfinite(value) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be finite and in [{minimum}, {maximum}]")


def _require_f32_range(
    name: str, value: float, minimum: float, maximum: float
) -> float:
    if not math.isfinite(value):
        raise ValueError(f"{name} must be finite and in [{minimum}, {maximum}]")
    try:
        rounded = _as_f32(value)
    except (OverflowError, struct.error) as error:
        raise ValueError(
            f"{name} must be finite and in [{minimum}, {maximum}]"
        ) from error
    if not _as_f32(minimum) <= rounded <= _as_f32(maximum):
        raise ValueError(f"{name} must be finite and in [{minimum}, {maximum}]")
    return rounded


def _layer_wire_kind(layer: CloudLayer) -> int:
    kind = layer.kind.strip().lower()
    if kind in ("stratiform", "fog"):
        return CLOUD_KIND_STRATIFORM
    if kind == "convective":
        return (
            CLOUD_KIND_DEEP_CONVECTIVE
            if _as_f32(layer.top_altitude_amsl_m) >= 8000.0
            else CLOUD_KIND_CONVECTIVE
        )
    if kind == "cirrus":
        return CLOUD_KIND_CIRRUS
    raise ValueError("cloud layer kind must be stratiform, fog, convective, or cirrus")


def _validate_layer(layer: CloudLayer, index: int) -> None:
    values = _require_finite(
        f"cloud layer {index}",
        (
            layer.base_altitude_amsl_m,
            layer.top_altitude_amsl_m,
            layer.coverage,
            layer.optical_depth,
            layer.liquid_fraction,
            layer.precipitation_rate_mm_h,
            layer.convective_activity,
        ),
    )
    base, top, coverage, optical_depth, liquid, precipitation, convection = (
        _as_f32(value) for value in values
    )
    thickness = _as_f32(top - base)
    if base < _as_f32(-500.0) or top <= base or top > _as_f32(100000.0):
        raise ValueError(f"cloud layer {index} altitude range is invalid")
    _require_f32_range(f"cloud layer {index} coverage", coverage, 0.0, 1.0)
    _require_f32_range(
        f"cloud layer {index} optical depth", optical_depth, 0.0, 500.0
    )
    if optical_depth > thickness:
        raise ValueError(
            f"cloud layer {index} optical depth exceeds this layer's wire precision"
        )
    _require_f32_range(
        f"cloud layer {index} liquid fraction", liquid, 0.0, 1.0
    )
    _require_f32_range(
        f"cloud layer {index} precipitation", precipitation, 0.0, 300.0
    )
    _require_f32_range(
        f"cloud layer {index} convection", convection, 0.0, 1.0
    )
    _layer_wire_kind(layer)


def _validate_optics(optics: AtmosphereOptics) -> None:
    for name, values in (
        ("ground albedo", optics.ground_albedo),
        ("Rayleigh scattering", optics.rayleigh_scattering_per_m),
        ("Mie scattering", optics.mie_scattering_per_m),
        ("Mie absorption", optics.mie_absorption_per_m),
        ("ozone absorption", optics.ozone_absorption_per_m),
    ):
        if len(values) != 3:
            raise ValueError(f"{name} must contain three values")
        _require_finite(name, values)
    _require_range("Mie anisotropy", optics.mie_anisotropy, -0.99, 0.99)
    if (
        not math.isfinite(optics.mie_scale_height_m)
        or not math.isfinite(optics.rayleigh_scale_height_m)
        or optics.mie_scale_height_m <= 0.0
        or optics.rayleigh_scale_height_m <= 0.0
    ):
        raise ValueError("atmosphere scale heights must be positive and finite")


def validate_command(command: SkyControlCommand) -> None:
    _require_unsigned("session_id", command.session_id, UINT32_MAX, allow_zero=False)
    _require_unsigned("sequence", command.sequence, UINT32_MAX)
    _require_unsigned("transition_milliseconds", command.transition_milliseconds, UINT32_MAX)
    _require_unsigned("hold_milliseconds", command.hold_milliseconds, UINT32_MAX)
    _require_unsigned("preset", command.preset, PRESET_CUSTOM)
    _require_unsigned("apply_mask", command.apply_mask, UINT64_MAX)
    _require_unsigned("clear_mask", command.clear_mask, UINT64_MAX)
    _require_unsigned("weather_seed", command.weather_seed, UINT32_MAX)
    _require_unsigned(
        "requested_cloud_field_mask", command.requested_cloud_field_mask, UINT16_MAX
    )
    _require_unsigned("requested_state_hz", command.requested_state_hz, UINT16_MAX)
    if not math.isfinite(command.client_time_seconds) or command.client_time_seconds < 0.0:
        raise ValueError("client_time_seconds must be nonnegative and finite")
    if command.opcode not in (
        OPCODE_PATCH_OVERRIDE,
        OPCODE_RELEASE_OVERRIDE,
        OPCODE_LOAD_PRESET,
        OPCODE_REQUEST_KEYFRAME,
    ):
        raise ValueError("unsupported SKC1 opcode")
    if command.evolution_mode not in (
        EVOLUTION_NATURAL,
        EVOLUTION_TIMELINE,
        EVOLUTION_MANUAL,
        EVOLUTION_REPLAY,
    ):
        raise ValueError("evolution_mode must be in [1, 4]")
    if command.transition_milliseconds > 86_400_000:
        raise ValueError("transition_milliseconds must not exceed one day")
    if command.hold_milliseconds != 0:
        raise ValueError("hold_milliseconds is reserved and must be zero")
    if command.requested_state_hz != 0 or command.requested_cloud_field_mask != 0:
        raise ValueError("requested cloud fields/state rate are reserved and must be zero")
    if command.apply_mask & ~SUPPORTED_APPLY_MASK or command.clear_mask & ~SUPPORTED_APPLY_MASK:
        raise ValueError("apply/clear mask contains unsupported bits")

    if command.opcode == OPCODE_LOAD_PRESET:
        if command.apply_mask or command.clear_mask or command.preset not in PRESET_NAMES.values():
            raise ValueError("load-preset requires one built-in preset and zero masks")
        return
    if command.opcode == OPCODE_REQUEST_KEYFRAME:
        if (
            command.apply_mask
            or command.clear_mask
            or command.transition_milliseconds != 0
            or command.preset != PRESET_NATURAL
        ):
            raise ValueError("keyframe request requires canonical zero values")
        return
    if command.opcode == OPCODE_RELEASE_OVERRIDE:
        if command.apply_mask or command.clear_mask != WEATHER_APPLY_MASK:
            raise ValueError("release requires the complete weather clear mask")
        return
    if not command.apply_mask or command.clear_mask:
        raise ValueError("patch requires a nonzero apply mask and zero clear mask")
    if command.apply_mask & APPLY_EVOLUTION:
        if command.evolution_mode not in (EVOLUTION_NATURAL, EVOLUTION_MANUAL):
            raise ValueError("evolution patch supports only natural or manual")
        if (
            command.evolution_mode == EVOLUTION_NATURAL
            and command.apply_mask & WEATHER_APPLY_MASK
        ):
            raise ValueError(
                "natural evolution cannot be combined with manual weather groups"
            )

    if command.apply_mask & APPLY_UTC:
        _require_range("UTC Unix seconds", command.utc_unix_seconds, -2208988800.0, 4133980800.0)
    if command.apply_mask & APPLY_LOCATION:
        _require_range("latitude", command.latitude_degrees, -90.0, 90.0)
        _require_range("longitude", command.longitude_degrees, -180.0, 180.0)
        _require_f32_range("elevation", command.elevation_m, -500.0, 100000.0)
    if command.apply_mask & APPLY_DOMAIN:
        _require_f32_range("horizontal extent", command.horizontal_extent_m, 100.0, 2_000_000.0)
        _require_f32_range("vertical extent", command.vertical_extent_m, 100.0, 100000.0)
    if command.apply_mask & APPLY_TIME_SCALE:
        _require_f32_range("time scale", command.time_scale, -86400.0, 86400.0)
    if command.apply_mask & APPLY_THERMODYNAMICS:
        _require_f32_range("temperature", command.surface_temperature_kelvin, 203.15, 333.15)
        _require_f32_range("pressure", command.sea_level_pressure_pa, 80000.0, 108000.0)
        _require_f32_range("relative humidity", command.relative_humidity, 0.01, 1.0)
    if command.apply_mask & APPLY_VISIBILITY:
        _require_f32_range("visibility", command.visibility_m, 25.0, 200000.0)
        _require_f32_range("aerosol optical depth", command.aerosol_optical_depth_550nm, 0.005, 3.0)
        _require_f32_range("ozone", command.ozone_dobson_units, 100.0, 600.0)
    if command.apply_mask & APPLY_WIND:
        if len(command.wind_enu_m_s) != 3:
            raise ValueError("wind_enu_m_s must contain east, north, and up")
        _require_finite("wind", command.wind_enu_m_s)
        mean_speed = _f32_length(command.wind_enu_m_s)
        if mean_speed > 150.0:
            raise ValueError("wind magnitude must not exceed 150 m/s")
        gust_speed = _require_f32_range(
            "gust speed", command.gust_speed_m_s, 0.0, 200.0
        )
        if gust_speed < mean_speed:
            raise ValueError("gust speed must be between mean wind and 200 m/s")
    if command.apply_mask & APPLY_PRECIPITATION:
        _require_f32_range("precipitation", command.precipitation_rate_mm_h, 0.0, 300.0)
        _require_f32_range("snow fraction", command.snow_fraction, 0.0, 1.0)
        _require_f32_range("surface wetness", command.surface_wetness, 0.0, 1.0)
    if command.apply_mask & APPLY_CONVECTION:
        _require_f32_range("convective activity", command.convective_activity, 0.0, 1.0)
        _require_f32_range("lightning activity", command.lightning_activity, 0.0, 1.0)
    if len(command.cloud_layers) != 4:
        raise ValueError("cloud_layers must contain exactly four entries")
    for index in range(4):
        if command.apply_mask & (APPLY_CLOUD_LAYER_0 << index):
            layer = command.cloud_layers[index]
            if layer is None:
                raise ValueError(f"cloud layer {index} apply bit requires layer values")
            _validate_layer(layer, index)
    if command.apply_mask & APPLY_ATMOSPHERE_OPTICS:
        _validate_optics(command.atmosphere_optics)


def _write_vector(packet: bytearray, offset: int, values: Sequence[float]) -> None:
    struct.pack_into("<3f", packet, offset, *values)


def _write_layer(packet: bytearray, index: int, layer: CloudLayer) -> None:
    offset = 224 + index * 32
    base = _as_f32(layer.base_altitude_amsl_m)
    top = _as_f32(layer.top_altitude_amsl_m)
    thickness = max(1.0, _as_f32(top - base))
    optical_depth = _as_f32(layer.optical_depth)
    denominator = _f32_mul(100.0, thickness)
    condensate = _as_f32(max(0.0, min(0.01, _as_f32(optical_depth / denominator))))
    ice_fraction = _as_f32(1.0 - _as_f32(layer.liquid_fraction))
    precipitation_flux = _as_f32(_as_f32(layer.precipitation_rate_mm_h) / 3600.0)
    canonical_precipitation = _as_f32(layer.precipitation_rate_mm_h)
    canonical_convection = _as_f32(layer.convective_activity)
    turbulence = _as_f32(0.25 + _f32_mul(4.0, canonical_convection))
    struct.pack_into(
        "<7fBB",
        packet,
        offset,
        base,
        top,
        layer.coverage,
        condensate,
        ice_fraction,
        precipitation_flux,
        turbulence,
        _layer_wire_kind(layer),
        (1 if canonical_convection > _as_f32(0.35) else 0)
        | (2 if canonical_precipitation > _as_f32(0.001) else 0),
    )


def pack_command(command: SkyControlCommand) -> bytes:
    """Validate and serialize one canonical 512-byte SKC1 datagram."""
    validate_command(command)
    packet = bytearray(PACKET_BYTES)
    struct.pack_into("<4sHHIIdBB", packet, 0, MAGIC, VERSION, PACKET_BYTES, command.session_id, command.sequence, command.client_time_seconds, command.opcode, command.evolution_mode)
    struct.pack_into("<IIIQQIHH", packet, 28, command.transition_milliseconds, command.hold_milliseconds, command.preset, command.apply_mask, command.clear_mask, command.weather_seed, command.requested_cloud_field_mask, command.requested_state_hz)

    if command.apply_mask & APPLY_UTC:
        struct.pack_into("<d", packet, 64, command.utc_unix_seconds)
    if command.apply_mask & APPLY_LOCATION:
        struct.pack_into("<ddf", packet, 72, command.latitude_degrees, command.longitude_degrees, command.elevation_m)
    if command.apply_mask & APPLY_TIME_SCALE:
        struct.pack_into("<f", packet, 92, command.time_scale)
    if command.apply_mask & APPLY_DOMAIN:
        struct.pack_into("<3f", packet, 96, command.horizontal_extent_m, command.horizontal_extent_m, command.vertical_extent_m)
    if command.apply_mask & APPLY_THERMODYNAMICS:
        struct.pack_into("<3f", packet, 108, command.surface_temperature_kelvin, command.sea_level_pressure_pa, command.relative_humidity)
    if command.apply_mask & APPLY_VISIBILITY:
        struct.pack_into("<3f", packet, 120, command.visibility_m, command.aerosol_optical_depth_550nm, command.ozone_dobson_units)
    if command.apply_mask & APPLY_WIND:
        wind = tuple(_as_f32(value) for value in command.wind_enu_m_s)
        _write_vector(packet, 132, wind)
        mean_speed = _f32_length(wind)
        delta = max(0.0, _as_f32(_as_f32(command.gust_speed_m_s) - mean_speed))
        if mean_speed > 0.0001:
            inverse = _as_f32(1.0 / mean_speed)
            gust = tuple(_f32_mul(_f32_mul(value, inverse), delta) for value in wind)
        else:
            gust = (delta, 0.0, 0.0)
        _write_vector(packet, 144, gust)
    if command.apply_mask & APPLY_PRECIPITATION:
        precipitation_flux = _as_f32(_as_f32(command.precipitation_rate_mm_h) / 3600.0)
        snow_fraction = _as_f32(command.snow_fraction)
        struct.pack_into("<3f", packet, 160, precipitation_flux, _as_f32(1.0 - snow_fraction), snow_fraction)
        struct.pack_into("<f", packet, 176, command.surface_wetness)
    if command.apply_mask & APPLY_CONVECTION:
        struct.pack_into("<f", packet, 184, _f32_mul(command.convective_activity, 3000.0))
        struct.pack_into("<f", packet, 192, _f32_mul(command.lightning_activity, 0.25))
    for index, layer in enumerate(command.cloud_layers):
        if command.apply_mask & (APPLY_CLOUD_LAYER_0 << index):
            assert layer is not None
            _write_layer(packet, index, layer)
    if command.apply_mask & APPLY_ATMOSPHERE_OPTICS:
        optics = command.atmosphere_optics
        _write_vector(packet, 196, optics.ground_albedo)
        struct.pack_into("<3f", packet, 208, optics.mie_anisotropy, optics.mie_scale_height_m, optics.rayleigh_scale_height_m)
        _write_vector(packet, 432, optics.rayleigh_scattering_per_m)
        _write_vector(packet, 444, optics.mie_scattering_per_m)
        _write_vector(packet, 456, optics.mie_absorption_per_m)
        _write_vector(packet, 468, optics.ozone_absorption_per_m)

    struct.pack_into("<I", packet, CRC_OFFSET, zlib.crc32(packet[:CRC_OFFSET]) & UINT32_MAX)
    return bytes(packet)


def _read_layer(packet: bytes, index: int) -> CloudLayer:
    offset = 224 + index * 32
    base, top, coverage, condensate, ice, precipitation_flux, turbulence = struct.unpack_from("<7f", packet, offset)
    wire_kind = packet[offset + 28]
    if wire_kind == CLOUD_KIND_STRATIFORM:
        kind = "fog" if base <= 5.0 else "stratiform"
    elif wire_kind in (CLOUD_KIND_CONVECTIVE, CLOUD_KIND_DEEP_CONVECTIVE):
        kind = "convective"
    elif wire_kind == CLOUD_KIND_CIRRUS:
        kind = "cirrus"
    else:
        kind = "stratiform"
    thickness = max(1.0, _as_f32(top - base))
    optical_depth = _f32_mul(_f32_mul(condensate, 100.0), thickness)
    maximum_optical_depth = min(500.0, _as_f32(top - base))
    optical_depth_tolerance = _as_f32(0.001)
    if (
        math.isfinite(optical_depth)
        and optical_depth > maximum_optical_depth
        and optical_depth
        <= _as_f32(maximum_optical_depth + optical_depth_tolerance)
    ):
        optical_depth = maximum_optical_depth
    precipitation_rate = _f32_mul(precipitation_flux, 3600.0)
    convective_activity = _as_f32(max(0.0, min(1.0, _as_f32(_as_f32(turbulence - 0.25) / 4.0))))
    return CloudLayer(
        kind=kind,
        base_altitude_amsl_m=base,
        top_altitude_amsl_m=top,
        coverage=coverage,
        optical_depth=optical_depth,
        liquid_fraction=_as_f32(1.0 - ice),
        precipitation_rate_mm_h=precipitation_rate,
        convective_activity=convective_activity,
    )


def _unused_control_bytes_are_zero(packet: bytes, apply_mask: int) -> bool:
    used = bytearray(PACKET_BYTES)

    def mark(offset: int, count: int) -> None:
        used[offset : offset + count] = bytes([1]) * count

    mark(0, 64)
    mark(CRC_OFFSET, 4)
    fields = (
        (APPLY_UTC, 64, 8),
        (APPLY_LOCATION, 72, 20),
        (APPLY_TIME_SCALE, 92, 4),
        (APPLY_DOMAIN, 96, 12),
        (APPLY_THERMODYNAMICS, 108, 12),
        (APPLY_VISIBILITY, 120, 12),
        (APPLY_WIND, 132, 24),
        (APPLY_PRECIPITATION, 160, 12),
        (APPLY_PRECIPITATION, 176, 4),
        (APPLY_CONVECTION, 184, 4),
        (APPLY_CONVECTION, 192, 4),
        (APPLY_ATMOSPHERE_OPTICS, 196, 24),
        (APPLY_ATMOSPHERE_OPTICS, 432, 48),
    )
    for bit, offset, count in fields:
        if apply_mask & bit:
            mark(offset, count)
    for index in range(4):
        if apply_mask & (APPLY_CLOUD_LAYER_0 << index):
            mark(224 + index * 32, 30)
    return all(used[index] or packet[index] == 0 for index in range(64, CRC_OFFSET))


def unpack_command(packet: bytes) -> SkyControlCommand:
    """Strictly decode SKC1, including canonical zero-fill validation."""
    if len(packet) != PACKET_BYTES:
        raise ValueError(f"SKC1 packet must be exactly {PACKET_BYTES} bytes")
    magic, version, packet_bytes = struct.unpack_from("<4sHH", packet, 0)
    if magic != MAGIC or version != VERSION or packet_bytes != PACKET_BYTES:
        raise ValueError("invalid SKC1 magic, version, or packet size")
    if packet[26:28] != bytes(2) or packet[504:508] != bytes(4):
        raise ValueError("SKC1 reserved header/trailer bytes must be zero")
    if struct.unpack_from("<I", packet, CRC_OFFSET)[0] != zlib.crc32(packet[:CRC_OFFSET]) & UINT32_MAX:
        raise ValueError("SKC1 CRC32 mismatch")

    session_id, sequence = struct.unpack_from("<II", packet, 8)
    client_time_seconds = struct.unpack_from("<d", packet, 16)[0]
    opcode, evolution_mode = struct.unpack_from("<BB", packet, 24)
    transition_ms, hold_ms, preset, apply_mask, clear_mask, weather_seed, field_mask, state_hz = struct.unpack_from("<IIIQQIHH", packet, 28)
    if not _unused_control_bytes_are_zero(packet, apply_mask):
        raise ValueError("SKC1 packet has nonzero unused bytes")
    utc_unix_seconds = struct.unpack_from("<d", packet, 64)[0] if apply_mask & APPLY_UTC else 0.0
    if apply_mask & APPLY_LOCATION:
        latitude, longitude, elevation = struct.unpack_from("<ddf", packet, 72)
    else:
        latitude = longitude = elevation = 0.0
    time_scale = struct.unpack_from("<f", packet, 92)[0] if apply_mask & APPLY_TIME_SCALE else 0.0
    if apply_mask & APPLY_DOMAIN:
        horizontal_x, horizontal_y, vertical_extent = struct.unpack_from("<3f", packet, 96)
        if horizontal_x != horizontal_y:
            raise ValueError("SKC1 horizontal domain extents must match")
        horizontal_extent = horizontal_x
    else:
        horizontal_extent = vertical_extent = 0.0
    if apply_mask & APPLY_THERMODYNAMICS:
        temperature, pressure, humidity = struct.unpack_from("<3f", packet, 108)
    else:
        temperature = pressure = humidity = 0.0
    if apply_mask & APPLY_VISIBILITY:
        visibility, aerosol, ozone = struct.unpack_from("<3f", packet, 120)
    else:
        visibility = aerosol = ozone = 0.0
    if apply_mask & APPLY_WIND:
        wind = struct.unpack_from("<3f", packet, 132)
        gust_delta = struct.unpack_from("<3f", packet, 144)
        gust_speed = _as_f32(_f32_length(wind) + _f32_length(gust_delta))
        wire_gust_limit = _as_f32(200.0 + _as_f32(0.0001))
        if math.isfinite(gust_speed) and gust_speed <= wire_gust_limit:
            gust_speed = min(gust_speed, 200.0)
    else:
        wind = (0.0, 0.0, 0.0)
        gust_speed = 0.0
    if apply_mask & APPLY_PRECIPITATION:
        precipitation_flux = struct.unpack_from("<f", packet, 160)[0]
        snow_fraction = struct.unpack_from("<f", packet, 168)[0]
        surface_wetness = struct.unpack_from("<f", packet, 176)[0]
        precipitation = _f32_mul(precipitation_flux, 3600.0)
    else:
        precipitation = snow_fraction = surface_wetness = 0.0
    if apply_mask & APPLY_CONVECTION:
        cape = struct.unpack_from("<f", packet, 184)[0]
        lightning_rate = struct.unpack_from("<f", packet, 192)[0]
        convection = _as_f32(cape / 3000.0)
        lightning = _as_f32(lightning_rate / 0.25)
    else:
        convection = lightning = 0.0
    layers: list[CloudLayer | None] = [None, None, None, None]
    for index in range(4):
        if apply_mask & (APPLY_CLOUD_LAYER_0 << index):
            offset = 224 + index * 32
            raw_layer_values = struct.unpack_from("<7f", packet, offset)
            if not all(math.isfinite(value) for value in raw_layer_values):
                raise ValueError(f"SKC1 cloud layer {index} has non-finite values")
            if not 0.25 <= raw_layer_values[6] <= 4.25:
                raise ValueError(f"SKC1 cloud layer {index} turbulence must be in [0.25, 4.25]")
            if packet[offset + 28] == 0 or packet[offset + 28] > 5:
                raise ValueError(f"SKC1 cloud layer {index} has an invalid kind")
            if packet[offset + 29] & ~0x07:
                raise ValueError(f"SKC1 cloud layer {index} has reserved flags")
            layers[index] = _read_layer(packet, index)

    optics = AtmosphereOptics()
    if apply_mask & APPLY_ATMOSPHERE_OPTICS:
        anisotropy, mie_height, rayleigh_height = struct.unpack_from("<3f", packet, 208)
        optics = AtmosphereOptics(
            ground_albedo=struct.unpack_from("<3f", packet, 196),
            mie_anisotropy=anisotropy,
            mie_scale_height_m=mie_height,
            rayleigh_scale_height_m=rayleigh_height,
            rayleigh_scattering_per_m=struct.unpack_from("<3f", packet, 432),
            mie_scattering_per_m=struct.unpack_from("<3f", packet, 444),
            mie_absorption_per_m=struct.unpack_from("<3f", packet, 456),
            ozone_absorption_per_m=struct.unpack_from("<3f", packet, 468),
        )

    command = SkyControlCommand(
        session_id=session_id,
        sequence=sequence,
        client_time_seconds=client_time_seconds,
        opcode=opcode,
        evolution_mode=evolution_mode,
        transition_milliseconds=transition_ms,
        hold_milliseconds=hold_ms,
        preset=preset,
        apply_mask=apply_mask,
        clear_mask=clear_mask,
        weather_seed=weather_seed,
        requested_cloud_field_mask=field_mask,
        requested_state_hz=state_hz,
        utc_unix_seconds=utc_unix_seconds,
        latitude_degrees=latitude,
        longitude_degrees=longitude,
        elevation_m=elevation,
        time_scale=time_scale,
        horizontal_extent_m=horizontal_extent,
        vertical_extent_m=vertical_extent,
        surface_temperature_kelvin=temperature,
        sea_level_pressure_pa=pressure,
        relative_humidity=humidity,
        visibility_m=visibility,
        aerosol_optical_depth_550nm=aerosol,
        ozone_dobson_units=ozone,
        wind_enu_m_s=tuple(wind),
        gust_speed_m_s=gust_speed,
        precipitation_rate_mm_h=precipitation,
        snow_fraction=snow_fraction,
        surface_wetness=surface_wetness,
        convective_activity=convection,
        lightning_activity=lightning,
        cloud_layers=tuple(layers),
        atmosphere_optics=optics,
    )
    validate_command(command)
    if apply_mask & APPLY_PRECIPITATION:
        rain_fraction = struct.unpack_from("<f", packet, 164)[0]
        expected_rain_fraction = _as_f32(1.0 - command.snow_fraction)
        phase_difference = abs(_as_f32(rain_fraction - expected_rain_fraction))
        if (
            not math.isfinite(rain_fraction)
            or not 0.0 <= rain_fraction <= 1.0
            or phase_difference > _as_f32(0.0001)
        ):
            raise ValueError("SKC1 rain and snow phase fractions are inconsistent")
    return command


def parse_utc(value: str) -> float:
    try:
        result = float(value)
    except ValueError:
        normalized = value[:-1] + "+00:00" if value.endswith(("Z", "z")) else value
        try:
            parsed = dt.datetime.fromisoformat(normalized)
        except ValueError as error:
            raise argparse.ArgumentTypeError(
                "time must be Unix seconds or ISO-8601 with a UTC offset"
            ) from error
        if parsed.tzinfo is None:
            raise argparse.ArgumentTypeError("ISO-8601 time must include Z or a UTC offset")
        result = parsed.timestamp()
    if not math.isfinite(result) or not -2208988800.0 <= result <= 4133980800.0:
        raise argparse.ArgumentTypeError(
            "time must be between 1900-01-01 and 2101-01-01 UTC"
        )
    return result


def _require_complete_group(args: argparse.Namespace, names: Sequence[str], label: str) -> bool:
    supplied = [getattr(args, name) is not None for name in names]
    if any(supplied) and not all(supplied):
        options = ", ".join("--" + name.replace("_", "-") for name in names)
        raise ValueError(f"{label} is atomic; supply all of {options}")
    return all(supplied)


def build_command(args: argparse.Namespace) -> SkyControlCommand:
    common = {
        "session_id": args.session,
        "sequence": args.sequence,
        "client_time_seconds": time.monotonic(),
        "transition_milliseconds": int(round(args.transition * 1000.0)),
        "weather_seed": args.seed,
    }
    if args.action == "preset":
        preset = PRESET_NAMES[args.preset]
        return SkyControlCommand(
            **common,
            opcode=OPCODE_LOAD_PRESET,
            preset=preset,
            evolution_mode=EVOLUTION_NATURAL if preset == PRESET_NATURAL else EVOLUTION_MANUAL,
        )
    if args.action == "time":
        return SkyControlCommand(**common, opcode=OPCODE_PATCH_OVERRIDE, apply_mask=APPLY_UTC, utc_unix_seconds=args.utc)
    if args.action == "location":
        return SkyControlCommand(
            **common,
            opcode=OPCODE_PATCH_OVERRIDE,
            apply_mask=APPLY_LOCATION,
            latitude_degrees=args.latitude,
            longitude_degrees=args.longitude,
            elevation_m=args.elevation,
        )
    if args.action == "time-scale":
        return SkyControlCommand(**common, opcode=OPCODE_PATCH_OVERRIDE, apply_mask=APPLY_TIME_SCALE, time_scale=args.scale)
    if args.action == "domain":
        return SkyControlCommand(
            **common,
            opcode=OPCODE_PATCH_OVERRIDE,
            apply_mask=APPLY_DOMAIN,
            horizontal_extent_m=args.horizontal,
            vertical_extent_m=args.vertical,
        )
    if args.action == "wind":
        wind = (args.east, args.north, args.up)
        gust = _f32_length(wind) if args.gust is None else args.gust
        return SkyControlCommand(
            **common,
            opcode=OPCODE_PATCH_OVERRIDE,
            apply_mask=APPLY_WIND,
            wind_enu_m_s=wind,
            gust_speed_m_s=gust,
        )
    if args.action == "cloud-layer":
        layers: list[CloudLayer | None] = [None, None, None, None]
        layers[args.index] = CloudLayer(
            kind=args.kind,
            base_altitude_amsl_m=args.base_amsl,
            top_altitude_amsl_m=args.top_amsl,
            coverage=args.coverage,
            optical_depth=args.optical_depth,
            liquid_fraction=args.liquid_fraction,
            precipitation_rate_mm_h=args.precipitation_mm_h,
            convective_activity=args.convection,
        )
        return SkyControlCommand(
            **common,
            opcode=OPCODE_PATCH_OVERRIDE,
            apply_mask=APPLY_CLOUD_LAYER_0 << args.index,
            cloud_layers=tuple(layers),
        )
    if args.action == "weather":
        thermo = _require_complete_group(
            args, ("temperature_c", "pressure_hpa", "humidity"), "thermodynamics"
        )
        visibility = _require_complete_group(
            args, ("visibility_km", "aerosol", "ozone_du"), "visibility"
        )
        precipitation = _require_complete_group(
            args,
            ("precipitation_mm_h", "snow_fraction", "surface_wetness"),
            "precipitation",
        )
        convection = _require_complete_group(
            args, ("convection", "lightning"), "convection"
        )
        apply_mask = (
            (APPLY_THERMODYNAMICS if thermo else 0)
            | (APPLY_VISIBILITY if visibility else 0)
            | (APPLY_PRECIPITATION if precipitation else 0)
            | (APPLY_CONVECTION if convection else 0)
        )
        if not apply_mask:
            raise ValueError("weather requires at least one complete value group")
        return SkyControlCommand(
            **common,
            opcode=OPCODE_PATCH_OVERRIDE,
            apply_mask=apply_mask,
            surface_temperature_kelvin=(args.temperature_c + 273.15) if thermo else 0.0,
            sea_level_pressure_pa=(args.pressure_hpa * 100.0) if thermo else 0.0,
            relative_humidity=args.humidity if thermo else 0.0,
            visibility_m=(args.visibility_km * 1000.0) if visibility else 0.0,
            aerosol_optical_depth_550nm=args.aerosol if visibility else 0.0,
            ozone_dobson_units=args.ozone_du if visibility else 0.0,
            precipitation_rate_mm_h=args.precipitation_mm_h if precipitation else 0.0,
            snow_fraction=args.snow_fraction if precipitation else 0.0,
            surface_wetness=args.surface_wetness if precipitation else 0.0,
            convective_activity=args.convection if convection else 0.0,
            lightning_activity=args.lightning if convection else 0.0,
        )
    if args.action == "release":
        return SkyControlCommand(
            **common,
            opcode=OPCODE_RELEASE_OVERRIDE,
            evolution_mode=EVOLUTION_NATURAL,
            clear_mask=WEATHER_APPLY_MASK,
            preset=PRESET_NATURAL,
        )
    if args.action == "keyframe":
        common["transition_milliseconds"] = 0
        return SkyControlCommand(
            **common,
            opcode=OPCODE_REQUEST_KEYFRAME,
            preset=PRESET_NATURAL,
        )
    raise ValueError(f"unsupported action: {args.action}")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7780)
    parser.add_argument("--source-port", type=int, default=0)
    parser.add_argument("--session", type=lambda value: int(value, 0))
    parser.add_argument("--sequence", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--transition", type=float, default=5.0, help="smooth transition seconds")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--quiet", action="store_true")
    subparsers = parser.add_subparsers(dest="action", required=True)

    preset = subparsers.add_parser("preset", help="load a coherent built-in weather preset")
    preset.add_argument("preset", choices=tuple(PRESET_NAMES))

    set_time = subparsers.add_parser("time", help="set UTC from Unix seconds or ISO-8601")
    set_time.add_argument("utc", type=parse_utc)

    location = subparsers.add_parser("location", help="set geodetic location")
    location.add_argument("latitude", type=float)
    location.add_argument("longitude", type=float)
    location.add_argument("elevation", type=float, help="meters above mean sea level")

    time_scale = subparsers.add_parser("time-scale", help="set simulated UTC seconds per real second")
    time_scale.add_argument("scale", type=float)

    domain = subparsers.add_parser("domain", help="set weather domain dimensions in meters")
    domain.add_argument("horizontal", type=float)
    domain.add_argument("vertical", type=float)

    wind = subparsers.add_parser("wind", help="set ENU mean wind and optional gust speed")
    wind.add_argument("east", type=float)
    wind.add_argument("north", type=float)
    wind.add_argument("up", type=float)
    wind.add_argument("--gust", type=float, help="scalar gust speed in m/s; defaults to mean speed")

    cloud_layer = subparsers.add_parser(
        "cloud-layer", help="patch one cloud layer using AMSL wire altitudes"
    )
    cloud_layer.add_argument("index", type=int, choices=range(4))
    cloud_layer.add_argument(
        "kind", choices=("stratiform", "fog", "convective", "cirrus")
    )
    cloud_layer.add_argument("base_amsl", type=float, help="base meters AMSL")
    cloud_layer.add_argument("top_amsl", type=float, help="top meters AMSL")
    cloud_layer.add_argument("coverage", type=float, help="fraction [0, 1]")
    cloud_layer.add_argument("--optical-depth", type=float, default=20.0)
    cloud_layer.add_argument("--liquid-fraction", type=float, default=1.0)
    cloud_layer.add_argument("--precipitation-mm-h", type=float, default=0.0)
    cloud_layer.add_argument("--convection", type=float, default=0.0)

    weather = subparsers.add_parser(
        "weather",
        help="patch one or more atomic manual-weather groups",
        description=(
            "Each group is atomic: provide every option in a group. "
            "Omitted groups retain their current server values."
        ),
    )
    weather.add_argument("--temperature-c", type=float)
    weather.add_argument("--pressure-hpa", type=float)
    weather.add_argument("--humidity", type=float, help="relative humidity fraction [0.01, 1]")
    weather.add_argument("--visibility-km", type=float)
    weather.add_argument("--aerosol", type=float, help="550 nm aerosol optical depth")
    weather.add_argument("--ozone-du", type=float)
    weather.add_argument("--precipitation-mm-h", type=float)
    weather.add_argument("--snow-fraction", type=float)
    weather.add_argument("--surface-wetness", type=float)
    weather.add_argument("--convection", type=float)
    weather.add_argument("--lightning", type=float)

    subparsers.add_parser("release", help="release manual weather back to natural evolution")
    subparsers.add_parser("keyframe", help="request an immediate SKS1 state packet")

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
        _require_unsigned("seed", args.seed, UINT32_MAX)
        if not math.isfinite(args.transition) or not 0.0 <= args.transition <= 86400.0:
            raise ValueError("transition must be finite and in [0, 86400] seconds")
        command = build_command(args)
        validate_command(command)
    except ValueError as error:
        parser.error(str(error))
    return args


def send_command(
    sock: socket.socket, target: tuple[str, int], command: SkyControlCommand
) -> None:
    packet = pack_command(command)
    sent = sock.sendto(packet, target)
    if sent != len(packet):
        raise OSError(f"short UDP send: {sent}/{len(packet)} bytes")


def run(args: argparse.Namespace) -> int:
    command = build_command(args)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if args.source_port:
            sock.bind(("0.0.0.0", args.source_port))
        send_command(sock, (args.host, args.port), command)
    if not args.quiet:
        print(
            f"sent SKC1 {args.action} to {args.host}:{args.port} "
            f"session=0x{args.session:08x} sequence={args.sequence} "
            f"apply=0x{command.apply_mask:x} clear=0x{command.clear_mask:x}"
        )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    try:
        return run(parse_args(argv))
    except (OSError, ValueError, struct.error) as error:
        print(f"send_sky_control: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
