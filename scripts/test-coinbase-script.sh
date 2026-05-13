#!/bin/bash

# Test script to verify coinbase script format and witness-based mining
# This tests the improvements made to prevent Bech32 decode errors

set -e

echo "🧪 Testing Coinbase Script Format and Witness-Based Mining"
echo "=========================================================="

# Configuration
TEST_DIR="/tmp/test-coinbase-script"
RPC_PORT=20996
COOKIE_PATH="$TEST_DIR/regtest/.cookie"

# Clean up previous test
echo "🧹 Cleaning up previous test..."
rm -rf "$TEST_DIR"
mkdir -p "$TEST_DIR"

# Start daemon
echo "🚀 Starting Dinero daemon in regtest mode..."
cd "$(dirname "$0")/../build-test"
./bin/dinerod -regtest -datadir="$TEST_DIR" -rpcport="$RPC_PORT" -printtoconsole > "$TEST_DIR/daemon.log" 2>&1 &
DAEMON_PID=$!

# Wait for daemon to start
echo "⏳ Waiting for daemon to start..."
sleep 15

# Check if daemon is running
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "❌ Daemon failed to start"
    cat "$TEST_DIR/daemon.log"
    exit 1
fi

echo "✅ Daemon started successfully"

# Wait for initialization
sleep 5

# Check initial block count
echo "📊 Checking initial blockchain state..."
AUTH="$(cat "$COOKIE_PATH")"
INITIAL_HEIGHT=$(curl -s --basic --user "$AUTH" \
    -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
    "http://127.0.0.1:$RPC_PORT/" | jq -r '.result')

echo "   Initial height: $INITIAL_HEIGHT"

# Mine a block
echo "⛏️  Mining a block to test coinbase script format..."
MINING_RESULT=$(curl -s --basic --user "$AUTH" \
    -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":2,"method":"setgenerate","params":[true,1]}' \
    "http://127.0.0.1:$RPC_PORT/")

echo "   Mining result: $MINING_RESULT"

# Wait for mining to complete
echo "⏳ Waiting for mining to complete..."
sleep 30

# Check new block count
NEW_HEIGHT=$(curl -s --basic --user "$AUTH" \
    -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":3,"method":"getblockcount","params":[]}' \
    "http://127.0.0.1:$RPC_PORT/" | jq -r '.result')

echo "   New height: $NEW_HEIGHT"

if [ "$NEW_HEIGHT" -le "$INITIAL_HEIGHT" ]; then
    echo "❌ Block height did not increase - mining may have failed"
    cat "$TEST_DIR/daemon.log" | tail -50
    exit 1
fi

echo "✅ Block height increased successfully"

# Get the latest block and examine coinbase script
echo "🔍 Examining coinbase script format..."
LATEST_BLOCK=$(curl -s --basic --user "$AUTH" \
    -H 'content-type: application/json' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"getblock\",\"params\":[\"$NEW_HEIGHT\"]}" \
    "http://127.0.0.1:$RPC_PORT/")

# Extract scriptPubKey from coinbase output
SCRIPT_PUBKEY=$(echo "$LATEST_BLOCK" | jq -r '.result.vout[0].scriptPubKey.hex')

echo "   Coinbase scriptPubKey: $SCRIPT_PUBKEY"

# Verify script format: should start with 0014 (OP_0 + push 20 bytes)
if [[ "$SCRIPT_PUBKEY" =~ ^0014[0-9a-f]{40}$ ]]; then
    echo "✅ Coinbase script format is correct:"
    echo "   - Starts with 0014 (OP_0 + push 20 bytes)"
    echo "   - Followed by 40 hex chars (20 bytes)"
    echo "   - Total length: 44 hex chars (22 bytes)"
else
    echo "❌ Coinbase script format is incorrect:"
    echo "   Expected: 0014 + 40 hex chars"
    echo "   Got: $SCRIPT_PUBKEY"
    echo "   Length: ${#SCRIPT_PUBKEY} chars"
    exit 1
fi

# Check daemon logs for witness-based creation
echo "📋 Checking daemon logs for witness-based coinbase creation..."
if grep -q "Creating coinbase transaction directly from witness data" "$TEST_DIR/daemon.log"; then
    echo "✅ Found witness-based coinbase creation log"
else
    echo "❌ Missing witness-based coinbase creation log"
    exit 1
fi

if grep -q "Coinbase script from cached witness" "$TEST_DIR/daemon.log"; then
    echo "✅ Found cached witness script generation log"
else
    echo "❌ Missing cached witness script generation log"
    exit 1
fi

# Check for absence of Bech32 decode errors
echo "🔍 Checking for absence of Bech32 decode errors..."
if grep -q "Failed to decode Bech32 address" "$TEST_DIR/daemon.log"; then
    echo "❌ Found Bech32 decode errors in logs"
    grep "Failed to decode Bech32 address" "$TEST_DIR/daemon.log"
    exit 1
else
    echo "✅ No Bech32 decode errors found"
fi

# Check HRP consistency
echo "🔍 Checking HRP consistency in logs..."
if grep -q "HRP=rdin" "$TEST_DIR/daemon.log"; then
    echo "✅ Found correct regtest HRP (rdin) in logs"
else
    echo "❌ Missing correct regtest HRP in logs"
    exit 1
fi

if grep -q "HRP=din" "$TEST_DIR/daemon.log"; then
    echo "❌ Found incorrect mainnet HRP (din) in regtest logs"
    exit 1
else
    echo "✅ No incorrect mainnet HRP found in logs"
fi

# Clean up
echo "🧹 Cleaning up..."
kill $DAEMON_PID 2>/dev/null || true
sleep 2

echo ""
echo "🎉 All tests passed successfully!"
echo "✅ Witness-based mining working correctly"
echo "✅ Coinbase script format: 0014 + 20 bytes (P2WPKH)"
echo "✅ No Bech32 decode errors"
echo "✅ HRP consistency maintained (rdin on regtest)"
echo "✅ Clean, production-ready mining implementation"
