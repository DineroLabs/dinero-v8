#!/bin/bash
# Cross-network HRP validation test
# Tests that address validation correctly rejects addresses from other networks

set -Eeuo pipefail

# Test configuration
DATADIR=$(mktemp -d /tmp/din-cross-network-test-XXXX)
LOG="$DATADIR/daemon.log"

# Cleanup function
cleanup() {
    echo "🧹 Cleaning up..."
    pkill -f dinerod || true
    rm -rf "$DATADIR"
}
trap cleanup EXIT

# RPC helper function
rpc() {
    local method="$1"
    local params="$2"
    local cookie=$(cut -d: -f2 "$DATADIR/regtest/.cookie")
    local auth="Authorization: Basic $(printf '__cookie__:%s' "$cookie" | base64)"
    
    curl -s -X POST -H "Content-Type: application/json" -H "$auth" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}" \
        "http://127.0.0.1:20999/"
}

echo "🔍 Cross-network HRP validation test"
echo "======================================"

# Test 1: Regtest (rdin HRP)
echo "📋 Test 1: Regtest network (rdin HRP)"
echo "Starting regtest daemon..."

# Start regtest daemon
./build/bin/dinerod --regtest --datadir="$DATADIR" --httpport=20999 --log-level=info >"$LOG" 2>&1 &
sleep 3

# Wait for daemon to be ready
for i in {1..30}; do
    if curl -s http://127.0.0.1:20999/healthz >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

# Create and load wallet
rpc "wallet.create" '["default","test_seed"]' >/dev/null
rpc "wallet.load" '["default"]' >/dev/null

# Generate a regtest address
REGTEST_ADDR=$(rpc "getnewaddress" '[]' | jq -r '.result')
echo "Generated regtest address: $REGTEST_ADDR"

# Validate regtest address (should pass)
REGTEST_VALID=$(rpc "wallet.validateaddress" "[\"$REGTEST_ADDR\"]" | jq -r '.result.isvalid')
if [ "$REGTEST_VALID" = "true" ]; then
    echo "✅ Regtest address validation: PASS"
else
    echo "❌ Regtest address validation: FAIL"
    exit 1
fi

# Test mainnet address on regtest (should fail)
MAINNET_ADDR="din1q68f926c852932c64715f5fef05cdafba74eaf3f1"
MAINNET_VALID=$(rpc "wallet.validateaddress" "[\"$MAINNET_ADDR\"]" | jq -r '.result.isvalid')
if [ "$MAINNET_VALID" = "false" ]; then
    echo "✅ Mainnet address rejection on regtest: PASS"
else
    echo "❌ Mainnet address rejection on regtest: FAIL"
    exit 1
fi

# Test testnet address on regtest (should fail)
TESTNET_ADDR="tdin1q68f926c852932c64715f5fef05cdafba74eaf3f1"
TESTNET_VALID=$(rpc "wallet.validateaddress" "[\"$TESTNET_ADDR\"]" | jq -r '.result.isvalid')
if [ "$TESTNET_VALID" = "false" ]; then
    echo "✅ Testnet address rejection on regtest: PASS"
else
    echo "❌ Testnet address rejection on regtest: FAIL"
    exit 1
fi

# Stop regtest daemon
pkill -f dinerod
sleep 2

echo ""
echo "📋 Test 2: Mainnet network (din HRP)"
echo "Starting mainnet daemon..."

# Start mainnet daemon
./build/bin/dinerod --mainnet --datadir="$DATADIR" --httpport=20999 --log-level=info >"$LOG" 2>&1 &
sleep 3

# Wait for daemon to be ready
for i in {1..30}; do
    if curl -s http://127.0.0.1:20999/healthz >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

# Create and load wallet
rpc "wallet.create" '["default","test_seed"]' >/dev/null
rpc "wallet.load" '["default"]' >/dev/null

# Generate a mainnet address
MAINNET_GEN_ADDR=$(rpc "getnewaddress" '[]' | jq -r '.result')
echo "Generated mainnet address: $MAINNET_GEN_ADDR"

