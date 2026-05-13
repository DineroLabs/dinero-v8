#!/bin/bash
# Quick diagnostic for Utreexo proof issues
# Run from your daemon server

echo "=== Utreexo Proof Diagnostic ==="
echo ""

# Get current height
HEIGHT=$(dinero-cli getblockcount 2>/dev/null || echo "RPC unavailable")
echo "Current height: $HEIGHT"

# Get block at height 1
echo ""
echo "=== Block 1 (Premine) ==="
BLOCK1=$(dinero-cli getblock $(dinero-cli getblockhash 1) 2 2>/dev/null)
if [ -n "$BLOCK1" ]; then
    echo "$BLOCK1" | python3 -c "
import json, sys
block = json.load(sys.stdin)
print(f'Hash: {block.get(\"hash\", \"?\")[:16]}...')
print(f'Height: {block.get(\"height\", \"?\")}')
print(f'Transactions: {len(block.get(\"tx\", []))}')
for i, tx in enumerate(block.get('tx', [])):
    if isinstance(tx, dict):
        vins = tx.get('vin', [])
        is_coinbase = vins and 'coinbase' in vins[0]
        print(f'  TX[{i}]: inputs={len(vins)} outputs={len(tx.get(\"vout\",[]))} {\"[COINBASE]\" if is_coinbase else \"\"}')
        if not is_coinbase:
            for vin in vins:
                print(f'    SPENDS: {vin.get(\"txid\",\"?\")[:16]}...:{vin.get(\"vout\",\"?\")}')
"
else
    echo "Could not fetch block 1"
fi

# Check block 2 (first block that might have spends)
echo ""
echo "=== Block 2 ==="
BLOCK2=$(dinero-cli getblock $(dinero-cli getblockhash 2) 2 2>/dev/null)
if [ -n "$BLOCK2" ]; then
    echo "$BLOCK2" | python3 -c "
import json, sys
block = json.load(sys.stdin)
print(f'Hash: {block.get(\"hash\", \"?\")[:16]}...')
print(f'Height: {block.get(\"height\", \"?\")}')
print(f'Transactions: {len(block.get(\"tx\", []))}')
spent_count = 0
for i, tx in enumerate(block.get('tx', [])):
    if isinstance(tx, dict):
        vins = tx.get('vin', [])
        is_coinbase = vins and 'coinbase' in vins[0]
        print(f'  TX[{i}]: inputs={len(vins)} outputs={len(tx.get(\"vout\",[]))} {\"[COINBASE]\" if is_coinbase else \"\"}')
        if not is_coinbase:
            spent_count += len(vins)
print(f'Total spent outpoints: {spent_count}')
"
else
    echo "Block 2 not yet mined"
fi

echo ""
echo "=== Summary ==="
echo "Block 1 (premine) should have 0 spent outpoints (coinbase only)"
echo "Block 2+ can have spending transactions"
echo ""
echo "If iOS fails on block 1 with 'Parsed 0 spent UTXOs', that's EXPECTED."
echo "The commitment mismatch is likely from a different issue (adds not matching)."
