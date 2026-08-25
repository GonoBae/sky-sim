#!/usr/bin/env python3

import json
import math
import struct
import sys
import unittest
import zlib
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import receive_sky as sky  # noqa: E402


def make_state_packet() -> bytes:
    packet = bytearray(sky.PACKET_BYTES)
    struct.pack_into("<4sHH", packet, 0, sky.MAGIC, sky.VERSION, sky.PACKET_BYTES)
    struct.pack_into("<IIII", packet, 8, 41, 17, 0x87654321, 93)
    struct.pack_into("<dd", packet, 24, 12.5, 12.7)
    struct.pack_into("<II", packet, 40, (1 << 0) | (1 << 1), 77)
    struct.pack_into("<BBB", packet, 48, 3, 1, 1)
    struct.pack_into("<II", packet, 52, 2, 9)
    struct.pack_into("<HH", packet, 60, 0x1F, 0x1F)

    struct.pack_into("<ddd", packet, 64, 1787648400.0, 37.5665, 126.9780)
    struct.pack_into(
        "<11f",
        packet,
        88,
        38.0,
        60.0,
        20000.0,
        20000.0,
        12000.0,
        293.15,
        101325.0,
        0.55,
        35000.0,
        0.10,
        300.0,
    )
    struct.pack_into("<3f", packet, 132, 4.0, 1.0, 0.0)
    struct.pack_into("<3f", packet, 144, 1.94, 0.485, 0.0)
    struct.pack_into("<10f", packet, 156, 1210.0, 0.0, 0.0, 0.0, 0.0, 0.2, 0.0, 1350.0, 66.0, 0.0)
    struct.pack_into("<3f", packet, 196, 0.18, 0.18, 0.18)
    struct.pack_into("<4f", packet, 208, 0.8, 1200.0, 8000.0, -0.0065)

    layer_offset = 224
    struct.pack_into(
        "<7fBB",
        packet,
        layer_offset,
        1038.0,
        5038.0,
        0.65,
        0.00012,
        0.05,
        0.0,
        2.65,
        2,
        1,
    )

    struct.pack_into("<3f", packet, 352, 0.0, 0.0, 1.0)
    struct.pack_into("<3f", packet, 364, math.radians(0.2666), 1361.0, 120000.0)
    struct.pack_into("<3f", packet, 376, 1.0, 0.0, 0.0)
    struct.pack_into("<3f", packet, 388, math.radians(0.2725), 0.42, 0.1)
    struct.pack_into("<4f", packet, 400, 0.0, 0.0, 0.0, 1.0)
    struct.pack_into("<4f", packet, 416, 1.25, 0.0, 0.0, 0.02)
    struct.pack_into("<3f", packet, 432, 5.802e-6, 13.558e-6, 33.1e-6)
    struct.pack_into("<3f", packet, 444, 3.996e-6, 3.996e-6, 3.996e-6)
    struct.pack_into("<3f", packet, 456, 4.4e-7, 4.4e-7, 4.4e-7)
    struct.pack_into("<3f", packet, 468, 0.650e-6, 1.881e-6, 0.085e-6)
    struct.pack_into("<3f", packet, 480, 1.0, 1.0, 1.0)
    struct.pack_into("<3f", packet, 492, 0.78, 0.84, 1.0)
    struct.pack_into("<I", packet, sky.CRC_OFFSET, zlib.crc32(packet[:sky.CRC_OFFSET]) & 0xFFFFFFFF)
    return bytes(packet)


def with_crc(packet: bytearray) -> bytes:
    struct.pack_into("<I", packet, sky.CRC_OFFSET, zlib.crc32(packet[:sky.CRC_OFFSET]) & 0xFFFFFFFF)
    return bytes(packet)


