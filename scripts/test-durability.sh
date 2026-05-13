#!/usr/bin/env bash
# Kill-9 durability test (WAL + sync really working)
set -euo pipefail

BIN="${1:-./build-test/bin/test_sqlite_wallet}"
DB="/tmp/test-wallet.sqlite"

echo "🧪 Kill-9 Durability Test"
echo "Binary: $BIN"
echo "Database: $DB"

# Clean slate
rm -f "$DB"*

echo "📋 Step 1: Start applying blocks in background..."
("$BIN" --start-applying-blocks & echo $! > /tmp/wpid) &

# Give it time to start and apply some blocks
sleep 0.5

if [ -f /tmp/wpid ] && ps -p "$(cat /tmp/wpid)" >/dev/null 2>&1; then
    echo "📋 Step 2: Kill -9 the process mid-operation..."
    kill -9 "$(cat /tmp/wpid)" 2>/dev/null || true
    sleep 0.2
else
    echo "❌ Process not found or already exited"
    exit 1
fi

echo "📋 Step 3: Attempt recovery..."
if "$BIN" --recover; then
    echo "✅ Recovery successful"
else
    echo "❌ Recovery failed"
    exit 1
fi

echo "📋 Step 4: Check database invariants..."
if "$BIN" --check-invariants; then
    echo "✅ Invariants check passed"
else
    echo "❌ Invariants check failed"
    exit 1
fi

echo "✅ Kill-9 durability test PASSED"
echo "🎉 WAL + synchronous mode working correctly"

# Cleanup
rm -f /tmp/wpid "$DB"*
