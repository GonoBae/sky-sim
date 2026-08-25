#!/usr/bin/env python3

import sys
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools import receive_probe as probe  # noqa: E402


class ReceiveProbeTests(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(
            protocol="auto",
            max_field_bytes=1024 * 1024,
            max_buffer_bytes=2 * 1024 * 1024,
        )
        self.source = ("127.0.0.1", 45678)

    def v2_packet(
        self,
        decoded,
        *,
        frame_id=7,
        field_id=1,
        field_mask=1,
        shape=(2, 1, 1),
        voxel_format=2,
        channels=1,
        compression=0,
        encoded=None,
        chunk_index=0,
        chunk_count=1,
        payload_offset=0,
        payload=None,
        flags=1,
    ):
        if encoded is None:
            encoded = decoded
        if payload is None:
            payload = encoded
        header = probe.V2_HEADER.pack(
            b"CLD2",
            2,
            probe.V2_HEADER.size,
            frame_id,
            zlib.crc32(decoded) & 0xFFFFFFFF,
            1.25,
            *shape,
            voxel_format,
            field_id,
            channels,
            compression,
            chunk_index,
            chunk_count,
            len(payload),
            field_mask,
            flags,
            payload_offset,
            len(encoded),
            len(decoded),
            1.0,
            0.0,
        )
        return header + payload

    def test_header_sizes_match_protocol(self):
        self.assertEqual(probe.V1_HEADER.size, 40)
        self.assertEqual(probe.V2_HEADER.size, 64)

    def test_rle_decode(self):
        encoded = bytes([0x82, 9, 2, 1, 2, 3])
        self.assertEqual(probe.rle_decompress(encoded, 8), bytes([9] * 5 + [1, 2, 3]))

    def test_single_field_v2_frame_completes(self):
        decoded = bytes([0, 64, 128, 255])
        frames = {}
        packet = self.v2_packet(decoded)
        frame = probe.process_v2(packet, self.source, frames, self.args)
        self.assertIsNotNone(frame)
        self.assertEqual(frame["fields"][1]["decoded"], decoded)
        duplicate_result = probe.process_v2(packet, self.source, frames, self.args)
        self.assertIs(duplicate_result, frame)

    def test_rle_v2_frame_completes_and_checks_crc(self):
        decoded = bytes([4] * 5 + [1, 2, 3])
        encoded = bytes([0x82, 4, 2, 1, 2, 3])
        packet = self.v2_packet(
            decoded,
            shape=(4, 1, 1),
            compression=1,
            encoded=encoded,
        )
        frame = probe.process_v2(packet, self.source, {}, self.args)
        self.assertEqual(frame["fields"][1]["decoded"], decoded)

    def test_velocity_uses_three_little_endian_snorm16_channels(self):
        decoded = bytes(range(12))
        packet = self.v2_packet(
            decoded,
            field_id=2,
            field_mask=2,
            shape=(2, 1, 1),
            voxel_format=4,
            channels=3,
        )
        frame = probe.process_v2(packet, self.source, {}, self.args)
        self.assertEqual(frame["fields"][2]["decoded"], decoded)

        old_snorm8 = self.v2_packet(
            bytes(range(6)),
            field_id=2,
            field_mask=2,
            shape=(2, 1, 1),
            voxel_format=3,
            channels=3,
        )
        with self.assertRaisesRegex(ValueError, "field format"):
            probe.process_v2(old_snorm8, self.source, {}, self.args)

    def test_all_five_fields_complete_with_consistent_macro_and_occupancy_shapes(self):
        shape = (4, 2, 2)
        field_mask = probe.SUPPORTED_FIELD_MASK
        packets = [
            self.v2_packet(bytes(4 * 2 * 2 * 2), field_id=1, field_mask=field_mask, shape=shape),
            self.v2_packet(
                bytes(4 * 2 * 2 * 3 * 2),
                field_id=2,
                field_mask=field_mask,
                shape=shape,
                voxel_format=4,
                channels=3,
            ),
            self.v2_packet(
                bytes(4 * 2 * 2), field_id=3, field_mask=field_mask,
                shape=shape, voxel_format=1,
            ),
            self.v2_packet(
                bytes(4 * 2 * 2), field_id=4, field_mask=field_mask,
                shape=shape, voxel_format=1,
            ),
            self.v2_packet(
                bytes(2), field_id=5, field_mask=field_mask,
                shape=(2, 1, 1), voxel_format=1,
                flags=probe.FIELD_KEYFRAME_FLAG | (2 << probe.OCCUPANCY_BRICK_SHIFT),
            ),
        ]
        frames = {}
        completed = None
        for packet in packets:
            completed = probe.process_v2(packet, self.source, frames, self.args)
        self.assertIsNotNone(completed)
        self.assertEqual(set(completed["fields"]), {1, 2, 3, 4, 5})

    def test_flags_reject_reserved_bits_and_wrong_field_brick_metadata(self):
        decoded = bytes(4)
        reserved = self.v2_packet(decoded, flags=probe.FIELD_KEYFRAME_FLAG | (1 << 1))
        with self.assertRaisesRegex(ValueError, "reserved"):
            probe.process_v2(reserved, self.source, {}, self.args)

        macro_with_brick = self.v2_packet(
            decoded, flags=probe.FIELD_KEYFRAME_FLAG | (4 << probe.OCCUPANCY_BRICK_SHIFT)
        )
        with self.assertRaisesRegex(ValueError, "only the occupancy"):
            probe.process_v2(macro_with_brick, self.source, {}, self.args)

        occupancy_without_brick = self.v2_packet(
            bytes(1), field_id=5, field_mask=17, shape=(1, 1, 1), voxel_format=1,
        )
        with self.assertRaisesRegex(ValueError, "brick size"):
            probe.process_v2(occupancy_without_brick, self.source, {}, self.args)

        occupancy_without_macro = self.v2_packet(
            bytes(1),
            field_id=5,
            field_mask=16,
            shape=(1, 1, 1),
            voxel_format=1,
            flags=probe.FIELD_KEYFRAME_FLAG | (2 << probe.OCCUPANCY_BRICK_SHIFT),
        )
        with self.assertRaisesRegex(ValueError, "requires at least one macro"):
            probe.process_v2(occupancy_without_macro, self.source, {}, self.args)

    def test_cross_field_shape_and_occupancy_relationships_are_enforced(self):
        frames = {}
        density = self.v2_packet(bytes(4), field_mask=3, shape=(2, 1, 1))
        self.assertIsNone(probe.process_v2(density, self.source, frames, self.args))
        mismatched_velocity = self.v2_packet(
            bytes(3 * 1 * 1 * 3 * 2),
            field_id=2,
            field_mask=3,
            shape=(3, 1, 1),
            voxel_format=4,
            channels=3,
        )
        with self.assertRaisesRegex(ValueError, "same grid shape"):
            probe.process_v2(mismatched_velocity, self.source, frames, self.args)

        frames = {}
        density = self.v2_packet(bytes(8 * 4 * 2 * 2), field_mask=17, shape=(8, 4, 2))
        self.assertIsNone(probe.process_v2(density, self.source, frames, self.args))
        mismatched_occupancy = self.v2_packet(
            bytes(3 * 2 * 1),
            field_id=5,
            field_mask=17,
            shape=(3, 2, 1),
            voxel_format=1,
            flags=probe.FIELD_KEYFRAME_FLAG | (2 << probe.OCCUPANCY_BRICK_SHIFT),
        )
        with self.assertRaisesRegex(ValueError, "occupancy shape"):
            probe.process_v2(mismatched_occupancy, self.source, frames, self.args)

    def test_occupancy_may_arrive_before_its_macro_reference(self):
        frames = {}
        occupancy = self.v2_packet(
            bytes(2),
            field_id=5,
            field_mask=17,
            shape=(2, 1, 1),
            voxel_format=1,
            flags=probe.FIELD_KEYFRAME_FLAG | (2 << probe.OCCUPANCY_BRICK_SHIFT),
        )
        self.assertIsNone(probe.process_v2(occupancy, self.source, frames, self.args))
        density = self.v2_packet(bytes(4 * 2 * 2), field_mask=17, shape=(4, 2, 1))
        completed = probe.process_v2(density, self.source, frames, self.args)
        self.assertIsNotNone(completed)

    def test_oversized_field_is_rejected_before_allocation(self):
        decoded = b"\0"
        packet = self.v2_packet(decoded, shape=(65535, 65535, 1))
        with self.assertRaisesRegex(ValueError, "decoded size"):
            probe.process_v2(packet, self.source, {}, self.args)

    def test_out_of_range_chunk_is_rejected(self):
        decoded = bytes([1, 2, 3, 4])
        packet = self.v2_packet(decoded, chunk_index=1)
        with self.assertRaisesRegex(ValueError, "chunk index"):
            probe.process_v2(packet, self.source, {}, self.args)

    def test_total_buffer_limit_is_enforced(self):
        self.args.max_buffer_bytes = 3
        packet = self.v2_packet(bytes([1, 2, 3, 4]))
        with self.assertRaisesRegex(ValueError, "buffer limit"):
            probe.process_v2(packet, self.source, {}, self.args)

    def test_bad_crc_is_rejected_when_field_completes(self):
        decoded = bytes([1, 2, 3, 4])
        packet = bytearray(self.v2_packet(decoded))
        packet[-1] ^= 0xFF
        with self.assertRaisesRegex(ValueError, "CRC32"):
            probe.process_v2(bytes(packet), self.source, {}, self.args)


if __name__ == "__main__":
    unittest.main()
