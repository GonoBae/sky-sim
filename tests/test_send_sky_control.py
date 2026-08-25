#!/usr/bin/env python3

import argparse
import hashlib
import math
import socket
import struct
import sys
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import send_sky_control as control  # noqa: E402


class SendSkyControlTests(unittest.TestCase):
    def patch(self, **changes):
        values = {
            "session_id": 0x12345678,
            "sequence": 0xFEDCBA98,
            "client_time_seconds": 1234567890.125,
            "opcode": control.OPCODE_PATCH_OVERRIDE,
            "apply_mask": control.APPLY_UTC,
            "utc_unix_seconds": 1787648400.0,
        }
        values.update(changes)
        return control.SkyControlCommand(**values)

    def test_packet_header_offsets_size_zero_fill_and_crc(self):
        packet = control.pack_command(self.patch())
        self.assertEqual(len(packet), 512)
        self.assertEqual(packet[:4], b"SKC1")
        self.assertEqual(struct.unpack_from("<H", packet, 4)[0], 1)
        self.assertEqual(struct.unpack_from("<H", packet, 6)[0], 512)
        self.assertEqual(struct.unpack_from("<I", packet, 8)[0], 0x12345678)
        self.assertEqual(struct.unpack_from("<I", packet, 12)[0], 0xFEDCBA98)
        self.assertEqual(struct.unpack_from("<Q", packet, 40)[0], control.APPLY_UTC)
        self.assertEqual(packet[26:28], bytes(2))
        self.assertEqual(packet[72:508], bytes(436))
        encoded_crc = struct.unpack_from("<I", packet, 508)[0]
        self.assertEqual(encoded_crc, zlib.crc32(packet[:508]) & 0xFFFFFFFF)

    def test_golden_packet_digest_locks_all_512_bytes(self):
        packet = control.pack_command(self.patch())
        self.assertEqual(
            packet[:72].hex(),
            "534b4331010000027856341298badcfe"
            "000088b48065d2410103000088130000"
            "00000000080000000100000000000000"
            "00000000000000000100000000000000"
            "0000006456a3da41",
        )
        self.assertEqual(
            hashlib.sha256(packet).hexdigest(),
            "5820f1040327a78397b767e6bbcf9b6e259cecc0ac85cb603e0153caca05c210",
        )

    def test_preset_time_location_and_time_scale_round_trip(self):
        preset = control.SkyControlCommand(
            session_id=7,
            sequence=8,
            client_time_seconds=9.0,
            opcode=control.OPCODE_LOAD_PRESET,
            preset=control.PRESET_STORM,
            transition_milliseconds=2500,
            weather_seed=99,
        )
        decoded = control.unpack_command(control.pack_command(preset))
        self.assertEqual(decoded.preset, control.PRESET_STORM)
        self.assertEqual(decoded.weather_seed, 99)

        location = self.patch(
            apply_mask=control.APPLY_LOCATION,
            utc_unix_seconds=0.0,
            latitude_degrees=37.5665,
            longitude_degrees=126.978,
            elevation_m=38.0,
        )
        decoded = control.unpack_command(control.pack_command(location))
        self.assertAlmostEqual(decoded.latitude_degrees, 37.5665)
        self.assertAlmostEqual(decoded.longitude_degrees, 126.978)
        self.assertAlmostEqual(decoded.elevation_m, 38.0)

        scale = self.patch(
            apply_mask=control.APPLY_TIME_SCALE,
            utc_unix_seconds=0.0,
            time_scale=60.0,
        )
        decoded = control.unpack_command(control.pack_command(scale))
        self.assertEqual(decoded.time_scale, 60.0)

    def test_manual_weather_groups_use_wire_units_and_canonical_zeros(self):
        command = self.patch(
            apply_mask=(
                control.APPLY_THERMODYNAMICS
                | control.APPLY_VISIBILITY
                | control.APPLY_PRECIPITATION
                | control.APPLY_CONVECTION
            ),
            utc_unix_seconds=0.0,
            surface_temperature_kelvin=297.15,
            sea_level_pressure_pa=100400.0,
            relative_humidity=0.82,
            visibility_m=12000.0,
            aerosol_optical_depth_550nm=0.15,
            ozone_dobson_units=310.0,
            precipitation_rate_mm_h=18.0,
            snow_fraction=0.25,
            surface_wetness=0.6,
            convective_activity=0.7,
            lightning_activity=0.2,
        )
        packet = control.pack_command(command)
        self.assertAlmostEqual(struct.unpack_from("<f", packet, 160)[0], 18.0 / 3600.0)
        self.assertAlmostEqual(struct.unpack_from("<f", packet, 164)[0], 0.75)
        self.assertEqual(packet[172:176], bytes(4))
        self.assertAlmostEqual(struct.unpack_from("<f", packet, 184)[0], 2100.0)
        decoded = control.unpack_command(packet)
        self.assertAlmostEqual(decoded.precipitation_rate_mm_h, 18.0, places=4)
        self.assertAlmostEqual(decoded.convective_activity, 0.7, places=6)

        minimums = self.patch(
            apply_mask=control.APPLY_THERMODYNAMICS | control.APPLY_VISIBILITY,
            utc_unix_seconds=0.0,
            surface_temperature_kelvin=203.15,
            sea_level_pressure_pa=80000.0,
            relative_humidity=0.01,
            visibility_m=25.0,
            aerosol_optical_depth_550nm=0.005,
            ozone_dobson_units=100.0,
        )
        control.unpack_command(control.pack_command(minimums))

    def test_wind_encodes_enu_gust_delta_and_round_trips(self):
        command = self.patch(
            apply_mask=control.APPLY_WIND,
            utc_unix_seconds=0.0,
            wind_enu_m_s=(8.0, 2.0, 0.0),
            gust_speed_m_s=13.0,
        )
        packet = control.pack_command(command)
        wind = struct.unpack_from("<3f", packet, 132)
        gust_delta = struct.unpack_from("<3f", packet, 144)
        self.assertEqual(wind, (8.0, 2.0, 0.0))
        self.assertAlmostEqual(math.sqrt(sum(v * v for v in gust_delta)), 13.0 - math.sqrt(68.0), places=5)
        decoded = control.unpack_command(packet)
        self.assertAlmostEqual(decoded.gust_speed_m_s, 13.0, places=5)

        calm_gust = self.patch(
            apply_mask=control.APPLY_WIND,
            utc_unix_seconds=0.0,
            wind_enu_m_s=(0.0, 0.0, 0.0),
            gust_speed_m_s=20.0,
        )
        packet = control.pack_command(calm_gust)
        self.assertEqual(struct.unpack_from("<3f", packet, 144), (20.0, 0.0, 0.0))
        self.assertAlmostEqual(control.unpack_command(packet).gust_speed_m_s, 20.0)

        high_precision = self.patch(
            apply_mask=control.APPLY_WIND,
            utc_unix_seconds=0.0,
            wind_enu_m_s=(67.65208461961127, 36.84660504724732, 46.397157856677474),
            gust_speed_m_s=200.0,
        )
        packet = control.pack_command(high_precision)
        decoded = control.unpack_command(packet)
        self.assertAlmostEqual(decoded.gust_speed_m_s, 200.0, places=4)

        quantized_boundary = self.patch(
            apply_mask=control.APPLY_WIND,
            utc_unix_seconds=0.0,
            wind_enu_m_s=(0.0411864, 0.122675, 0.585879),
            gust_speed_m_s=200.0,
        )
        decoded = control.unpack_command(control.pack_command(quantized_boundary))
        self.assertAlmostEqual(decoded.gust_speed_m_s, 200.0, places=3)

    def test_cloud_layer_command_uses_amsl_and_derives_kind_flags(self):
        layer = control.CloudLayer(
            kind="convective",
            base_altitude_amsl_m=1200.0,
            top_altitude_amsl_m=9000.0,
            coverage=0.65,
            optical_depth=35.0,
            liquid_fraction=0.8,
            precipitation_rate_mm_h=12.0,
            convective_activity=0.6,
        )
        command = self.patch(
            apply_mask=control.APPLY_CLOUD_LAYER_1,
            utc_unix_seconds=0.0,
            cloud_layers=(None, layer, None, None),
        )
        packet = control.pack_command(command)
        offset = 224 + 32
        self.assertEqual(struct.unpack_from("<f", packet, offset)[0], 1200.0)
        self.assertEqual(struct.unpack_from("<f", packet, offset + 4)[0], 9000.0)
        self.assertAlmostEqual(struct.unpack_from("<f", packet, offset + 8)[0], 0.65, places=6)
        self.assertEqual(packet[offset + 28], control.CLOUD_KIND_DEEP_CONVECTIVE)
        self.assertEqual(packet[offset + 29], 3)
        decoded = control.unpack_command(packet)
        decoded_layer = decoded.cloud_layers[1]
        self.assertIsNotNone(decoded_layer)
        self.assertAlmostEqual(decoded_layer.coverage, 0.65, places=6)

        precision_layer = control.CloudLayer(
            kind="stratiform",
            base_altitude_amsl_m=-42.2754173,
            top_altitude_amsl_m=290.1203918,
            coverage=0.5,
            optical_depth=332.395813,
        )
        precision_packet = control.pack_command(
            self.patch(
                apply_mask=control.APPLY_CLOUD_LAYER_0,
                utc_unix_seconds=0.0,
                cloud_layers=(precision_layer, None, None, None),
            )
        )
        control.unpack_command(precision_packet)

        optical_boundary_layer = control.CloudLayer(
            kind="stratiform",
            base_altitude_amsl_m=32479.4,
            top_altitude_amsl_m=33787.0,
            coverage=0.8,
            optical_depth=500.0,
            precipitation_rate_mm_h=300.0,
            convective_activity=1.0,
        )
        optical_boundary_packet = control.pack_command(
            self.patch(
                apply_mask=control.APPLY_CLOUD_LAYER_0,
                utc_unix_seconds=0.0,
                cloud_layers=(optical_boundary_layer, None, None, None),
            )
        )
        decoded_boundary = control.unpack_command(optical_boundary_packet)
        self.assertEqual(decoded_boundary.cloud_layers[0].optical_depth, 500.0)

        with self.assertRaisesRegex(ValueError, "wire precision"):
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_CLOUD_LAYER_0,
                    utc_unix_seconds=0.0,
                    cloud_layers=(
                        control.CloudLayer(
                            base_altitude_amsl_m=0.0,
                            top_altitude_amsl_m=100.0,
                            optical_depth=500.0,
                        ),
                        None,
                        None,
                        None,
                    ),
                )
            )

    def test_corruption_reserved_and_noncanonical_unused_bytes_are_rejected(self):
        packet = bytearray(control.pack_command(self.patch()))
        packet[80] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC32"):
            control.unpack_command(bytes(packet))

        packet = bytearray(control.pack_command(self.patch()))
        packet[504] = 1
        struct.pack_into("<I", packet, 508, zlib.crc32(packet[:508]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "reserved"):
            control.unpack_command(bytes(packet))

        packet = bytearray(control.pack_command(self.patch()))
        packet[120] = 1
        struct.pack_into("<I", packet, 508, zlib.crc32(packet[:508]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "unused bytes"):
            control.unpack_command(bytes(packet))

        layer = control.CloudLayer(
            kind="convective",
            base_altitude_amsl_m=1000.0,
            top_altitude_amsl_m=4000.0,
            coverage=0.5,
            optical_depth=20.0,
        )
        packet = bytearray(
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_CLOUD_LAYER_0,
                    utc_unix_seconds=0.0,
                    cloud_layers=(layer, None, None, None),
                )
            )
        )
        struct.pack_into("<f", packet, 224 + 24, math.nan)
        struct.pack_into("<I", packet, 508, zlib.crc32(packet[:508]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "non-finite"):
            control.unpack_command(bytes(packet))

        packet = bytearray(
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_CLOUD_LAYER_0,
                    utc_unix_seconds=0.0,
                    cloud_layers=(layer, None, None, None),
                )
            )
        )
        struct.pack_into("<f", packet, 224 + 24, 100.0)
        struct.pack_into("<I", packet, 508, zlib.crc32(packet[:508]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "turbulence"):
            control.unpack_command(bytes(packet))

        packet = bytearray(
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_PRECIPITATION,
                    utc_unix_seconds=0.0,
                    precipitation_rate_mm_h=1.0,
                    snow_fraction=0.25,
                    surface_wetness=0.0,
                )
            )
        )
        struct.pack_into("<f", packet, 164, 0.8870903254)
        struct.pack_into("<f", packet, 168, 0.1130096689)
        struct.pack_into("<I", packet, 508, zlib.crc32(packet[:508]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "inconsistent"):
            control.unpack_command(bytes(packet))

    def test_server_ranges_and_reserved_request_fields_are_enforced(self):
        with self.assertRaisesRegex(ValueError, "latitude"):
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_LOCATION,
                    latitude_degrees=91.0,
                    longitude_degrees=0.0,
                    elevation_m=0.0,
                )
            )
        with self.assertRaisesRegex(ValueError, "gust speed"):
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_WIND,
                    wind_enu_m_s=(10.0, 0.0, 0.0),
                    gust_speed_m_s=9.0,
                )
            )
        with self.assertRaisesRegex(ValueError, "reserved"):
            control.pack_command(self.patch(requested_state_hz=5))
        packet = bytearray(control.pack_command(self.patch()))
        struct.pack_into("<I", packet, 36, 256)
        struct.pack_into("<I", packet, 508, zlib.crc32(packet[:508]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "preset"):
            control.unpack_command(bytes(packet))
        with self.assertRaisesRegex(ValueError, "complete weather clear mask"):
            control.pack_command(
                control.SkyControlCommand(
                    session_id=1,
                    sequence=1,
                    client_time_seconds=1.0,
                    opcode=control.OPCODE_RELEASE_OVERRIDE,
                    clear_mask=control.APPLY_WIND,
                )
            )

    def test_time_parser_and_weather_atomic_groups(self):
        self.assertEqual(control.parse_utc("1970-01-01T00:00:00Z"), 0.0)
        self.assertEqual(control.parse_utc("0"), 0.0)
        self.assertEqual(control.parse_utc("2100-12-31T23:59:59Z"), 4133980799.0)
        decoded = control.unpack_command(
            control.pack_command(
                self.patch(utc_unix_seconds=4133980799.0)
            )
        )
        self.assertEqual(decoded.utc_unix_seconds, 4133980799.0)
        with self.assertRaises(argparse.ArgumentTypeError):
            control.parse_utc("2026-08-25T12:00:00")

        args = SimpleNamespace(
            session=1,
            sequence=1,
            transition=5.0,
            seed=1,
            action="weather",
            temperature_c=20.0,
            pressure_hpa=None,
            humidity=0.5,
            visibility_km=None,
            aerosol=None,
            ozone_du=None,
            precipitation_mm_h=None,
            snow_fraction=None,
            surface_wetness=None,
            convection=None,
            lightning=None,
        )
        with self.assertRaisesRegex(ValueError, "atomic"):
            control.build_command(args)

    def test_evolution_patch_rejects_unimplemented_and_conflicting_modes(self):
        with self.assertRaisesRegex(ValueError, "only natural or manual"):
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_EVOLUTION,
                    utc_unix_seconds=0.0,
                    evolution_mode=control.EVOLUTION_TIMELINE,
                )
            )
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            control.pack_command(
                self.patch(
                    apply_mask=control.APPLY_EVOLUTION | control.APPLY_WIND,
                    utc_unix_seconds=0.0,
                    evolution_mode=control.EVOLUTION_NATURAL,
                    wind_enu_m_s=(1.0, 0.0, 0.0),
                    gust_speed_m_s=1.0,
                )
            )

    def test_send_emits_one_complete_datagram(self):
        class RecordingSocket:
            def __init__(self):
                self.calls = []

            def sendto(self, packet, target):
                self.calls.append((packet, target))
                return len(packet)

        sender = RecordingSocket()
        command = self.patch()
        control.send_command(sender, ("127.0.0.1", 7780), command)
        self.assertEqual(len(sender.calls), 1)
        self.assertEqual(len(sender.calls[0][0]), 512)
        self.assertEqual(sender.calls[0][1], ("127.0.0.1", 7780))


if __name__ == "__main__":
    unittest.main()
