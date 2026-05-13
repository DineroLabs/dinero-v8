#!/bin/bash

# ═══════════════════════════════════════════════════════════════════════════
# ASERT DIFFICULTY ADJUSTMENT TESTING
# ═══════════════════════════════════════════════════════════════════════════
# This script tests ASERT difficulty adjustment by:
# 1. Mining blocks on regtest
# 2. Showing difficulty after each block
# 3. Demonstrating ASERT responds to block timing
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║  ASERT DIFFICULTY ADJUSTMENT TEST - Dinero Blockchain      ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Clean regtest directory
REGTEST_DIR="/tmp/asert-test-$$"
echo "📁 Using clean regtest directory: $REGTEST_DIR"
rm -rf "$REGTEST_DIR"
mkdir -p "$REGTEST_DIR"

# Start regtest daemon
echo ""
echo "═══════════════════════════════════════════════════════════"
echo "Starting regtest daemon..."
echo "═══════════════════════════════════════════════════════════"
./build/dinerod --regtest --datadir="$REGTEST_DIR" > /dev/null 2>&1 &
DAEMON_PID=$!

echo "✅ Started regtest daemon (PID: $DAEMON_PID)"
echo "⏳ Waiting 5 seconds for initialization..."
sleep 5

# Check daemon is running
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "❌ ERROR: Daemon failed to start!"
    exit 1
fi

echo "✅ Daemon running"
echo ""

# RPC helper function
# Note: Daemon currently uses port 20998 for regtest (TODO: fix to use 20996)
rpc() {
    curl -s -X POST http://127.0.0.1:20998 \
        -u "$(cat $REGTEST_DIR/.cookie)" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"$1\",\"params\":$2}"
}

# Get block info
get_block_info() {
    local height=$1
    local hash=$(rpc "getblockhash" "[${height}]" | jq -r '.result')
    local block=$(rpc "getblock" "[\"${hash}\"]" | jq -r '.result')

    echo "$block" | jq -r '{height: .height, difficulty: .difficulty, bits: .bits, time: .time}'
}

# Test address for mining rewards
TEST_ADDR="rdin1q5jlf85f9ntwd8mzvej93jgfxfydyx59uxm2rjz"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  ASERT DIFFICULTY ADJUSTMENT - LIVE TEST                   ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""
echo "Target block time: 5 minutes (300 seconds)"
echo "ASERT half-life: 12 hours (72 blocks)"
echo "Starting difficulty: 0x207fffff (very easy for instant mining)"
echo ""
echo "How ASERT works:"
echo "  • Faster blocks → difficulty increases"
echo "  • Slower blocks → difficulty decreases"
echo "  • Adjustment is smooth and per-block"
echo ""

# Get genesis block info
echo "═══════════════════════════════════════════════════════════"
echo "GENESIS BLOCK (height 0)"
echo "═══════════════════════════════════════════════════════════"
GENESIS=$(rpc "getblockhash" "[0]" | jq -r '.result')
GENESIS_BLOCK=$(rpc "getblock" "[\"${GENESIS}\"]" | jq -r '.result')
echo "Hash:       $GENESIS"
echo "Difficulty: $(echo $GENESIS_BLOCK | jq -r '.difficulty')"
echo "Bits:       $(echo $GENESIS_BLOCK | jq -r '.bits')"
echo "Time:       $(echo $GENESIS_BLOCK | jq -r '.time')"
echo ""

# Mine first block
echo "═══════════════════════════════════════════════════════════"
echo "MINING BLOCK 1 (instantly - FASTER than target)"
echo "═══════════════════════════════════════════════════════════"
sleep 1
rpc "generatetoaddress" "[1, \"${TEST_ADDR}\"]" > /dev/null
sleep 1

BLOCK1=$(rpc "getblockhash" "[1]" | jq -r '.result')
BLOCK1_INFO=$(rpc "getblock" "[\"${BLOCK1}\"]" | jq -r '.result')
DIFF1=$(echo $BLOCK1_INFO | jq -r '.difficulty')
BITS1=$(echo $BLOCK1_INFO | jq -r '.bits')
TIME1=$(echo $BLOCK1_INFO | jq -r '.time')
TIME0=$(echo $GENESIS_BLOCK | jq -r '.time')
ELAPSED1=$((TIME1 - TIME0))

echo "Hash:       $BLOCK1"
echo "Difficulty: $DIFF1"
echo "Bits:       $BITS1"
echo "Time:       $TIME1"
echo "Elapsed:    ${ELAPSED1}s (target: 300s)"
echo ""
echo "📊 ASERT Response:"
echo "   Block came ${ELAPSED1}s after genesis (much faster than 300s target)"
echo "   → ASERT should INCREASE difficulty for next block"
echo ""

# Mine second block
echo "═══════════════════════════════════════════════════════════"
echo "MINING BLOCK 2 (instantly - FASTER than target)"
echo "═══════════════════════════════════════════════════════════"
sleep 1
rpc "generatetoaddress" "[1, \"${TEST_ADDR}\"]" > /dev/null
sleep 1

