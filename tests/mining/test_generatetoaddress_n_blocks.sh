#!/usr/bin/env bash
# CI Guardrail: Verify generatetoaddress N mines exactly N blocks
#
# This test prevents regressions of:
# - Early return inside mining loop (ignoring nblocks parameter)
# - Stale block template causing partial mining failures
# - Any other bug that causes generatetoaddress to mine fewer than N blocks
#
# Expected behavior (Bitcoin Core compatible):
# - generatetoaddress N should increase chain height by exactly N
# - All N blocks should be returned in the response
# - Each block should build on the previous block's hash

set -euo pipefail

# Test configuration
REGTEST_DATADIR="/tmp/dinero_ci_regtest_$$"
REGTEST_PORT="${REGTEST_PORT:-21001}"
RPC_PORT="${RPC_PORT:-20996}"
TEST_ADDR="rdin1px32rxxxmu03rh6mvp43q4jwvpl3utz206rlfuvg5pv5wk2wmgwgqpxxg8n"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "CI Test: generatetoaddress N blocks"
echo "=========================================="
echo ""

# Cleanup function
cleanup() {
    if [ -n "${DAEMON_PID:-}" ]; then
        echo -e "${YELLOW}Stopping daemon (PID: $DAEMON_PID)...${NC}"
        kill -9 "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$REGTEST_DATADIR"
}
trap cleanup EXIT

# Start regtest daemon
echo -e "${YELLOW}Starting regtest daemon...${NC}"
rm -rf "$REGTEST_DATADIR"
./dinerod --regtest --datadir="$REGTEST_DATADIR" --port="$REGTEST_PORT" --rpcport="$RPC_PORT" > /dev/null 2>&1 &
DAEMON_PID=$!
sleep 3

if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo -e "${RED}❌ FAIL: Daemon failed to start${NC}"
    exit 1
fi

echo -e "${GREEN}✅ Daemon started (PID: $DAEMON_PID)${NC}"
echo ""

# Helper function for RPC calls
rpc_call() {
    ./dinero-cli -datadir="$REGTEST_DATADIR" -rpcport="$RPC_PORT" "$@"
}

# Test 1: Mine 1 block
echo -e "${YELLOW}Test 1: generatetoaddress 1${NC}"
H1=$(rpc_call getblockcount)
rpc_call generatetoaddress 1 "$TEST_ADDR" > /dev/null
H2=$(rpc_call getblockcount)
MINED=$((H2 - H1))

if [ "$MINED" -ne 1 ]; then
    echo -e "${RED}❌ FAIL: Expected 1 block, mined $MINED${NC}"
    exit 1
fi
echo -e "${GREEN}✅ PASS: Mined exactly 1 block${NC}"
echo ""

# Test 2: Mine 5 blocks
echo -e "${YELLOW}Test 2: generatetoaddress 5${NC}"
H1=$(rpc_call getblockcount)
rpc_call generatetoaddress 5 "$TEST_ADDR" > /dev/null
H2=$(rpc_call getblockcount)
MINED=$((H2 - H1))

if [ "$MINED" -ne 5 ]; then
    echo -e "${RED}❌ FAIL: Expected 5 blocks, mined $MINED${NC}"
    exit 1
fi
echo -e "${GREEN}✅ PASS: Mined exactly 5 blocks${NC}"
echo ""

# Test 3: Mine 10 blocks (maturity threshold)
echo -e "${YELLOW}Test 3: generatetoaddress 10 (maturity threshold)${NC}"
H1=$(rpc_call getblockcount)
rpc_call generatetoaddress 10 "$TEST_ADDR" > /dev/null
H2=$(rpc_call getblockcount)
MINED=$((H2 - H1))

if [ "$MINED" -ne 10 ]; then
    echo -e "${RED}❌ FAIL: Expected 10 blocks, mined $MINED${NC}"
    exit 1
fi
echo -e "${GREEN}✅ PASS: Mined exactly 10 blocks${NC}"
echo ""

# Test 4: Mine 20 blocks (stress test)
echo -e "${YELLOW}Test 4: generatetoaddress 20 (stress test)${NC}"
H1=$(rpc_call getblockcount)
rpc_call generatetoaddress 20 "$TEST_ADDR" > /dev/null
H2=$(rpc_call getblockcount)
MINED=$((H2 - H1))

if [ "$MINED" -ne 20 ]; then
    echo -e "${RED}❌ FAIL: Expected 20 blocks, mined $MINED${NC}"
    exit 1
fi
echo -e "${GREEN}✅ PASS: Mined exactly 20 blocks${NC}"
echo ""

# Final report
FINAL_HEIGHT=$(rpc_call getblockcount)
echo "=========================================="
echo -e "${GREEN}✅ ALL TESTS PASSED${NC}"
echo "   Final chain height: $FINAL_HEIGHT"
echo "   generatetoaddress N reliably mines N blocks"
echo "=========================================="

exit 0
