#!/usr/bin/env python3

import argparse
import json
import re
import sys
from pathlib import Path


BLOCK_FILE_RE = re.compile(r"^block(\d+)\.hex$")
NETWORK_MAGIC = {
    "mainnet": 0xD1A0C0DE,
    "testnet": 0xDAB5BFFA,
    "regtest": 0xFABFB5DA,
}
DEFAULT_MAX_FILE_SIZE = 128 * 1024 * 1024


def fnv1a_checksum(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pack exported blockNNN.hex files into canonical blk*.dat flatfiles."
    )
    parser.add_argument("--input-dir", required=True, help="Directory containing blockNNN.hex files")
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory where blk*.dat files should be written",
    )
    parser.add_argument(
        "--network",
        choices=sorted(NETWORK_MAGIC.keys()),
        default="mainnet",
        help="Network magic to encode into blk*.dat records",
    )
    parser.add_argument("--start-height", type=int, help="First height to pack")
    parser.add_argument("--end-height", type=int, help="Last height to pack")
    parser.add_argument(
        "--allow-holes",
        action="store_true",
        help="Allow gaps in the selected height range instead of failing closed",
    )
    parser.add_argument(
        "--max-file-size",
        type=int,
        default=DEFAULT_MAX_FILE_SIZE,
        help="Rotate blk files after this many bytes (default: 128 MiB)",
    )
    parser.add_argument(
        "--manifest",
        help="Optional path for a JSON packing manifest",
    )
    return parser.parse_args()


def parse_height(path: Path) -> int | None:
    match = BLOCK_FILE_RE.match(path.name)
    if not match:
        return None
    return int(match.group(1))


def collect_exports(input_dir: Path) -> dict[int, Path]:
    exports: dict[int, Path] = {}
    for path in input_dir.iterdir():
        if not path.is_file():
            continue
        height = parse_height(path)
        if height is None or path.stat().st_size <= 0:
            continue
        exports[height] = path
    return exports


def select_heights(exports: dict[int, Path], start_height: int | None, end_height: int | None) -> tuple[list[int], list[int]]:
    if not exports:
        raise RuntimeError("No blockNNN.hex files found")

    selected_start = min(exports) if start_height is None else start_height
    selected_end = max(exports) if end_height is None else end_height
    if selected_end < selected_start:
        raise RuntimeError(f"end height {selected_end} is below start height {selected_start}")

    heights = [height for height in sorted(exports) if selected_start <= height <= selected_end]
    missing = [height for height in range(selected_start, selected_end + 1) if height not in exports]
    return heights, missing


def write_record(block_file, magic: int, raw_block: bytes) -> int:
    checksum = fnv1a_checksum(raw_block)
    record = (
        magic.to_bytes(4, "little")
        + len(raw_block).to_bytes(4, "little")
        + raw_block
        + checksum.to_bytes(4, "little")
    )
    block_file.write(record)
    return len(record)


def open_block_file(output_dir: Path, file_number: int):
    path = output_dir / f"blk{file_number:05d}.dat"
    return path, path.open("wb")


def main() -> int:
    args = parse_args()
    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    exports = collect_exports(input_dir)
    heights, missing = select_heights(exports, args.start_height, args.end_height)
    if not heights:
        raise SystemExit("No exported block files matched the requested height range")
    selected_start = heights[0]
    selected_end = heights[-1]

    if missing and not args.allow_holes:
        raise SystemExit(
            "Refusing to pack a non-contiguous archive. Missing heights inside "
            f"{selected_start}-{selected_end}: {missing[:50]}"
        )

    magic = NETWORK_MAGIC[args.network]
    file_number = 0
    bytes_in_file = 0
    total_bytes = 0
    packed_records = 0
    output_files: list[str] = []
    current_path, current_file = open_block_file(output_dir, file_number)
    output_files.append(current_path.name)

    try:
        for height in heights:
            raw_block = bytes.fromhex("".join(exports[height].read_text().split()))
            record_size = 12 + len(raw_block)
            if bytes_in_file > 0 and bytes_in_file + record_size > args.max_file_size:
                current_file.close()
                file_number += 1
                bytes_in_file = 0
                current_path, current_file = open_block_file(output_dir, file_number)
                output_files.append(current_path.name)

            written = write_record(current_file, magic, raw_block)
            bytes_in_file += written
            total_bytes += written
            packed_records += 1
    finally:
        current_file.close()

    manifest = {
        "network": args.network,
        "magic": f"0x{magic:08x}",
        "input_dir": str(input_dir),
        "output_dir": str(output_dir),
        "start_height": selected_start,
        "end_height": selected_end,
        "record_count": packed_records,
        "missing_count": len(missing),
        "first_missing_heights": missing[:100],
        "output_files": output_files,
        "total_bytes": total_bytes,
        "max_file_size": args.max_file_size,
    }

    if args.manifest:
        Path(args.manifest).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    print(
        "packed records={record_count} heights={start_height}-{end_height} files={files} bytes={bytes}".format(
            record_count=manifest["record_count"],
            start_height=manifest["start_height"],
            end_height=manifest["end_height"],
            files=len(output_files),
            bytes=manifest["total_bytes"],
        ),
        flush=True,
    )
    if missing:
        print(f"missing_count={len(missing)} first_missing={missing[:20]}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