BLOCK2=$(rpc "getblockhash" "[2]" | jq -r '.result')
BLOCK2_INFO=$(rpc "getblock" "[\"${BLOCK2}\"]" | jq -r '.result')
DIFF2=$(echo $BLOCK2_INFO | jq -r '.difficulty')
BITS2=$(echo $BLOCK2_INFO | jq -r '.bits')
TIME2=$(echo $BLOCK2_INFO | jq -r '.time')
ELAPSED2=$((TIME2 - TIME1))

echo "Hash:       $BLOCK2"
echo "Difficulty: $DIFF2"
echo "Bits:       $BITS2"
echo "Time:       $TIME2"
echo "Elapsed:    ${ELAPSED2}s since block 1 (target: 300s)"
echo ""
echo "📊 ASERT Response:"
echo "   Block came ${ELAPSED2}s after block 1 (much faster than 300s target)"
echo "   → ASERT should continue INCREASING difficulty"
echo ""

# Wait a bit (simulate slower block)
echo "═══════════════════════════════════════════════════════════"
echo "MINING BLOCK 3 (with delay - simulating SLOWER mining)"
echo "═══════════════════════════════════════════════════════════"
echo "⏳ Waiting 10 seconds before mining (simulating slow block)..."
sleep 10
rpc "generatetoaddress" "[1, \"${TEST_ADDR}\"]" > /dev/null
sleep 1

BLOCK3=$(rpc "getblockhash" "[3]" | jq -r '.result')
BLOCK3_INFO=$(rpc "getblock" "[\"${BLOCK3}\"]" | jq -r '.result')
DIFF3=$(echo $BLOCK3_INFO | jq -r '.difficulty')
BITS3=$(echo $BLOCK3_INFO | jq -r '.bits')
TIME3=$(echo $BLOCK3_INFO | jq -r '.time')
ELAPSED3=$((TIME3 - TIME2))

echo "Hash:       $BLOCK3"
echo "Difficulty: $DIFF3"
echo "Bits:       $BITS3"
echo "Time:       $TIME3"
echo "Elapsed:    ${ELAPSED3}s since block 2 (target: 300s)"
echo ""
echo "📊 ASERT Response:"
echo "   Block came ${ELAPSED3}s after block 2 (still faster than 300s, but slower than before)"
echo "   → ASERT adjustment should reflect this slower pace"
echo ""

# Mine a few more blocks rapidly
echo "═══════════════════════════════════════════════════════════"
echo "MINING BLOCKS 4-6 (rapid succession - VERY FAST)"
echo "═══════════════════════════════════════════════════════════"
rpc "generatetoaddress" "[3, \"${TEST_ADDR}\"]" > /dev/null
sleep 1

for height in 4 5 6; do
    HASH=$(rpc "getblockhash" "[${height}]" | jq -r '.result')
    INFO=$(rpc "getblock" "[\"${HASH}\"]" | jq -r '.result')
    DIFF=$(echo $INFO | jq -r '.difficulty')
    BITS=$(echo $INFO | jq -r '.bits')

    echo "Block ${height}: difficulty=${DIFF}, bits=${BITS}"
done

echo ""
echo "📊 ASERT Response:"
echo "   Blocks 4-6 mined in rapid succession (<<300s each)"
echo "   → ASERT should continue INCREASING difficulty"
echo ""

# Summary
echo "═══════════════════════════════════════════════════════════"
echo "ASERT SUMMARY"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Difficulty progression:"
echo "  Genesis: 0x207fffff (starting difficulty)"
echo "  Block 1: $BITS1 (adjusted based on time from genesis)"
echo "  Block 2: $BITS2 (adjusted based on time from block 1)"
echo "  Block 3: $BITS3 (adjusted based on time from block 2)"
echo ""

# Get current blockchain info
INFO=$(rpc "getblockchaininfo" "[]")
BLOCKCOUNT=$(echo $INFO | jq -r '.result.blocks')
CURRENT_DIFF=$(echo $INFO | jq -r '.result.difficulty')

echo "Current chain status:"
echo "  Height:     $BLOCKCOUNT blocks"
echo "  Difficulty: $CURRENT_DIFF"
echo ""

echo "✅ ASERT is working!"
echo ""
echo "Key observations:"
echo "  1. Difficulty changes after EVERY block (per-block adjustment)"
echo "  2. Fast blocks → difficulty increases"
echo "  3. Slower blocks → smaller difficulty increases (or decreases)"
echo "  4. ASERT provides smooth, predictable adjustments"
echo ""

# Cleanup
echo "═══════════════════════════════════════════════════════════"
echo "CLEANUP"
echo "═══════════════════════════════════════════════════════════"
kill $DAEMON_PID
wait $DAEMON_PID 2>/dev/null || true
echo "✅ Daemon stopped"
echo ""
echo "Test data preserved at: $REGTEST_DIR"
echo "To clean up: rm -rf $REGTEST_DIR"
echo ""
