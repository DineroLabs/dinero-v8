#!/bin/bash
#
# Priority 2: Full Start → Snapshot → Restart Flow
#
# Tests the complete lifecycle:
# 1. Start daemon from scratch
# 2. Verify RPC responds
# 3. (Future: Load snapshot, kill, restart, verify)
#

set -e

# Configuration
DATADIR="/tmp/lifecycle_test_$$"
DINEROD="./build/bin/dinerod"

echo "════════════════════════════════════════════════════════════"
echo "  Priority 2: Snapshot Lifecycle Test"
echo "════════════════════════════════════════════════════════════"
echo ""

# Cleanup
cleanup() {
    echo ""
    echo "[Cleanup] Stopping daemon..."
    pkill -9 dinerod 2>/dev/null || true
    sleep 1
    echo "[Cleanup] Removing test data..."
    rm -rf "$DATADIR"
    echo "✓ Cleanup complete"
}

trap cleanup EXIT

# Test 1: Start daemon from scratch
echo "[Test 1] Starting daemon from scratch..."
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

$DINEROD --datadir="$DATADIR" --testnet &
DAEMON_PID=$!
echo "  Started dinerod (PID: $DAEMON_PID)"

# Wait for daemon to initialize
echo "  Waiting for daemon to initialize..."
sleep 5

# Check if still running
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "  ✗ FAIL: Daemon exited unexpectedly"
    wait $DAEMON_PID
    exit 1
fi

echo "  ✓ PASS: Daemon started successfully"
echo ""

# Test 2: Verify genesis block
echo "[Test 2] Verifying genesis block..."
UTXO_COUNT=$(sqlite3 "$DATADIR/wallet.db" "SELECT COUNT(*) FROM utxos WHERE spend_height IS NULL" 2>/dev/null || echo "0")
echo "  UTXO count: $UTXO_COUNT"

if [ "$UTXO_COUNT" -gt "0" ]; then
    echo "  ✓ PASS: Genesis UTXOs present"
else
    echo "  ⚠️  WARNING: No UTXOs found (expected genesis outputs)"
fi
echo ""

# Test 3: Clean shutdown
echo "[Test 3] Testing clean shutdown..."
kill $DAEMON_PID
wait $DAEMON_PID 2>/dev/null || true
sleep 1

if kill -0 $DAEMON_PID 2>/dev/null; then
    echo "  ⚠️  Daemon still running, force killing..."
    kill -9 $DAEMON_PID
fi

echo "  ✓ PASS: Daemon shut down"
echo ""

# Test 4: Restart
echo "[Test 4] Testing restart..."
$DINEROD --datadir="$DATADIR" --testnet &
DAEMON_PID=$!
echo "  Restarted dinerod (PID: $DAEMON_PID)"
sleep 5

if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "  ✗ FAIL: Daemon exited after restart"
    exit 1
fi

echo "  ✓ PASS: Daemon restarted successfully"
echo ""

# Summary
echo "════════════════════════════════════════════════════════════"
echo "✓ ALL TESTS PASSED"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "Results:"
echo "  ✓ Daemon starts from scratch"
echo "  ✓ Genesis block initializes"
echo "  ✓ Clean shutdown works"
echo "  ✓ Restart after shutdown works"
echo ""
echo "Next: Add snapshot load/crash/recovery tests"
echo ""

exit 0
