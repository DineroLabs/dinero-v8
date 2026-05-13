#!/bin/bash
# DineroCoin v0.6.0 Public Smoke Test Script
# Tests core wallet and mining functionality

set -e

echo "🧪 DineroCoin v0.6.0 Smoke Test"
echo "================================"

# Canonical regtest datadir.
DATADIR="${DINERO_DATADIR:-$HOME/.dinero/regtest}"

echo "📁 Data directory: $DATADIR"

# Check if daemon is running
if [ ! -f "$DATADIR/nodeinfo.json" ]; then
    echo "❌ Daemon not running or nodeinfo.json missing"
    echo "💡 Start daemon with: dinerod -regtest -rpcport=0 -wsport=0 -port=0"
    exit 1
fi

# Read nodeinfo and extract endpoints
if ! command -v jq &> /dev/null; then
    echo "❌ jq not found. Install with: brew install jq (macOS) or apt install jq (Linux)"
    exit 1
fi

RPC_URL=$(jq -r '.rpc.url' "$DATADIR/nodeinfo.json")
COOKIE_PATH=$(jq -r '.cookie' "$DATADIR/nodeinfo.json")
NETWORK=$(jq -r '.network' "$DATADIR/nodeinfo.json")

echo "🔗 RPC URL: $RPC_URL"
echo "🍪 Cookie: $COOKIE_PATH"
echo "🌐 Network: $NETWORK"

# Validate network
if [ "$NETWORK" != "regtest" ]; then
    echo "❌ Expected regtest network, got: $NETWORK"
    echo "💡 This smoke test is designed for regtest only"
    exit 1
fi

# Read auth cookie
if [ ! -f "$COOKIE_PATH" ]; then
    echo "❌ Cookie file not found: $COOKIE_PATH"
    exit 1
fi

AUTH=$(tr -d '\r\n' < "$COOKIE_PATH")
echo "✅ Authentication loaded"

# Helper function for RPC calls
rpc_call() {
    local method="$1"
    local params="$2"
    
    curl -s -u "$AUTH" -H 'content-type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}" \
        "$RPC_URL"
}

echo ""
echo "🔍 Testing RPC connectivity..."

# Test basic connectivity
RESULT=$(rpc_call "getbestblockhash" "[]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ RPC call failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi

BLOCK_HASH=$(echo "$RESULT" | jq -r '.result')
echo "✅ RPC connected - Latest block: ${BLOCK_HASH:0:16}..."

echo ""
echo "🏦 Testing wallet operations..."

# Create test wallet
echo "Creating test wallet..."
RESULT=$(rpc_call "wallet.create" "[\"smoke_test\", \"password123\"]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    ERROR_MSG=$(echo "$RESULT" | jq -r '.error.message')
    if [[ "$ERROR_MSG" == *"already exists"* ]]; then
        echo "ℹ️  Wallet already exists, loading..."
    else
        echo "❌ Wallet creation failed: $ERROR_MSG"
        exit 1
    fi
fi

# Load wallet
echo "Loading wallet..."
RESULT=$(rpc_call "wallet.load" "[\"smoke_test\", \"password123\"]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ Wallet load failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi
echo "✅ Wallet loaded"

# Generate new address
echo "Generating new address..."
RESULT=$(rpc_call "wallet.getnewaddress" "[]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ Address generation failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi

ADDR=$(echo "$RESULT" | jq -r '.result')
echo "✅ Generated address: $ADDR"

# Validate address ownership
echo "Validating address ownership..."
RESULT=$(rpc_call "wallet.validateaddress" "[\"$ADDR\"]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ Address validation failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi

IS_MINE=$(echo "$RESULT" | jq -r '.result.ismine')
if [ "$IS_MINE" != "true" ]; then
    echo "❌ Address not owned by wallet: ismine=$IS_MINE"
    exit 1
fi
echo "✅ Address ownership confirmed"

echo ""
echo "⛏️  Testing mining operations..."

# Set mining address
echo "Setting mining address..."
RESULT=$(rpc_call "mining.setaddress" "[\"$ADDR\"]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ Mining address setup failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi
echo "✅ Mining address set"

# Get mining address
echo "Verifying mining address..."
RESULT=$(rpc_call "mining.getaddress" "[]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ Mining address retrieval failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi

MINING_ADDR=$(echo "$RESULT" | jq -r '.result.address')
IS_MINE_MINING=$(echo "$RESULT" | jq -r '.result.ismine')

if [ "$MINING_ADDR" != "$ADDR" ]; then
    echo "❌ Mining address mismatch: expected $ADDR, got $MINING_ADDR"
    exit 1
fi

if [ "$IS_MINE_MINING" != "true" ]; then
    echo "❌ Mining address not owned: ismine=$IS_MINE_MINING"
    exit 1
fi
echo "✅ Mining address verified"

# Generate test blocks
echo "Generating test blocks..."
RESULT=$(rpc_call "mining.generatetoaddress" "[3, \"$ADDR\"]")
if echo "$RESULT" | jq -e '.error' > /dev/null; then
    echo "❌ Block generation failed:"
    echo "$RESULT" | jq '.error'
    exit 1
fi

BLOCKS=$(echo "$RESULT" | jq -r '.result | length')
echo "✅ Generated $BLOCKS blocks"

# Show first block hash
FIRST_BLOCK=$(echo "$RESULT" | jq -r '.result[0]')
echo "   First block: ${FIRST_BLOCK:0:16}..."

echo ""
echo "🎉 All tests passed!"
echo ""
echo "📊 Summary:"
echo "  ✅ RPC connectivity"
echo "  ✅ Wallet creation and loading"
echo "  ✅ Address generation and validation"
echo "  ✅ Mining address configuration"
echo "  ✅ Block generation (regtest)"
echo ""
echo "🚀 DineroCoin v0.6.0 is working correctly!"
