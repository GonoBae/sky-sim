#!/usr/bin/env python3

import argparse
import math
import struct
import sys
import unittest
import zlib
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import send_interactor as control  # noqa: E402


class SendInteractorTests(unittest.TestCase):
    def upsert(self, **changes):
        values = {
            "session_id": 0x12345678,
            "sequence": 0xFEDCBA98,
            "interactor_id": 0x100000002,
            "client_time": 1234567890.125,
            "opcode": control.OPCODE_UPSERT,
            "shape": control.SHAPE_ELLIPSOID,
            "flags": control.SUPPORTED_FLAGS,
            "ttl_ms": 500,
            "position": (0.25, 0.5, 0.75),
            "rotation": (0.0, 0.0, 0.0, 1.0),
            "half_extent": (0.06, 0.025, 0.015),
            "linear_velocity": (0.3, -0.1, 0.02),
            "angular_velocity": (0.0, 0.0, 1.5),
            "strength": 1.25,
        }
        values.update(changes)
        return control.ControlCommand(**values)

    def test_struct_and_packet_are_exactly_128_bytes(self):
        packet = control.pack_command(self.upsert())
        self.assertEqual(control.CONTROL_PACKET.size, 128)
        self.assertEqual(len(packet), 128)
        self.assertEqual(packet[:4], b"CLC2")
        self.assertEqual(struct.unpack_from("<H", packet, 4)[0], 2)
        self.assertEqual(struct.unpack_from("<H", packet, 6)[0], 128)
        self.assertEqual(struct.unpack_from("<I", packet, 8)[0], 0x12345678)
        self.assertEqual(struct.unpack_from("<Q", packet, 16)[0], 0x100000002)
        self.assertEqual(packet[108:124], bytes(16))

    def test_golden_packet_locks_every_offset_and_endianness(self):
        packet = control.pack_command(self.upsert())
        expected_hex = (
            "434c4332020080007856341298badcfe"
            "0200000001000000000088b48065d241"
            "01040700f40100000000803e0000003f"
            "0000403f000000000000000000000000"
            "0000803f8fc2753dcdcccc3c8fc2753c"
            "9a99993ecdccccbd0ad7a33c00000000"
            "000000000000c03f0000a03f00000000"
            "000000000000000000000000af174886"
        )
        self.assertEqual(packet.hex(), expected_hex)

    def test_crc_covers_first_124_bytes(self):
        packet = control.pack_command(self.upsert())
        encoded_crc = struct.unpack_from("<I", packet, 124)[0]
        self.assertEqual(encoded_crc, zlib.crc32(packet[:124]) & 0xFFFFFFFF)

    def test_strict_round_trip_preserves_fields(self):
        source = self.upsert()
        decoded = control.unpack_command(control.pack_command(source))
        self.assertEqual(decoded.session_id, source.session_id)
        self.assertEqual(decoded.sequence, source.sequence)
        self.assertEqual(decoded.interactor_id, source.interactor_id)
        self.assertEqual(decoded.opcode, source.opcode)
        self.assertEqual(decoded.shape, source.shape)
        self.assertEqual(decoded.flags, source.flags)
        self.assertEqual(decoded.ttl_ms, source.ttl_ms)
        self.assertEqual(decoded.client_time, source.client_time)
        for actual, expected in zip(decoded.position, source.position):
            self.assertAlmostEqual(actual, expected, places=6)
        for actual, expected in zip(decoded.half_extent, source.half_extent):
            self.assertAlmostEqual(actual, expected, places=6)
        self.assertAlmostEqual(decoded.strength, source.strength, places=6)

    def test_corruption_and_nonzero_reserved_data_are_rejected(self):
        packet = bytearray(control.pack_command(self.upsert()))
        packet[60] ^= 0x80
        with self.assertRaisesRegex(ValueError, "CRC32"):
            control.unpack_command(bytes(packet))

        packet = bytearray(control.pack_command(self.upsert()))
        packet[108] = 1
        struct.pack_into("<I", packet, 124, zlib.crc32(packet[:124]) & 0xFFFFFFFF)
        with self.assertRaisesRegex(ValueError, "reserved"):
            control.unpack_command(bytes(packet))

    def test_shape_constraints_are_enforced(self):
        with self.assertRaisesRegex(ValueError, "sphere requires equal"):
            control.pack_command(
                self.upsert(shape=control.SHAPE_SPHERE, half_extent=(0.02, 0.03, 0.02))
            )
        with self.assertRaisesRegex(ValueError, "capsule-z"):
            control.pack_command(
                self.upsert(shape=control.SHAPE_CAPSULE_Z, half_extent=(0.03, 0.02, 0.08))
            )
        packet = control.pack_command(
            self.upsert(shape=control.SHAPE_CAPSULE_Z, half_extent=(0.02, 0.02, 0.08))
        )
        self.assertEqual(len(packet), 128)
        with self.assertRaisesRegex(ValueError, "bounding extent"):
            control.pack_command(
                self.upsert(shape=control.SHAPE_BOX, half_extent=(0.30, 0.30, 0.30))
            )

    def test_normalized_ranges_and_strength_are_enforced(self):
        with self.assertRaisesRegex(ValueError, "position"):
            control.pack_command(self.upsert(position=(1.01, 0.5, 0.5)))
        with self.assertRaisesRegex(ValueError, "normalized"):
            control.pack_command(self.upsert(rotation=(0.0, 0.0, 0.0, 2.0)))
        with self.assertRaisesRegex(ValueError, "strength"):
            control.pack_command(self.upsert(strength=4.01))

    def test_tool_bounds_match_server_control_validation(self):
        packet = control.pack_command(
            self.upsert(
                ttl_ms=2000,
                half_extent=(0.45, 0.2, 0.1),
                linear_velocity=(2.0, 0.0, 0.0),
            )
        )
        self.assertEqual(len(packet), 128)
        with self.assertRaisesRegex(ValueError, "ttl_ms"):
            control.pack_command(self.upsert(ttl_ms=2001))
        with self.assertRaisesRegex(ValueError, "linear_velocity"):
            control.pack_command(self.upsert(linear_velocity=(2.01, 0.0, 0.0)))
        with self.assertRaisesRegex(ValueError, "angular tip speed"):
            control.pack_command(
                self.upsert(half_extent=(0.45, 0.2, 0.1), angular_velocity=(0.0, 0.0, 5.0))
            )
        with self.assertRaisesRegex(ValueError, "client_time"):
            control.pack_command(self.upsert(client_time=-0.01))

    def test_remove_and_clear_have_canonical_zero_tail(self):
        remove = control.ControlCommand(
            session_id=7,
            sequence=8,
            interactor_id=9,
            client_time=10.0,
            opcode=control.OPCODE_REMOVE,
        )
        clear = control.ControlCommand(
            session_id=7,
            sequence=9,
            interactor_id=0,
            client_time=10.0,
            opcode=control.OPCODE_CLEAR,
        )
        self.assertEqual(control.unpack_command(control.pack_command(remove)), remove)
        self.assertEqual(control.unpack_command(control.pack_command(clear)), clear)
        with self.assertRaisesRegex(ValueError, "zero"):
            control.pack_command(clear.__class__(**{**clear.__dict__, "strength": 1.0}))

    def test_helpers_compute_velocity_and_flags(self):
        self.assertEqual(
            control.normalized_velocity((0.1, 0.2, 0.3), (0.7, 0.4, 0.1), 2.0),
            (0.3, 0.1, -0.09999999999999999),
        )
        self.assertEqual(control.parse_flags("solid,wake"), control.FLAG_SOLID | control.FLAG_WAKE)
        self.assertEqual(control.parse_flags("all"), control.SUPPORTED_FLAGS)
        with self.assertRaises(argparse.ArgumentTypeError):
            control.parse_flags("solid,unknown")
        quaternion = control.normalize_quaternion((0.0, 0.0, 0.0, 2.0))
        self.assertTrue(math.isclose(sum(value * value for value in quaternion), 1.0))

    def test_send_emits_one_complete_datagram_to_target(self):
        class RecordingSocket:
            def __init__(self):
                self.calls = []

            def sendto(self, packet, target):
                self.calls.append((packet, target))
                return len(packet)

        sender = RecordingSocket()
        command = self.upsert(sequence=2)
        target = ("127.0.0.1", 7778)
        control.send_command(sender, target, command)
        self.assertEqual(len(sender.calls), 1)
        packet, actual_target = sender.calls[0]
        self.assertEqual(actual_target, target)
        self.assertEqual(control.unpack_command(packet).interactor_id, command.interactor_id)


if __name__ == "__main__":
    unittest.main()
