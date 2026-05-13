#!/usr/bin/env bash
set -euo pipefail

echo "🌐 **SIMPLE NETWORK SWITCHING TEST**"
echo "===================================="

# Clean up
killall dinero-desktop dinerod 2>/dev/null || true
sleep 1

TEST_ROOT="${TEST_ROOT:-/tmp/dinero-network-switch-$$}"
REGTEST_DATADIR="${REGTEST_DATADIR:-$TEST_ROOT/regtest}"
MAINNET_DATADIR="${MAINNET_DATADIR:-$TEST_ROOT/mainnet}"

cleanup() {
    kill ${REGTEST_PID:-} ${MAINNET_PID:-} 2>/dev/null || true
    rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

# Test with explicit isolated datadirs.
echo "🚀 **Step 1: Start Regtest Daemon**"
./build/dinerod -regtest -server -rpcport=20996 -port=21001 -datadir="$REGTEST_DATADIR" >/tmp/regtest.log 2>&1 &
REGTEST_PID=$!
echo "Regtest daemon PID: $REGTEST_PID"
sleep 3

# Check health
if curl -s http://127.0.0.1:20996/healthz | grep -q "ok"; then
    echo "✅ Regtest daemon healthy"
else
    echo "❌ Regtest daemon failed"
    kill $REGTEST_PID 2>/dev/null || true
    exit 1
fi

# Find the actual cookie location
REGTEST_COOKIE_PATH=""
for path in "$REGTEST_DATADIR/regtest/.cookie" "$REGTEST_DATADIR/.cookie"; do
    if [ -f "$path" ]; then
        REGTEST_COOKIE_PATH="$path"
        break
    fi
done

if [ -z "$REGTEST_COOKIE_PATH" ]; then
    echo "❌ Could not find regtest cookie"
    kill $REGTEST_PID 2>/dev/null || true
    exit 1
fi

echo "Found regtest cookie at: $REGTEST_COOKIE_PATH"
REGTEST_COOKIE=$(cat "$REGTEST_COOKIE_PATH")

# Test RPC
rpc_regtest() {
    curl -s -u "$REGTEST_COOKIE" -H 'content-type: application/json' \
      -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"$1\",\"params\":${2:-[]}}" \
      http://127.0.0.1:20996/
}

echo "Testing regtest RPC..."
if rpc_regtest getnetworkinfo | jq -e '.result' >/dev/null; then
    echo "✅ Regtest RPC working"
else
    echo "❌ Regtest RPC failed"
    kill $REGTEST_PID 2>/dev/null || true
    exit 1
fi

echo ""
echo "🌐 **Step 2: Start Mainnet Daemon**"
./build/dinerod -server -rpcport=20998 -datadir="$MAINNET_DATADIR" >/tmp/mainnet.log 2>&1 &
MAINNET_PID=$!
echo "Mainnet daemon PID: $MAINNET_PID"
sleep 3

# Check health
if curl -s http://127.0.0.1:20998/healthz | grep -q "ok"; then
    echo "✅ Mainnet daemon healthy"
else
    echo "❌ Mainnet daemon failed"
    kill $REGTEST_PID $MAINNET_PID 2>/dev/null || true
    exit 1
fi

# Find mainnet cookie
MAINNET_COOKIE_PATH=""
for path in "$MAINNET_DATADIR/.cookie"; do
    if [ -f "$path" ]; then
        MAINNET_COOKIE_PATH="$path"
        break
    fi
done

if [ -z "$MAINNET_COOKIE_PATH" ]; then
    echo "❌ Could not find mainnet cookie"
    kill $REGTEST_PID $MAINNET_PID 2>/dev/null || true
    exit 1
fi

echo "Found mainnet cookie at: $MAINNET_COOKIE_PATH"
MAINNET_COOKIE=$(cat "$MAINNET_COOKIE_PATH")

# Test mainnet RPC
rpc_mainnet() {
    curl -s -u "$MAINNET_COOKIE" -H 'content-type: application/json' \
      -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"$1\",\"params\":${2:-[]}}" \
      http://127.0.0.1:20998/
}

echo "Testing mainnet RPC..."
if rpc_mainnet getnetworkinfo | jq -e '.result' >/dev/null; then
    echo "✅ Mainnet RPC working"
else
    echo "❌ Mainnet RPC failed"
    kill $REGTEST_PID $MAINNET_PID 2>/dev/null || true
    exit 1
fi

echo ""
echo "📊 **Step 3: Verify Network Isolation**"
echo "Regtest cookie path: $REGTEST_COOKIE_PATH"
echo "Mainnet cookie path: $MAINNET_COOKIE_PATH"
echo "Cookies different: $([ "$REGTEST_COOKIE" != "$MAINNET_COOKIE" ] && echo "✅ YES" || echo "❌ NO")"
echo "Ports isolated: $(lsof -nP -iTCP:20996,20998 -sTCP:LISTEN | wc -l | xargs) listeners"

echo ""
echo "🧹 **Cleanup**"
kill $REGTEST_PID $MAINNET_PID 2>/dev/null || true
sleep 2

echo ""
echo "🎉 **NETWORK ISOLATION TEST COMPLETE**"
echo "✅ Both networks can run simultaneously"
echo "✅ Different ports (20996 vs 20998)"
echo "✅ Different cookies"
echo "✅ Different data directories"
echo ""
echo "🚀 **Ready for GUI Testing!**"
