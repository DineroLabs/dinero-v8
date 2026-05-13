#!/usr/bin/env bash
set -euo pipefail

echo "🔧 **CORRECTED NETWORK SWITCHING TEST**"
echo "======================================"

BASE="$HOME/.dinero"
echo "Base directory: $BASE"

# Clean up any existing processes
pkill -f dinerod || true
sleep 1

echo ""
echo "🚀 **Step 1: Test Regtest with Corrected Paths**"
echo "Starting regtest daemon with --datadir=\"$BASE\"..."
echo "(Daemon will create: $BASE/regtest/ automatically)"

cd "$(dirname "$0")/.."
./build/dinerod --regtest \
  --datadir="$BASE" \
  --rpcport=20996 --wsport=18881 \
  --printtoconsole > /tmp/regtest_corrected.log 2>&1 &
REGTEST_PID=$!

sleep 3
echo "Regtest daemon PID: $REGTEST_PID"

# Check if daemon is running
if curl -s http://127.0.0.1:20996/healthz | grep -q "ok"; then
    echo "✅ Regtest daemon healthy"
else
    echo "❌ Regtest daemon failed to start"
    exit 1
fi

# Check where the cookie actually is
EXPECTED_COOKIE="$BASE/regtest/.cookie"
if [ -f "$EXPECTED_COOKIE" ]; then
    echo "✅ Cookie found at expected location: $EXPECTED_COOKIE"
    REGTEST_COOKIE=$(cat "$EXPECTED_COOKIE")
    echo "Cookie content: $REGTEST_COOKIE"
else
    echo "❌ Cookie not found at $EXPECTED_COOKIE"
    echo "Searching for actual cookie location..."
    find "$BASE" -name ".cookie" -type f 2>/dev/null || echo "No cookies found in $BASE"
    exit 1
fi

# Test RPC
echo "Testing regtest RPC..."
if curl -s -u "$REGTEST_COOKIE" -H 'content-type: application/json' \
   -d '{"jsonrpc":"2.0","id":"test","method":"getnetworkinfo","params":[]}' \
   http://127.0.0.1:20996/ | jq -e '.result' >/dev/null; then
    echo "✅ Regtest RPC working"
else
    echo "❌ Regtest RPC failed"
    kill $REGTEST_PID 2>/dev/null || true
    exit 1
fi

echo ""
echo "🌐 **Step 2: Switch to Mainnet (Serial)**"
echo "Stopping regtest daemon..."
kill $REGTEST_PID 2>/dev/null || true
sleep 2

echo "Starting mainnet daemon with --datadir=\"$BASE\"..."
echo "(Daemon will create: $BASE/mainnet/ automatically)"

./build/dinerod \
  --datadir="$BASE" \
  --rpcport=20998 --wsport=21001 \
  --printtoconsole > /tmp/mainnet_corrected.log 2>&1 &
MAINNET_PID=$!

sleep 3
echo "Mainnet daemon PID: $MAINNET_PID"

# Check if mainnet daemon is running
if curl -s http://127.0.0.1:20998/healthz | grep -q "ok"; then
    echo "✅ Mainnet daemon healthy"
else
    echo "❌ Mainnet daemon failed to start"
    echo "Checking logs for errors..."
    head -30 /tmp/mainnet_corrected.log | grep -E "bind|Address already in use|Error|FATAL|incorrect|genesis|datadir|port" || echo "No obvious errors found"
    exit 1
fi

# Check mainnet cookie
EXPECTED_MAINNET_COOKIE="$BASE/.cookie"
if [ -f "$EXPECTED_MAINNET_COOKIE" ]; then
    echo "✅ Mainnet cookie found at expected location: $EXPECTED_MAINNET_COOKIE"
    MAINNET_COOKIE=$(cat "$EXPECTED_MAINNET_COOKIE")
    echo "Cookie content: $MAINNET_COOKIE"
else
    echo "❌ Mainnet cookie not found at $EXPECTED_MAINNET_COOKIE"
    echo "Searching for actual cookie location..."
    find "$BASE" -name ".cookie" -type f 2>/dev/null || echo "No cookies found in $BASE"
    kill $MAINNET_PID 2>/dev/null || true
    exit 1
fi

# Test mainnet RPC
echo "Testing mainnet RPC..."
if curl -s -u "$MAINNET_COOKIE" -H 'content-type: application/json' \
   -d '{"jsonrpc":"2.0","id":"test","method":"getnetworkinfo","params":[]}' \
   http://127.0.0.1:20998/ | jq -e '.result' >/dev/null; then
    echo "✅ Mainnet RPC working"
else
    echo "❌ Mainnet RPC failed"
    kill $MAINNET_PID 2>/dev/null || true
    exit 1
fi

echo ""
echo "📊 **Step 3: Verify Complete Network Isolation**"
echo "Regtest cookie: $REGTEST_COOKIE"
echo "Mainnet cookie: $MAINNET_COOKIE"
echo "Cookies are different: $([ "$REGTEST_COOKIE" != "$MAINNET_COOKIE" ] && echo "✅ YES" || echo "❌ NO")"
echo "Directory structure:"
ls -la "$BASE"/ 2>/dev/null || echo "Base directory not accessible"

echo ""
echo "🧹 **Cleanup**"
kill $MAINNET_PID 2>/dev/null || true
sleep 1

echo ""
echo "🎉 **CORRECTED PATHS TEST COMPLETE**"
echo "=================================="
echo "✅ Regtest: Daemon uses $BASE → creates $BASE/regtest/"
echo "✅ Mainnet: Daemon uses $BASE"
echo "✅ Cookies: Found at expected locations"
echo "✅ RPC: Both networks working correctly"
echo "✅ Serial switching: No port conflicts"
echo ""
echo "🚀 **GUI Paths Now Match Daemon Behavior!**"
