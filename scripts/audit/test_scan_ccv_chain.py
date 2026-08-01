#!/usr/bin/env python3

import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

from scan_ccv_chain import (
    OP_CHECKCONTRACTVERIFY,
    block_hash,
    fnv1a32,
    run_scan,
    script_contains_opcode,
)


def compact_size(value: int) -> bytes:
    if value < 0xFD:
        return bytes([value])
    if value <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", value)
    if value <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", value)
    return b"\xff" + struct.pack("<Q", value)


def varbytes(value: bytes) -> bytes:
    return compact_size(len(value)) + value


def make_transaction(script: bytes) -> bytes:
    result = bytearray(struct.pack("<I", 2))
    result += b"\x00\x01"  # SegWit marker/flag
    result += compact_size(1)
    result += b"\x00" * 32
    result += struct.pack("<I", 0xFFFFFFFF)
    result += varbytes(b"")
    result += struct.pack("<I", 0xFFFFFFFF)
    result += compact_size(1)
    result += struct.pack("<Q", 50_000)
    result += varbytes(b"\x51\x20" + b"\x22" * 32)
    result += compact_size(2)
    result += varbytes(script)
    result += varbytes(b"\xc0" + b"\x33" * 32)
    result += struct.pack("<I", 0)
    return bytes(result)


def make_block(previous_header: bytes | None, script: bytes) -> tuple[bytes, str]:
    header = bytearray(128)
    header[:4] = struct.pack("<I", 1)
    if previous_header is not None:
        previous_digest = hashlib.sha256(
            hashlib.sha256(previous_header).digest()
        ).digest()
        # BlockHeader stores uint256 identity in little-endian byte order.
        header[4:36] = previous_digest[::-1]
    payload = bytes(header) + compact_size(1) + make_transaction(script) + b"\x00"
    return payload, block_hash(bytes(header))


def record(payload: bytes) -> bytes:
    return (
        b"\xfa\xbf\xb5\xda"
        + struct.pack("<I", len(payload))
        + payload
        + struct.pack("<I", fnv1a32(payload))
    )


class ScriptParserTests(unittest.TestCase):
    def test_finds_executable_ccv(self):
        self.assertTrue(script_contains_opcode(b"\x51\xbe", OP_CHECKCONTRACTVERIFY))

    def test_ignores_ccv_byte_inside_direct_push(self):
        self.assertFalse(script_contains_opcode(b"\x01\xbe\x51", OP_CHECKCONTRACTVERIFY))

    def test_ignores_ccv_byte_inside_pushdata1(self):
        self.assertFalse(
            script_contains_opcode(b"\x4c\x01\xbe\x51", OP_CHECKCONTRACTVERIFY)
        )


class ChainSelectionTests(unittest.TestCase):
    def test_walks_explicit_tip_and_counts_revealed_leaf(self):
        genesis_payload, _ = make_block(None, b"\x51")
        second_payload, second_hash = make_block(
            genesis_payload[:128], b"\xbe"
        )
        with tempfile.TemporaryDirectory() as tmp:
            blocks_dir = Path(tmp)
            (blocks_dir / "blk00000.dat").write_bytes(
                record(genesis_payload) + record(second_payload)
            )
            result = run_scan(
                blocks_dir, expected_tip=second_hash, expected_height=1
            )

        self.assertEqual(result.scope, "active-chain-to-explicit-tip")
        self.assertEqual(result.active_genesis, block_hash(genesis_payload[:128]))
        self.assertEqual(result.records_scanned, 2)
        self.assertEqual(result.unique_blocks_scanned, 2)
        self.assertEqual(result.duplicate_records, 0)
        self.assertEqual(result.active_chain_blocks, 2)
        self.assertEqual(result.metrics.transactions, 2)
        self.assertEqual(result.metrics.p2tr_outputs, 2)
        self.assertEqual(result.metrics.revealed_tapscript_leaves, 2)
        self.assertEqual(result.metrics.revealed_ccv_leaves, 1)
        self.assertEqual(len(result.ccv_reveals), 1)

    def test_rejects_wrong_expected_height(self):
        payload, tip = make_block(None, b"\x51")
        with tempfile.TemporaryDirectory() as tmp:
            blocks_dir = Path(tmp)
            (blocks_dir / "blk00000.dat").write_bytes(record(payload))
            with self.assertRaisesRegex(ValueError, "height mismatch"):
                run_scan(blocks_dir, expected_tip=tip, expected_height=9)

    def test_checksum_neuter_fails(self):
        payload, tip = make_block(None, b"\x51")
        damaged = bytearray(record(payload))
        damaged[-5] ^= 1
        with tempfile.TemporaryDirectory() as tmp:
            blocks_dir = Path(tmp)
            (blocks_dir / "blk00000.dat").write_bytes(damaged)
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                run_scan(blocks_dir, expected_tip=tip, expected_height=0)


if __name__ == "__main__":
    unittest.main()
