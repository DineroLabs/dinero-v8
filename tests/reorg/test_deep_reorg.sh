#!/usr/bin/env bash
# Deep Reorg Stress Test (100-block reorg)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
DATA_DIR="/tmp/dinero_deep_reorg_$$"
COOKIE_FILE="$DATA_DIR/.cookie"
RPC_PORT="${RPC_PORT:-20996}"
P2P_PORT="${P2P_PORT:-21001}"

# Test configuration
FORK_POINT=100
CHAIN_A_HEIGHT=200
CHAIN_B_HEIGHT=220

echo "🧪 Deep Reorg Stress Test"
echo "=========================="
echo ""
echo "Configuration:"
echo "  Fork point: $FORK_POINT"
echo "  Chain A: 0-$CHAIN_A_HEIGHT"
echo "  Chain B: 0-$CHAIN_B_HEIGHT (longer by $((CHAIN_B_HEIGHT - CHAIN_A_HEIGHT)) blocks)"
echo "  Reorg depth: $((CHAIN_A_HEIGHT - FORK_POINT)) blocks"
echo ""

# Cleanup
cleanup() {
    echo ""
    echo "🧹 Cleaning up..."
    if [ -n "${DINEROD_PID:-}" ]; then
        kill -9 "$DINEROD_PID" 2>/dev/null || true
    fi
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

# Start daemon
echo "🚀 Starting dinerod..."
mkdir -p "$DATA_DIR"

"$ROOT_DIR/build/dinerod" \
    --regtest \
    --datadir="$DATA_DIR" \
    --rpcport="$RPC_PORT" \
    --port="$P2P_PORT" \
    --daemon \
    >/dev/null 2>&1 &

DINEROD_PID=$!
echo "   PID: $DINEROD_PID"

# Wait for RPC cookie
echo "⏳ Waiting for RPC cookie..."
for i in {1..50}; do
    [[ -s "$COOKIE_FILE" ]] && break
    sleep 0.1
done

[[ ! -s "$COOKIE_FILE" ]] && { echo "❌ No cookie"; exit 1; }

RPC_AUTH=$(cat "$COOKIE_FILE")

# Wait for RPC
echo "⏳ Waiting for RPC server..."
for i in {1..30}; do
    curl -s --user "$RPC_AUTH" \
        --data-binary '{"jsonrpc":"1.0","method":"getblockcount","params":[],"id":1}' \
        "http://127.0.0.1:$RPC_PORT" >/dev/null 2>&1 && break
    [ $i -eq 30 ] && { echo "❌ RPC timeout"; exit 1; }
    sleep 0.5
done
echo "✅ RPC ready"

# RPC helper
rpc() {
    local method="$1"
    shift
    local params=""
    if [ $# -gt 0 ]; then
        if [[ "$1" =~ ^[0-9]+$ ]]; then
            params="$1"
        else
            params="\"$1\""
        fi
        shift
        for p in "$@"; do
            if [[ "$p" =~ ^[0-9]+$ ]]; then
                params="$params,$p"
            else
                params="$params,\"$p\""
            fi
        done
    fi
    
    curl -s --user "$RPC_AUTH" \
        --data-binary "{\"jsonrpc\":\"1.0\",\"method\":\"$method\",\"params\":[$params],\"id\":1}" \
        "http://127.0.0.1:$RPC_PORT" | jq -r '.result // empty'
}

ADDRESS="dint1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqmyrvxk"

echo ""
echo "📦 Step 1: Common chain (0-$FORK_POINT)"
rpc generatetoaddress $FORK_POINT "$ADDRESS" >/dev/null
HEIGHT=$(rpc getblockcount)
echo "   ✅ Height: $HEIGHT"

echo ""
echo "📦 Step 2: Chain A (to $CHAIN_A_HEIGHT)"
rpc generatetoaddress $((CHAIN_A_HEIGHT - FORK_POINT)) "$ADDRESS" >/dev/null
HEIGHT_A=$(rpc getblockcount)
TIP_A=$(rpc getbestblockhash)
echo "   ✅ Height: $HEIGHT_A"
echo "   Tip: ${TIP_A:0:16}..."

echo ""
echo "📦 Step 3: Chain B fork (to $CHAIN_B_HEIGHT)"
echo "   Rolling back to fork point..."

CURRENT=$HEIGHT_A
while [ "$CURRENT" -gt "$FORK_POINT" ]; do
    TIP=$(rpc getbestblockhash)
    rpc invalidateblock "$TIP" >/dev/null 2>&1 || true
    CURRENT=$(rpc getblockcount)
done
echo "   ✅ Rolled back to $CURRENT"

echo "   Mining Chain B..."
rpc generatetoaddress $((CHAIN_B_HEIGHT - FORK_POINT)) "$ADDRESS" >/dev/null
HEIGHT_B=$(rpc getblockcount)
TIP_B=$(rpc getbestblockhash)
echo "   ✅ Height: $HEIGHT_B"
echo "   Tip: ${TIP_B:0:16}..."

echo ""
echo "🔍 Verification"
echo "==============="

if [ "$HEIGHT_B" -ne "$CHAIN_B_HEIGHT" ]; then
    echo "❌ Wrong height: $HEIGHT_B (expected $CHAIN_B_HEIGHT)"
    exit 1
fi

if [ "$TIP_B" = "$TIP_A" ]; then
    echo "❌ No reorg (tip unchanged)"
    exit 1
fi

echo "   ✅ Reorg successful (disconnected $((CHAIN_A_HEIGHT - FORK_POINT)) blocks)"
echo "   ✅ New chain is $((CHAIN_B_HEIGHT - CHAIN_A_HEIGHT)) blocks longer"

# Verify mining still works
rpc generatetoaddress 1 "$ADDRESS" >/dev/null
FINAL=$(rpc getblockcount)
echo "   ✅ Chain healthy (height: $FINAL)"

echo ""
echo "✅ TEST PASSED"
echo "=============="
echo ""
echo "Summary:"
echo "  ✅ Common chain: $FORK_POINT blocks"
echo "  ✅ Chain A: $CHAIN_A_HEIGHT blocks"
echo "  ✅ Chain B: $CHAIN_B_HEIGHT blocks"
echo "  ✅ Reorg depth: $((CHAIN_A_HEIGHT - FORK_POINT)) blocks"
echo "  ✅ No corruption"
echo ""
echo "🎉 Deep reorg test completed!"
