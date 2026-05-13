#!/bin/bash
#
# Simple Adversarial Test: Single Bad Snapshot
# Manual test to verify error handling
#

set -e

DATADIR="/tmp/adv_single_$$"
SNAPSHOT="/tmp/bad_snapshot_$$.dat"

echo "═══════════════════════════════════════════════════════════════════════"
echo "  Single Adversarial Snapshot Test"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Cleanup
cleanup() {
    echo ""
    echo "[Cleanup]"
    pkill -9 dinerod 2>/dev/null || true
    sleep 1
    rm -rf "$DATADIR" "$SNAPSHOT"
}

trap cleanup EXIT

# Create bad snapshot (bad magic number)
echo "[1] Creating malicious snapshot (bad magic number)..."
printf '\x00\x00\x00\x00' > "$SNAPSHOT"  # Wrong magic
printf '\x01\x00\x00\x00' >> "$SNAPSHOT"  # Version: 1
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT"  # Block hash
printf '\x00\x00\x00\x00' >> "$SNAPSHOT"  # Height: 0
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT"  # UTXO count: 0
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT"  # Timestamp
printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$SNAPSHOT"  # Reserved
dd if=/dev/zero bs=32 count=1 2>/dev/null >> "$SNAPSHOT"  # Checksum

echo "  ✓ Created: $SNAPSHOT ($(stat -f%z "$SNAPSHOT") bytes)"
echo ""

# Start daemon
echo "[2] Starting daemon..."
mkdir -p "$DATADIR"
./build/bin/dinerod --datadir="$DATADIR" --testnet > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
echo "  Started PID: $DAEMON_PID"
echo "  Waiting for initialization..."

# Wait for RPC cookie to be created (up to 30 seconds)
WAITED=0
while [ $WAITED -lt 30 ]; do
    if [ -f "$DATADIR/.cookie" ]; then
        echo "  ✓ RPC cookie created after ${WAITED}s"
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done

if [ ! -f "$DATADIR/.cookie" ]; then
    echo "  ✗ RPC cookie not created after ${WAITED}s"
    echo "  Daemon log:"
    tail -20 "$DATADIR/daemon.log"
    exit 1
fi

if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "  ✗ Daemon died!"
    tail -20 "$DATADIR/daemon.log"
    exit 1
fi

echo "  ✓ Daemon running and RPC ready"
echo ""

# Get initial UTXO count
echo "[3] Checking initial state..."
UTXO_BEFORE=$(sqlite3 "$DATADIR/wallet.db" "SELECT COUNT(*) FROM utxos WHERE spend_height IS NULL" 2>/dev/null || echo "0")
echo "  UTXO count: $UTXO_BEFORE"
echo ""

# Try to load bad snapshot
echo "[4] Attempting to load malicious snapshot..."
echo "  Command: dinero-cli loadtxoutset $SNAPSHOT"
echo ""
echo "  Response:"
RESPONSE=$(./build/bin/dinero-cli -datadir="$DATADIR" loadtxoutset "$SNAPSHOT" 2>&1 || true)
echo "$RESPONSE"
echo ""

# Check if snapshot was rejected
if echo "$RESPONSE" | grep -qi "error\|invalid\|failed\|bad"; then
    echo "  ✓ Snapshot was rejected (error detected in response)"
elif [ -z "$RESPONSE" ]; then
    echo "  ⚠️  Empty response (RPC might have failed to connect)"
else
    echo "  ⚠️  Response did not contain error (unexpected)"
fi
echo ""

# Check final state
echo "[5] Checking final state..."
UTXO_AFTER=$(sqlite3 "$DATADIR/wallet.db" "SELECT COUNT(*) FROM utxos WHERE spend_height IS NULL" 2>/dev/null || echo "0")
echo "  UTXO count: $UTXO_AFTER"
echo ""

# Verify
if [ "$UTXO_BEFORE" -eq "$UTXO_AFTER" ]; then
    echo "✓ SUCCESS: UTXO count unchanged ($UTXO_BEFORE → $UTXO_AFTER)"
    echo "✓ No state corruption detected"
else
    echo "✗ CRITICAL: UTXO count changed! ($UTXO_BEFORE → $UTXO_AFTER)"
    echo "✗ State was corrupted!"
    exit 1
fi
