#!/usr/bin/env python3
"""
DineroCoin Regtest Crypto CLI

Command-line interface for generating test vectors and crypto operations.

Usage:
    python -m tools.regtest_crypto.cli --help
    python -m tools.regtest_crypto.cli mine-headers --count 4 --network regtest
    python -m tools.regtest_crypto.cli generate-all --output tests/vectors
"""

import argparse
import sys
import json
from pathlib import Path

from .params import get_network, NETWORKS
from .pow import (
    DineroHeader, mine_header, mine_header_chain,
    bits_to_target, bits_to_difficulty, check_pow
)
from .merkle import merkle_root_hex
from .vectors import (
    generate_header_vectors, generate_merkle_vectors,
    generate_tx_vectors, generate_signature_vectors,
    generate_all_vectors, write_vectors
)

def cmd_mine_headers(args):
    """Mine a chain of valid headers."""
    print(f"Mining {args.count} headers for {args.network}...")
    print()

    params = get_network(args.network)

    vectors = generate_header_vectors(
        count=args.count,
        network=args.network,
        base_timestamp=args.timestamp,
    )

    if args.output:
        write_vectors(args.output, vectors)
        print(f"\nWritten to: {args.output}")

    if args.cpp:
        print("\n" + "=" * 60)
        print("C++ Code Snippet:")
        print("=" * 60)
        print(vectors["cpp_snippet"])

    if args.json:
        print("\n" + "=" * 60)
        print("JSON Output:")
        print("=" * 60)
        # Print without cpp_snippet for cleaner JSON
        output = {k: v for k, v in vectors.items() if k != "cpp_snippet"}
        print(json.dumps(output, indent=2))

def cmd_mine_single(args):
    """Mine a single header from hex input."""
    header_bytes = bytes.fromhex(args.header_hex)

    if len(header_bytes) != 128:
        print(f"Error: Header must be 128 bytes, got {len(header_bytes)}")
        sys.exit(1)

    header = DineroHeader.deserialize(header_bytes)

    if args.bits:
        header.bits = args.bits

    print(f"Mining header with bits=0x{header.bits:08x}...")

    nonce, hash_hex = mine_header(header, verbose=True)

    print()
    print(f"Nonce: {nonce}")
    print(f"Hash:  {hash_hex}")

    if args.output:
        result = {
            "input_hex": args.header_hex,
            "bits": f"0x{header.bits:08x}",
            "nonce": nonce,
            "hash": hash_hex,
        }
        with open(args.output, 'w') as f:
            json.dump(result, f, indent=2)
        print(f"\nWritten to: {args.output}")

def cmd_bits_info(args):
    """Show information about difficulty bits."""
    bits = args.bits

    target = bits_to_target(bits)
    difficulty = bits_to_difficulty(bits)

    exponent = bits >> 24
    mantissa = bits & 0x00FFFFFF

    print(f"Bits:       0x{bits:08x}")
    print(f"Exponent:   {exponent} (0x{exponent:02x})")
    print(f"Mantissa:   {mantissa} (0x{mantissa:06x})")
    print(f"Target:     0x{target:064x}")
    print(f"Difficulty: {difficulty:.6f}")
    print()

    # Show comparison with known values
    print("Comparison:")
    for name, net in NETWORKS.items():
        net_diff = bits_to_difficulty(net.bits)
        ratio = difficulty / net_diff if net_diff > 0 else float('inf')
        print(f"  vs {name} (0x{net.bits:08x}): {ratio:.2f}x")

def cmd_merkle(args):
    """Compute merkle root from txids."""
    if not args.txids:
        print("Error: At least one txid required")
        sys.exit(1)

    root = merkle_root_hex(args.txids, from_display=True)
    print(f"Merkle root: {root}")

    if len(args.txids) == 1:
        print("(Single tx: root == txid)")

def cmd_hash(args):
    """Compute double SHA-256 hash."""
    from .pow import double_sha256

    data = bytes.fromhex(args.hex_data)
    hash_bytes = double_sha256(data)

    print(f"Input:   {args.hex_data}")
    print(f"Hash:    {hash_bytes.hex()}")
    print(f"Display: {hash_bytes[::-1].hex()}")

def cmd_generate_all(args):
    """Generate all test vector files."""
    generate_all_vectors(
        output_dir=args.output,
        network=args.network,
    )

