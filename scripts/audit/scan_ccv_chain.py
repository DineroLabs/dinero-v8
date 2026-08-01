#!/usr/bin/env python3
"""Read-only audit for revealed OP_CHECKCONTRACTVERIFY Taproot leaves.

The scanner reads Dinero's append-only blkNNNNN.dat records, mirrors the
consensus transaction decoder closely enough to locate transaction witnesses,
and follows an explicitly supplied tip back to genesis. Supplying the tip is
important: flat files may also contain stale-fork records.

This can prove that no active-chain spend revealed a Tapscript containing CCV.
It cannot prove that no unspent P2TR output commits to a hidden CCV leaf:
Taproot intentionally conceals unspent script paths.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


OP_PUSHDATA1 = 0x4C
OP_PUSHDATA2 = 0x4D
OP_PUSHDATA4 = 0x4E
OP_CHECKCONTRACTVERIFY = 0xBE
MAX_BLOCK_BYTES = 4_000_000


class DecodeError(ValueError):
    """Raised when an on-disk record or consensus object is malformed."""


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def remaining(self) -> int:
        return len(self.data) - self.pos

    def read(self, size: int) -> bytes:
        if size < 0 or self.pos + size > len(self.data):
            raise DecodeError(
                f"read beyond end at offset {self.pos}: need {size}, "
                f"have {self.remaining()}"
            )
        value = self.data[self.pos : self.pos + size]
        self.pos += size
        return value

    def u8(self) -> int:
        return self.read(1)[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.read(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.read(8))[0]

    def compact_size(self) -> int:
        first = self.u8()
        if first < 0xFD:
            return first
        if first == 0xFD:
            return struct.unpack("<H", self.read(2))[0]
        if first == 0xFE:
            return self.u32()
        return self.u64()

    def varbytes(self) -> bytes:
        return self.read(self.compact_size())


@dataclass
class BlockMetrics:
    transactions: int = 0
    inputs: int = 0
    outputs: int = 0
    p2tr_outputs: int = 0
    script_path_witnesses: int = 0
    revealed_tapscript_leaves: int = 0
    revealed_ccv_leaves: int = 0

    def add(self, other: "BlockMetrics") -> None:
        for name in self.__dataclass_fields__:
            setattr(self, name, getattr(self, name) + getattr(other, name))


@dataclass
class ParsedBlock:
    block_hash: str
    previous_hash: str
    file: str
    offset: int
    payload_sha256: str
    transaction_section_sha256: str
    metrics: BlockMetrics


@dataclass
class ScanResult:
    schema_version: int = 1
    scope: str = "all-stored-records"
    expected_tip: str | None = None
    expected_height: int | None = None
    active_genesis: str | None = None
    active_tip: str | None = None
    active_height: int | None = None
    files_scanned: list[str] = field(default_factory=list)
    records_scanned: int = 0
    unique_blocks_scanned: int = 0
    duplicate_records: int = 0
    alternate_body_records: int = 0
    active_chain_blocks: int = 0
    stale_or_unselected_records: int = 0
    metrics: BlockMetrics = field(default_factory=BlockMetrics)
    ccv_reveals: list[dict[str, object]] = field(default_factory=list)
    conclusion: str = ""
    limitation: str = (
        "Zero revealed CCV leaves cannot exclude hidden, unspent CCV leaves "
        "committed inside P2TR outputs."
    )


def display_hash(raw_digest_or_uint256: bytes) -> str:
    if len(raw_digest_or_uint256) != 32:
        raise DecodeError("hash must be 32 bytes")
    return raw_digest_or_uint256[::-1].hex()


def block_hash(header: bytes) -> str:
    if len(header) != 128:
        raise DecodeError("Dinero block header must be 128 bytes")
    # BlockHeader::GetHash reverses the SHA digest into uint256's internal
    # little-endian storage; uint256::GetHex reverses it back for display.
    return hashlib.sha256(hashlib.sha256(header).digest()).digest().hex()


def fnv1a32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def script_contains_opcode(script: bytes, wanted: int) -> bool:
    """Find an opcode while excluding identically valued pushed data bytes."""
    pc = 0
    while pc < len(script):
        opcode = script[pc]
        pc += 1
        if opcode == wanted:
            return True
        if 1 <= opcode <= 0x4B:
            push_size = opcode
        elif opcode == OP_PUSHDATA1:
            if pc + 1 > len(script):
                return False
            push_size = script[pc]
            pc += 1
        elif opcode == OP_PUSHDATA2:
            if pc + 2 > len(script):
                return False
            push_size = struct.unpack("<H", script[pc : pc + 2])[0]
            pc += 2
        elif opcode == OP_PUSHDATA4:
            if pc + 4 > len(script):
                return False
            push_size = struct.unpack("<I", script[pc : pc + 4])[0]
            pc += 4
        else:
            continue
        if pc + push_size > len(script):
            return False
        pc += push_size
    return False


def is_p2tr(script_pubkey: bytes) -> bool:
    return len(script_pubkey) == 34 and script_pubkey[:2] == b"\x51\x20"


def is_control_block(value: bytes) -> bool:
    return (
        33 <= len(value) <= 4129
        and (len(value) - 33) % 32 == 0
        and value[0] & 0xFE == 0xC0
    )


def parse_transaction(reader: Reader, metrics: BlockMetrics) -> None:
    version = reader.u32()
    is_segwit = (
        reader.remaining() >= 2
        and reader.data[reader.pos : reader.pos + 2] == b"\x00\x01"
    )
    if is_segwit:
        reader.read(2)

    input_count = reader.compact_size()
    metrics.inputs += input_count
    for _ in range(input_count):
        reader.read(32)
        reader.u32()
        reader.varbytes()
        reader.u32()

    output_count = reader.compact_size()
    metrics.outputs += output_count
    has_confidential_outputs = False
    for _ in range(output_count):
        value = reader.u64()
        script_pubkey = reader.varbytes()
        if is_p2tr(script_pubkey):
            metrics.p2tr_outputs += 1

        # Mirror TransactionSerializer::Deserialize: a zero-valued output is
        # confidential only if three following varbytes decode with the exact
        # structural commitment and nonce lengths. Otherwise rewind.
        if value == 0:
            after_script = reader.pos
            try:
                commitment = reader.varbytes()
                reader.varbytes()  # range proof
                nonce = reader.varbytes()
                if len(commitment) == 33 and len(nonce) == 65:
                    has_confidential_outputs = True
                else:
                    reader.pos = after_script
            except DecodeError:
                reader.pos = after_script

    is_shielded = version in (5, 6)
    if has_confidential_outputs or is_shielded:
        fee_marker = reader.u8()
        if fee_marker == 1:
            reader.u64()
        elif fee_marker != 0:
            raise DecodeError(f"invalid explicit fee marker {fee_marker:#x}")

    witnesses: list[list[bytes]] = []
    if is_segwit:
        for _ in range(input_count):
            witnesses.append(
                [reader.varbytes() for _ in range(reader.compact_size())]
            )

    if is_shielded:
        reader.varbytes()

    reader.u32()  # locktime

    for witness in witnesses:
        if len(witness) < 2:
            continue
        metrics.script_path_witnesses += 1
        control_block = witness[-1]
        if not is_control_block(control_block):
            continue
        metrics.revealed_tapscript_leaves += 1
        script = witness[-2]
        if script_contains_opcode(script, OP_CHECKCONTRACTVERIFY):
            metrics.revealed_ccv_leaves += 1


def parse_block(payload: bytes, file: Path, offset: int) -> ParsedBlock:
    if len(payload) < 129 or len(payload) > MAX_BLOCK_BYTES:
        raise DecodeError(f"invalid block payload size {len(payload)}")
    header = payload[:128]
    reader = Reader(payload)
    reader.read(128)
    tx_count = reader.compact_size()
    metrics = BlockMetrics(transactions=tx_count)
    for _ in range(tx_count):
        parse_transaction(reader, metrics)

    # Block::Deserialize accepts either end-of-record (legacy) or an optional
    # Utreexo flag/payload here. Transaction parsing must never consume beyond
    # the record; the Utreexo body is irrelevant to witness inspection.
    if reader.pos > len(payload):
        raise DecodeError("transaction parser consumed beyond block payload")

    return ParsedBlock(
        block_hash=block_hash(header),
        previous_hash=display_hash(header[4:36]),
        file=str(file),
        offset=offset,
        payload_sha256=hashlib.sha256(payload).hexdigest(),
        transaction_section_sha256=hashlib.sha256(
            payload[128 : reader.pos]
        ).hexdigest(),
        metrics=metrics,
    )


def scan_block_files(
    paths: Iterable[Path],
) -> tuple[dict[str, ParsedBlock], int, int, int]:
    blocks: dict[str, ParsedBlock] = {}
    magic: bytes | None = None
    record_count = 0
    duplicate_count = 0
    alternate_body_count = 0
    for path in paths:
        with path.open("rb") as handle:
            offset = 0
            while True:
                prefix = handle.read(8)
                if not prefix:
                    break
                if len(prefix) != 8:
                    raise DecodeError(f"{path}:{offset}: truncated record header")
                record_magic, payload_size = prefix[:4], struct.unpack("<I", prefix[4:])[0]
                if magic is None:
                    magic = record_magic
                elif record_magic != magic:
                    raise DecodeError(f"{path}:{offset}: network magic changed")
                if payload_size < 129 or payload_size > MAX_BLOCK_BYTES:
                    raise DecodeError(
                        f"{path}:{offset}: invalid payload size {payload_size}"
                    )
                payload = handle.read(payload_size)
                checksum_bytes = handle.read(4)
                if len(payload) != payload_size or len(checksum_bytes) != 4:
                    raise DecodeError(f"{path}:{offset}: truncated block record")
                stored_checksum = struct.unpack("<I", checksum_bytes)[0]
                actual_checksum = fnv1a32(payload)
                if stored_checksum != actual_checksum:
                    raise DecodeError(
                        f"{path}:{offset}: checksum mismatch: "
                        f"stored={stored_checksum:#x} actual={actual_checksum:#x}"
                    )
                block = parse_block(payload, path, offset)
                if block.block_hash in blocks:
                    existing = blocks[block.block_hash]
                    if (
                        existing.transaction_section_sha256
                        != block.transaction_section_sha256
                    ):
                        raise DecodeError(
                            "same block header has different transactions: "
                            f"{block.block_hash}"
                        )
                    if existing.payload_sha256 != block.payload_sha256:
                        alternate_body_count += 1
                    duplicate_count += 1
                else:
                    blocks[block.block_hash] = block
                record_count += 1
                offset += 12 + payload_size
    return blocks, record_count, duplicate_count, alternate_body_count


def select_chain(
    blocks: dict[str, ParsedBlock], expected_tip: str
) -> list[ParsedBlock]:
    chain: list[ParsedBlock] = []
    seen: set[str] = set()
    current = expected_tip.lower()
    zero_hash = "00" * 32
    while current != zero_hash:
        if current in seen:
            raise DecodeError(f"cycle while walking active chain at {current}")
        seen.add(current)
        block = blocks.get(current)
        if block is None:
            raise DecodeError(f"active-chain block missing from flat files: {current}")
        chain.append(block)
        current = block.previous_hash
    chain.reverse()
    return chain


def run_scan(
    blocks_dir: Path,
    expected_tip: str | None = None,
    expected_height: int | None = None,
) -> ScanResult:
    paths = sorted(blocks_dir.glob("blk[0-9][0-9][0-9][0-9][0-9].dat"))
    if not paths:
        raise DecodeError(f"no blkNNNNN.dat files under {blocks_dir}")
    blocks, record_count, duplicate_count, alternate_body_count = scan_block_files(
        paths
    )

    result = ScanResult(
        files_scanned=[path.name for path in paths],
        records_scanned=record_count,
        unique_blocks_scanned=len(blocks),
        duplicate_records=duplicate_count,
        alternate_body_records=alternate_body_count,
        expected_tip=expected_tip.lower() if expected_tip else None,
        expected_height=expected_height,
    )

    if expected_tip:
        selected = select_chain(blocks, expected_tip)
        result.scope = "active-chain-to-explicit-tip"
        result.active_genesis = selected[0].block_hash
        result.active_tip = expected_tip.lower()
        result.active_height = len(selected) - 1
        result.active_chain_blocks = len(selected)
        result.stale_or_unselected_records = len(blocks) - len(selected)
        if expected_height is not None and result.active_height != expected_height:
            raise DecodeError(
                f"active height mismatch: walked {result.active_height}, "
                f"expected {expected_height}"
            )
    else:
        selected = list(blocks.values())
        result.active_chain_blocks = 0

    for block in selected:
        result.metrics.add(block.metrics)
        if block.metrics.revealed_ccv_leaves:
            result.ccv_reveals.append(
                {
                    "block_hash": block.block_hash,
                    "file": block.file,
                    "offset": block.offset,
                    "revealed_ccv_leaves": block.metrics.revealed_ccv_leaves,
                }
            )

    if result.metrics.revealed_ccv_leaves == 0:
        result.conclusion = (
            "No selected block reveals a Taproot script leaf containing "
            "OP_CHECKCONTRACTVERIFY."
        )
    else:
        result.conclusion = (
            f"Found {result.metrics.revealed_ccv_leaves} revealed Taproot "
            "script leaf/leaves containing OP_CHECKCONTRACTVERIFY."
        )
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--blocks-dir",
        required=True,
        type=Path,
        help="Directory containing blkNNNNN.dat files",
    )
    parser.add_argument(
        "--expected-tip",
        help="RPC-reported active tip hash; required for an active-chain claim",
    )
    parser.add_argument(
        "--expected-height",
        type=int,
        help="RPC-reported active height; verifies a complete genesis-to-tip walk",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write JSON evidence to this file instead of stdout",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        result = run_scan(
            args.blocks_dir,
            expected_tip=args.expected_tip,
            expected_height=args.expected_height,
        )
    except (DecodeError, OSError) as exc:
        print(f"scan failed: {exc}", file=sys.stderr)
        return 1

    rendered = json.dumps(asdict(result), indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
