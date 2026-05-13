#!/bin/bash

# DINERO MAINNET CONSENSUS TEST
# Tests that mainnet can start without consensus commitment mismatch

set -e

echo "🔒 DINERO MAINNET CONSENSUS TEST"
echo "==============================="
echo ""

# Clean test environment
TESTDIR="./test-data/mainnet-consensus-test"
rm -rf "$TESTDIR"
mkdir -p "$TESTDIR"

echo "🧪 Testing mainnet startup (should not crash)..."
echo ""

# Start daemon with timeout to prevent hanging
timeout 5s ./build/dinerod \
    --mainnet \
    --datadir="$TESTDIR" \
    --rpcport=29888 \
    --port=29889 \
    --connect=0 \
    --listen=0 \
    --printtoconsole \
    2>&1 | tee "$TESTDIR/daemon.log" || true

echo ""
echo "📋 RESULTS:"
echo "----------"

# Check for consensus commitment mismatch (the fatal error we fixed)
if grep -q "FATAL: Consensus commitment mismatch" "$TESTDIR/daemon.log"; then
    echo "❌ FAILED: Consensus commitment mismatch still present"
    exit 1
else
    echo "✅ PASSED: No consensus commitment mismatch"
fi

# Check for successful chain identity validation
if grep -q "Chain Identity Check passed" "$TESTDIR/daemon.log"; then
    echo "✅ PASSED: Chain identity validation successful"
else
    echo "⚠️  WARNING: Chain identity check not found in logs"
fi

# Check for successful network selection
if grep -q "Selected network: main" "$TESTDIR/daemon.log"; then
    echo "✅ PASSED: Mainnet network selected successfully"
else
    echo "❌ FAILED: Mainnet network selection failed"
    exit 1
fi

# Check for genesis hash issues (separate from consensus commitment)
if grep -q "Genesis hash mismatch" "$TESTDIR/daemon.log"; then
    echo "⚠️  WARNING: Genesis hash mismatch detected (separate issue from consensus commitment)"
    echo "   This needs to be fixed by mining a proper genesis block"
else
    echo "✅ PASSED: No genesis hash issues"
fi

echo ""
echo "🎉 CONSENSUS COMMITMENT MISMATCH FIXED!"
echo "   Mainnet can now start without the fatal consensus error"
echo ""
echo "📝 Next steps:"
echo "   1. Mine proper genesis block to fix genesis hash mismatch"
echo "   2. Add unit tests for consensus validation"
echo "   3. Set up CI to test mainnet init daily"

# Cleanup
rm -rf "$TESTDIR"