def cmd_verify_pow(args):
    """Verify PoW for a header."""
    header_bytes = bytes.fromhex(args.header_hex)

    if len(header_bytes) != 128:
        print(f"Error: Header must be 128 bytes, got {len(header_bytes)}")
        sys.exit(1)

    header = DineroHeader.deserialize(header_bytes)

    print(f"Version:    {header.version}")
    print(f"Prev hash:  {header.prev_block_hash[::-1].hex()[:16]}...")
    print(f"Merkle:     {header.merkle_root[::-1].hex()[:16]}...")
    print(f"Timestamp:  {header.timestamp}")
    print(f"Bits:       0x{header.bits:08x}")
    print(f"Nonce:      {header.nonce}")
    print(f"Utreexo:    {header.utreexo_root[::-1].hex()[:16]}...")
    print(f"Hash:       {header.get_hash_hex()}")
    print()

    valid = check_pow(header, require_standard=args.require_standard)
    print(f"PoW valid:  {valid}")

    if not valid:
        target = bits_to_target(header.bits)
        hash_int = int.from_bytes(header.get_hash(), 'little')
        print(f"  Target: 0x{target:064x}")
        print(f"  Hash:   0x{hash_int:064x}")
        print(f"  Hash > Target (invalid)")

def main():
    parser = argparse.ArgumentParser(
        description="DineroCoin Regtest Crypto CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  Mine 4 regtest headers:
    python -m tools.regtest_crypto.cli mine-headers --count 4 --cpp

  Get difficulty info:
    python -m tools.regtest_crypto.cli bits 0x207fffff

  Compute merkle root:
    python -m tools.regtest_crypto.cli merkle txid1 txid2 txid3

  Generate all test vectors:
    python -m tools.regtest_crypto.cli generate-all --output tests/vectors
""")

    subparsers = parser.add_subparsers(dest="command", help="Command")

    # mine-headers
    p_mine = subparsers.add_parser("mine-headers", help="Mine a chain of headers")
    p_mine.add_argument("-n", "--count", type=int, default=4, help="Number of headers")
    p_mine.add_argument("--network", default="regtest", choices=NETWORKS.keys())
    p_mine.add_argument("--timestamp", type=int, default=1_000_000, help="Base timestamp")
    p_mine.add_argument("-o", "--output", help="Output JSON file")
    p_mine.add_argument("--cpp", action="store_true", help="Print C++ snippet")
    p_mine.add_argument("--json", action="store_true", help="Print JSON output")
    p_mine.set_defaults(func=cmd_mine_headers)

    # mine-single
    p_single = subparsers.add_parser("mine-single", help="Mine a single header")
    p_single.add_argument("header_hex", help="128-byte header as hex")
    p_single.add_argument("--bits", type=lambda x: int(x, 0), help="Override difficulty bits")
    p_single.add_argument("-o", "--output", help="Output JSON file")
    p_single.set_defaults(func=cmd_mine_single)

    # bits
    p_bits = subparsers.add_parser("bits", help="Show difficulty bits info")
    p_bits.add_argument("bits", type=lambda x: int(x, 0), help="Bits value (hex ok)")
    p_bits.set_defaults(func=cmd_bits_info)

    # merkle
    p_merkle = subparsers.add_parser("merkle", help="Compute merkle root")
    p_merkle.add_argument("txids", nargs="+", help="Transaction IDs (display format)")
    p_merkle.set_defaults(func=cmd_merkle)

    # hash
    p_hash = subparsers.add_parser("hash", help="Compute double SHA-256")
    p_hash.add_argument("hex_data", help="Hex data to hash")
    p_hash.set_defaults(func=cmd_hash)

    # verify-pow
    p_verify = subparsers.add_parser("verify-pow", help="Verify header PoW")
    p_verify.add_argument("header_hex", help="128-byte header as hex")
    p_verify.add_argument("--require-standard", action="store_true",
                          help="Require standard difficulty")
    p_verify.set_defaults(func=cmd_verify_pow)

    # generate-all
    p_gen = subparsers.add_parser("generate-all", help="Generate all test vectors")
    p_gen.add_argument("-o", "--output", default="tests/vectors", help="Output directory")
    p_gen.add_argument("--network", default="regtest", choices=NETWORKS.keys())
    p_gen.set_defaults(func=cmd_generate_all)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    args.func(args)

if __name__ == "__main__":
    main()