# Validate mainnet address (should pass)
MAINNET_GEN_VALID=$(rpc "wallet.validateaddress" "[\"$MAINNET_GEN_ADDR\"]" | jq -r '.result.isvalid')
if [ "$MAINNET_GEN_VALID" = "true" ]; then
    echo "✅ Mainnet address validation: PASS"
else
    echo "❌ Mainnet address validation: FAIL"
    exit 1
fi

# Test regtest address on mainnet (should fail)
REGTEST_VALID_MAINNET=$(rpc "wallet.validateaddress" "[\"$REGTEST_ADDR\"]" | jq -r '.result.isvalid')
if [ "$REGTEST_VALID_MAINNET" = "false" ]; then
    echo "✅ Regtest address rejection on mainnet: PASS"
else
    echo "❌ Regtest address rejection on mainnet: FAIL"
    exit 1
fi

# Test testnet address on mainnet (should fail)
TESTNET_VALID_MAINNET=$(rpc "wallet.validateaddress" "[\"$TESTNET_ADDR\"]" | jq -r '.result.isvalid')
if [ "$TESTNET_VALID_MAINNET" = "false" ]; then
    echo "✅ Testnet address rejection on mainnet: PASS"
else
    echo "❌ Testnet address rejection on mainnet: FAIL"
    exit 1
fi

# Stop mainnet daemon
pkill -f dinerod
sleep 2

echo ""
echo "📋 Test 3: Testnet network (tdin HRP)"
echo "Starting testnet daemon..."

# Start testnet daemon
./build/bin/dinerod --testnet --datadir="$DATADIR" --httpport=20999 --log-level=info >"$LOG" 2>&1 &
sleep 3

# Wait for daemon to be ready
for i in {1..30}; do
    if curl -s http://127.0.0.1:20999/healthz >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

# Create and load wallet
rpc "wallet.create" '["default","test_seed"]' >/dev/null
rpc "wallet.load" '["default"]' >/dev/null

# Generate a testnet address
TESTNET_GEN_ADDR=$(rpc "getnewaddress" '[]' | jq -r '.result')
echo "Generated testnet address: $TESTNET_GEN_ADDR"

# Validate testnet address (should pass)
TESTNET_GEN_VALID=$(rpc "wallet.validateaddress" "[\"$TESTNET_GEN_ADDR\"]" | jq -r '.result.isvalid')
if [ "$TESTNET_GEN_VALID" = "true" ]; then
    echo "✅ Testnet address validation: PASS"
else
    echo "❌ Testnet address validation: FAIL"
    exit 1
fi

# Test regtest address on testnet (should fail)
REGTEST_VALID_TESTNET=$(rpc "wallet.validateaddress" "[\"$REGTEST_ADDR\"]" | jq -r '.result.isvalid')
if [ "$REGTEST_VALID_TESTNET" = "false" ]; then
    echo "✅ Regtest address rejection on testnet: PASS"
else
    echo "❌ Regtest address rejection on testnet: FAIL"
    exit 1
fi

# Test mainnet address on testnet (should fail)
MAINNET_VALID_TESTNET=$(rpc "wallet.validateaddress" "[\"$MAINNET_GEN_ADDR\"]" | jq -r '.result.isvalid')
if [ "$MAINNET_VALID_TESTNET" = "false" ]; then
    echo "✅ Mainnet address rejection on testnet: PASS"
else
    echo "❌ Mainnet address rejection on testnet: FAIL"
    exit 1
fi

echo ""
echo "🎉 All cross-network HRP tests passed!"
echo "======================================"
echo "✅ Regtest (rdin): accepts rdin, rejects din/tdin"
echo "✅ Mainnet (din): accepts din, rejects rdin/tdin"
echo "✅ Testnet (tdin): accepts tdin, rejects rdin/din"
echo ""
echo "Cross-network HRP validation is working correctly."
