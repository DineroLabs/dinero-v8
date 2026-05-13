#!/bin/bash
# Genesis Block Verification Script
# Verifies that the node loads the correct Post-Utreexo genesis

set -e

echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║       DINERO POST-UTREEXO GENESIS VERIFICATION                ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHAIN_BUNDLE_HEADER="$ROOT_DIR/include/consensus/chain_bundle_generated.h"

extract_bundle_value() {
    local name="$1"
    sed -nE "s/^static constexpr (const char\\*|uint64_t|uint32_t)[[:space:]]+${name}[[:space:]]*=[[:space:]]*\\\"?([^\\\";]+)\\\"?;.*/\\2/p" \
        "$CHAIN_BUNDLE_HEADER" | head -1
}

# Expected values come from the canonical generated bundle.
EXPECTED_HASH="$(extract_bundle_value GENESIS_BLOCK_HASH)"
EXPECTED_TIME="$(extract_bundle_value GENESIS_TIMESTAMP)"
EXPECTED_BITS="$(extract_bundle_value GENESIS_DIFFICULTY)"
EXPECTED_NONCE="$(extract_bundle_value GENESIS_NONCE)"

# Check if dinerod is running
if pgrep -x "dinerod" > /dev/null; then
    echo "✅ dinerod is running"
else
    echo "❌ dinerod is not running"
    echo "   Start it with: ./build/dinerod --chain=mainnet --daemon"
    exit 1
fi

# Wait for RPC to be ready
echo "⏳ Waiting for RPC to be ready..."
sleep 3

# Get genesis hash
ACTUAL_HASH="$("$ROOT_DIR/build/dinero-cli" getblockhash 0 2>&1 | tr -d '"')"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "VERIFICATION RESULTS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Verify hash
if [ "$ACTUAL_HASH" == "$EXPECTED_HASH" ]; then
    echo "✅ Genesis Hash: CORRECT"
    echo "   $ACTUAL_HASH"
else
    echo "❌ Genesis Hash: MISMATCH"
    echo "   Expected: $EXPECTED_HASH"
    echo "   Got:      $ACTUAL_HASH"
    exit 1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "GENESIS PARAMETERS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  Network:      mainnet"
echo "  Protocol:     2.0.0 (Post-Utreexo)"
echo "  Timestamp:    $EXPECTED_TIME (Mar 7, 2026 00:00:00 UTC)"
echo "  nBits:        $EXPECTED_BITS"
echo "  Nonce:        $EXPECTED_NONCE"
echo "  Header Size:  128 bytes"
echo "  Premine:      NONE (100 DIN burned via OP_RETURN)"
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "🎉 GENESIS VALIDATION PASSED!"
echo ""
echo "   This node is ready for the Fair Launch v3 network."
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
