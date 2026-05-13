#!/usr/bin/env python3
"""
Script #3: Proof Sanity Checker
Answers: What does the proof blob actually contain?

Binary format (BlockUtreexoData::serialize):
1. accumulator_root_before (32 bytes)
2. spend_proof:
   - num_targets (4 bytes LE)
   - targets (32 bytes each)
   - num_proof_hashes (4 bytes LE)
   - proof_hashes (32 bytes each)
3. spent_outputs_count (4 bytes LE)
4. spent_outputs (for each):
   - value (8 bytes LE)
   - spk_len (4 bytes LE)
   - scriptPubKey (spk_len bytes)

Usage:
    python3 inspect_proof.py proof.bin
    python3 inspect_proof.py --hex <hex_string>
"""

import struct
import sys

def parse_proof(data: bytes) -> dict:
    """Parse BlockUtreexoData binary format"""
    result = {
        "total_bytes": len(data),
        "accumulator_root_before": None,
        "num_targets": 0,
        "targets": [],
        "num_proof_hashes": 0,
        "proof_hashes": [],
        "spent_outputs_count": 0,
        "spent_outputs": [],
        "parse_errors": []
    }

    offset = 0

    # 1. accumulator_root_before (32 bytes)
    if len(data) < 32:
        result["parse_errors"].append(f"Too short for root: {len(data)} bytes")
        return result

    result["accumulator_root_before"] = data[offset:offset+32].hex()
    offset += 32

    # 2a. num_targets (4 bytes LE)
    if len(data) < offset + 4:
        result["parse_errors"].append(f"Too short for num_targets at offset {offset}")
        return result

    result["num_targets"] = struct.unpack("<I", data[offset:offset+4])[0]
    offset += 4

    # 2b. targets (32 bytes each)
    for i in range(result["num_targets"]):
        if len(data) < offset + 32:
            result["parse_errors"].append(f"Too short for target {i} at offset {offset}")
            return result
        result["targets"].append(data[offset:offset+32].hex())
        offset += 32

    # 2c. num_proof_hashes (4 bytes LE)
    if len(data) < offset + 4:
        result["parse_errors"].append(f"Too short for num_proof_hashes at offset {offset}")
        return result

    result["num_proof_hashes"] = struct.unpack("<I", data[offset:offset+4])[0]
    offset += 4

    # 2d. proof_hashes (32 bytes each)
    for i in range(result["num_proof_hashes"]):
        if len(data) < offset + 32:
            result["parse_errors"].append(f"Too short for proof_hash {i} at offset {offset}")
            return result
        result["proof_hashes"].append(data[offset:offset+32].hex())
        offset += 32

    # 3. spent_outputs_count (4 bytes LE)
    if len(data) < offset + 4:
        result["parse_errors"].append(f"Too short for spent_outputs_count at offset {offset}")
        return result

    result["spent_outputs_count"] = struct.unpack("<I", data[offset:offset+4])[0]
    offset += 4

    # 4. spent_outputs
    for i in range(result["spent_outputs_count"]):
        spent = {}

        # 4a. value (8 bytes LE)
        if len(data) < offset + 8:
            result["parse_errors"].append(f"Too short for value at spent output {i}")
            return result
        spent["value"] = struct.unpack("<Q", data[offset:offset+8])[0]
        offset += 8

        # 4b. spk_len (4 bytes LE)
        if len(data) < offset + 4:
            result["parse_errors"].append(f"Too short for spk_len at spent output {i}")
            return result
        spk_len = struct.unpack("<I", data[offset:offset+4])[0]
        spent["spk_len"] = spk_len
        offset += 4

        # 4c. scriptPubKey
        if len(data) < offset + spk_len:
            result["parse_errors"].append(f"Too short for scriptPubKey at spent output {i}")
            return result
        spent["script_pubkey"] = data[offset:offset+spk_len].hex()
        offset += spk_len

        result["spent_outputs"].append(spent)

    result["bytes_consumed"] = offset
    result["bytes_remaining"] = len(data) - offset

    return result

def print_proof(result: dict):
    """Pretty print proof analysis"""
    print("=" * 60)
    print("UTREEXO PROOF ANALYSIS")
    print("=" * 60)
    print(f"Total bytes: {result['total_bytes']}")
    print(f"Bytes consumed: {result.get('bytes_consumed', 'N/A')}")
    print(f"Bytes remaining: {result.get('bytes_remaining', 'N/A')}")
    print()

    print(f"Accumulator root before: {result['accumulator_root_before'][:32]}..." if result['accumulator_root_before'] else "N/A")
    print()

    print(f"Targets (leaf hashes): {result['num_targets']}")
    for i, t in enumerate(result['targets'][:5]):  # Show first 5
        print(f"  [{i}] {t[:32]}...")
    if len(result['targets']) > 5:
        print(f"  ... and {len(result['targets']) - 5} more")
    print()

    print(f"Proof hashes (siblings): {result['num_proof_hashes']}")
    for i, h in enumerate(result['proof_hashes'][:5]):  # Show first 5
        print(f"  [{i}] {h[:32]}...")
    if len(result['proof_hashes']) > 5:
        print(f"  ... and {len(result['proof_hashes']) - 5} more")
    print()

    print(f"Spent outputs: {result['spent_outputs_count']}")
    for i, s in enumerate(result['spent_outputs'][:5]):  # Show first 5
        print(f"  [{i}] value={s['value']} script={s['script_pubkey'][:20]}...")
    if len(result['spent_outputs']) > 5:
        print(f"  ... and {len(result['spent_outputs']) - 5} more")
    print()

    if result['parse_errors']:
        print("PARSE ERRORS:")
        for err in result['parse_errors']:
            print(f"  ❌ {err}")
    print()

    # VERDICT
    print("=" * 60)
    print("VERDICT")
    print("=" * 60)

    if result['spent_outputs_count'] == 0:
        print("❌ PROOF HAS ZERO SPENT OUTPUTS!")
        print()
        print("This means one of:")
        print("  1. Block is coinbase-only (no spending txs) - CHECK THE BLOCK")
        print("  2. Proof generated AFTER ConnectBlock (UTXOs already spent)")
        print("  3. Proof generator not collecting spent outpoints")
    else:
        print(f"✅ Proof has {result['spent_outputs_count']} spent outputs")

    if result['num_targets'] != result['spent_outputs_count']:
        print(f"⚠️  Mismatch: {result['num_targets']} targets vs {result['spent_outputs_count']} spent outputs")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 inspect_proof.py proof.bin")
        print("       python3 inspect_proof.py --hex <hex_string>")
        sys.exit(1)

    data = None

    if sys.argv[1] == "--hex":
        data = bytes.fromhex(sys.argv[2])
    else:
        with open(sys.argv[1], "rb") as f:
            data = f.read()

    result = parse_proof(data)
    print_proof(result)

    # Write JSON for further analysis
    import json
    # Convert bytes to hex for JSON serialization
    with open("/tmp/proof_parsed.json", "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nWrote details to /tmp/proof_parsed.json")

if __name__ == "__main__":
    main()