class ReceiveSkyTests(unittest.TestCase):
    def test_valid_packet_decodes_every_major_section(self):
        state = sky.parse_packet(make_state_packet())
        self.assertEqual(state["sequence"], 41)
        self.assertEqual(state["acknowledgement"]["last_control_session"], 0x87654321)
        self.assertEqual(state["model"]["evolution_mode_name"], "manual")
        self.assertAlmostEqual(state["location"]["latitude_degrees"], 37.5665)
        self.assertAlmostEqual(state["weather"]["surface_temperature_celsius"], 20.0, places=3)
        self.assertAlmostEqual(state["weather"]["wind_speed_m_s"], math.sqrt(17.0), places=6)
        self.assertAlmostEqual(state["celestial"]["sun_elevation_degrees"], 90.0)
        self.assertEqual(len(state["cloud_layers"]), 1)
        layer = state["cloud_layers"][0]
        self.assertEqual(layer["kind"], "convective")
        self.assertAlmostEqual(layer["base_agl_m"], 1000.0)
        self.assertAlmostEqual(layer["coverage"], 0.65, places=6)
        for value in state["atmosphere"]["ground_albedo"]:
            self.assertAlmostEqual(value, 0.18, places=6)

    def test_state_is_json_serializable_and_summary_is_useful(self):
        state = sky.parse_packet(make_state_packet())
        encoded = json.dumps(state, allow_nan=False)
        self.assertIn('"utc_iso8601"', encoded)
        summary = sky.format_summary(state, ("127.0.0.1", 7779))
        self.assertIn("SKS1 state from 127.0.0.1:7779", summary)
        self.assertIn("layer[0] convective", summary)
        self.assertIn("control_ack", summary)

    def test_length_header_crc_and_reserved_bytes_are_strict(self):
        packet = make_state_packet()
        with self.assertRaisesRegex(ValueError, "exactly 512"):
            sky.parse_packet(packet[:-1])

        damaged = bytearray(packet)
        damaged[300] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC32"):
            sky.parse_packet(bytes(damaged))

        reserved = bytearray(packet)
        reserved[504] = 1
        with self.assertRaisesRegex(ValueError, "reserved"):
            sky.parse_packet(with_crc(reserved))

        layer_reserved = bytearray(packet)
        layer_reserved[254] = 1
        with self.assertRaisesRegex(ValueError, "reserved"):
            sky.parse_packet(with_crc(layer_reserved))

    def test_nonfinite_and_non_normalized_celestial_data_are_rejected(self):
        packet = bytearray(make_state_packet())
        struct.pack_into("<d", packet, 32, math.nan)
        with self.assertRaisesRegex(ValueError, "non-finite"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 108, math.nan)
        with self.assertRaisesRegex(ValueError, "non-finite"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<3f", packet, 352, 0.0, 0.0, 0.5)
        with self.assertRaisesRegex(ValueError, "sun direction"):
            sky.parse_packet(with_crc(packet))

    def test_domain_masks_and_inactive_layers_are_validated(self):
        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 100, 19000.0)
        with self.assertRaisesRegex(ValueError, "domain extents"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<H", packet, 62, 0x20)
        with self.assertRaisesRegex(ValueError, "supported subset"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        packet[256] = 1
        with self.assertRaisesRegex(ValueError, "inactive cloud layer"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 120, 200001.0)
        with self.assertRaisesRegex(ValueError, "visibility"):
            sky.parse_packet(with_crc(packet))

    def test_reserved_flags_control_result_rotation_and_phases_are_strict(self):
        packet = bytearray(make_state_packet())
        struct.pack_into("<I", packet, 40, 1 << 6)
        with self.assertRaisesRegex(ValueError, "reserved bits"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        packet[49] = 3
        with self.assertRaisesRegex(ValueError, "outside 0..2"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<4f", packet, 400, 0.0, 0.0, 0.0, 0.5)
        with self.assertRaisesRegex(ValueError, "quaternion"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into(
            "<4f",
            packet,
            400,
            0.5497187376,
            0.6808882356,
            -0.2575999200,
            0.4335238039,
        )
        sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        maximum_precipitation_flux = 300.0 / 3600.0
        struct.pack_into("<f", packet, 160, maximum_precipitation_flux)
        struct.pack_into("<3f", packet, 164, 1.0, 0.0, 0.0)
        struct.pack_into("<f", packet, 244, maximum_precipitation_flux)
        sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 392, -100.0)
        with self.assertRaisesRegex(ValueError, "moon illuminated fraction"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 424, -1.0)
        with self.assertRaisesRegex(ValueError, "aurora intensity"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 160, 1.0 / 3600.0)
        with self.assertRaisesRegex(ValueError, "phase fractions"):
            sky.parse_packet(with_crc(packet))

        packet = bytearray(make_state_packet())
        struct.pack_into("<f", packet, 160, 1.0 / 3600.0)
        struct.pack_into(
            "<3f",
            packet,
            164,
            0.414993017911911,
            0.49542924761772156,
            0.08967772871255875,
        )
        sky.parse_packet(with_crc(packet))


if __name__ == "__main__":
    unittest.main()
