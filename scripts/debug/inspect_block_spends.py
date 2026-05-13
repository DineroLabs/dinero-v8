#!/usr/bin/env python3
"""
Script #1: Spent-Outpoint Extractor
Answers: What are the spent outpoints in this block?

Usage:
    python3 inspect_block_spends.py <block_hash>

Or with JSON file:
    python3 inspect_block_spends.py --file block.json
"""

import json
import sys
import subprocess
import os

def get_block_from_daemon(block_hash: str) -> dict:
    """Fetch block from daemon via RPC"""
    # Try dinero-cli first
    try:
        result = subprocess.run(
            ["dinero-cli", "getblock", block_hash, "2"],  # verbosity=2 for full tx details
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0:
            return json.loads(result.stdout)
    except Exception as e:
        print(f"dinero-cli failed: {e}")

    # Try curl to RPC
    try:
        import requests
        response = requests.post(
            "http://127.0.0.1:9332",
            json={
                "jsonrpc": "1.0",
                "method": "getblock",
                "params": [block_hash, 2]
            },
            auth=("user", "password"),
            timeout=10
        )
        if response.ok:
            return response.json().get("result", {})
    except Exception as e:
        print(f"RPC failed: {e}")

    return None

def extract_spent_outpoints(block: dict) -> list:
    """Extract all spent outpoints from block transactions"""
    spent = []

    txs = block.get("tx", [])
    if not txs:
        print("WARNING: No transactions in block!")
        return spent

    print(f"Block has {len(txs)} transactions")

    for tx_idx, tx in enumerate(txs):
        # Handle both dict format and raw format
        if isinstance(tx, str):
            print(f"  TX {tx_idx}: raw hex (need verbosity=2)")
            continue

        txid = tx.get("txid", tx.get("hash", "unknown"))
        vins = tx.get("vin", [])
        vouts = tx.get("vout", [])

        # Check if coinbase
        is_coinbase = False
        if vins and "coinbase" in vins[0]:
            is_coinbase = True
            print(f"  TX {tx_idx}: COINBASE (creates {len(vouts)} outputs)")
            continue

        print(f"  TX {tx_idx}: {txid[:16]}... ({len(vins)} inputs, {len(vouts)} outputs)")

        for vin in vins:
            prev_txid = vin.get("txid")
            prev_vout = vin.get("vout")
            if prev_txid is not None and prev_vout is not None:
                outpoint = f"{prev_txid}:{prev_vout}"
                spent.append({
                    "txid": prev_txid,
                    "vout": prev_vout,
                    "outpoint": outpoint,
                    "spent_by": txid
                })
                print(f"    SPENDS: {prev_txid[:16]}...:{prev_vout}")

    return spent

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 inspect_block_spends.py <block_hash>")
        print("       python3 inspect_block_spends.py --file block.json")
        sys.exit(1)

    block = None

    if sys.argv[1] == "--file":
        with open(sys.argv[2]) as f:
            block = json.load(f)
    else:
        block_hash = sys.argv[1]
        print(f"Fetching block {block_hash}...")
        block = get_block_from_daemon(block_hash)

    if not block:
        print("ERROR: Could not get block data")
        print("\nTry dumping block manually:")
        print("  dinero-cli getblock <hash> 2 > block.json")
        print("  python3 inspect_block_spends.py --file block.json")
        sys.exit(1)

    print(f"\n{'='*60}")
    print(f"Block: {block.get('hash', 'unknown')}")
    print(f"Height: {block.get('height', 'unknown')}")
    print(f"{'='*60}\n")

    spent = extract_spent_outpoints(block)

    print(f"\n{'='*60}")
    print(f"SUMMARY: {len(spent)} spent outpoints")
    print(f"{'='*60}")

    if spent:
        print("\nSpent outpoints:")
        for s in spent:
            print(f"  {s['outpoint']}")
    else:
        print("\n⚠️  NO SPENT OUTPOINTS!")
        print("This block only has coinbase - no spending transactions.")
        print("This is EXPECTED for early blocks in a new chain.")

    # Write to file for further analysis
    with open("/tmp/spent_outpoints.json", "w") as f:
        json.dump(spent, f, indent=2)
    print(f"\nWrote details to /tmp/spent_outpoints.json")

    return len(spent)

if __name__ == "__main__":
    count = main()
    sys.exit(0 if count >= 0 else 1)
