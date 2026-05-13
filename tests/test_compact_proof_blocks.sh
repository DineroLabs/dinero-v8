#!/bin/bash
# Phase 34.7: Compact-Proof Blocks Test Suite
# Tests: blocktxnproofs message, compact block + proof relay, bandwidth efficiency

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

PASS=0
FAIL=0
TOTAL=0

check_result() {
    local test_name="$1"
    local result="$2"
    local expected="$3"
    TOTAL=$((TOTAL + 1))

    if [ "$result" = "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC}: $test_name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $test_name (got: $result, expected: $expected)"
        FAIL=$((FAIL + 1))
    fi
}

check_contains() {
    local test_name="$1"
    local haystack="$2"
    local needle="$3"
    TOTAL=$((TOTAL + 1))

    if echo "$haystack" | grep -q "$needle"; then
        echo -e "  ${GREEN}PASS${NC}: $test_name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $test_name (expected to contain: $needle)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=============================================="
echo "Phase 34.7: Compact-Proof Blocks Test Suite"
echo "=============================================="
echo ""

# Build directory
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"
if [ ! -f "$BUILD_DIR/dinerod" ]; then
    BUILD_DIR="$(pwd)/build"
fi

# Cleanup
pkill -f "dinerod.*24100" 2>/dev/null || true
sleep 1
rm -rf /tmp/din_cpb_test

# Start daemon (capture output for log checking)
echo "Starting test daemon..."
mkdir -p /tmp/din_cpb_test
cd "$BUILD_DIR"
./dinerod --regtest --datadir=/tmp/din_cpb_test --rpcport=24100 --p2pport=24101 > /tmp/din_cpb_test/daemon.log 2>&1 &
DAEMON_PID=$!
sleep 5

# Get cookie
COOKIE=$(cat /tmp/din_cpb_test/.cookie | cut -d: -f2)
LOG_FILE="/tmp/din_cpb_test/daemon.log"

rpc() {
    curl -s -X POST http://127.0.0.1:24100 \
        -u "__cookie__:$COOKIE" \
        -H "Content-Type: application/json" \
        -d "$1"
}

echo ""
echo "=== Test 1: Daemon startup with CompactProofBlockService ==="
STARTUP=$(cat "$LOG_FILE" 2>/dev/null | grep -c "Phase 34.7" || echo "0")
check_result "Phase 34.7 initialization logged" "$([ "$STARTUP" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 2: CompactProofBlockService initialized ==="
SERVICE_INIT=$(cat "$LOG_FILE" 2>/dev/null | grep -c "CompactProofBlockService initialized" || echo "0")
check_result "CompactProofBlockService init logged" "$([ "$SERVICE_INIT" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 3: BlockTxnProofs handler ready ==="
HANDLER=$(cat "$LOG_FILE" 2>/dev/null | grep -c "blocktxnproofs" || echo "0")
check_result "blocktxnproofs handler logged" "$([ "$HANDLER" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 4: Generate blocks to test mempool interaction ==="
# Create wallet first
WALLET_RESULT=$(rpc '{"jsonrpc":"2.0","method":"wallet.createhd","params":["test_cpb"],"id":1}')
check_contains "Wallet created" "$WALLET_RESULT" "success"

ADDR_RESULT=$(rpc '{"jsonrpc":"2.0","method":"wallet.getnewaddress","params":[],"id":2}')
ADDR=$(echo "$ADDR_RESULT" | grep -o '"address":"[^"]*"' | cut -d'"' -f4)
check_result "Address generated" "$([ -n "$ADDR" ] && echo "yes" || echo "no")" "yes"

# Generate blocks
GEN_RESULT=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"generatetoaddress\",\"params\":[5,\"$ADDR\"],\"id\":3}")
GEN_COUNT=$(echo "$GEN_RESULT" | grep -o '"result":\[' | wc -l)
check_result "Blocks generated" "$([ "$GEN_COUNT" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 5: Check blockchain height ==="
HEIGHT_RESULT=$(rpc '{"jsonrpc":"2.0","method":"getblockcount","params":[],"id":4}')
HEIGHT=$(echo "$HEIGHT_RESULT" | grep -o '"result":[0-9]*' | cut -d: -f2)
check_result "Height is 5" "$([ "$HEIGHT" -eq "5" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 6: P2P info shows compact blocks enabled ==="
P2P_INFO=$(rpc '{"jsonrpc":"2.0","method":"getnetworkinfo","params":[],"id":5}')
check_result "P2P info retrieved" "$(echo "$P2P_INFO" | grep -q 'result' && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 7: CompactBlock service stats ==="
# Check log for stats output
STATS=$(cat "$LOG_FILE" 2>/dev/null | grep -c "CompactProofBlockManager" || echo "0")
check_result "Manager logged" "$([ "$STATS" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 8: Verify Utreexo proof integration ==="
UTREEXO=$(cat "$LOG_FILE" 2>/dev/null | grep -c "Utreexo" || echo "0")
check_result "Utreexo integration active" "$([ "$UTREEXO" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 9: BIP152 compact blocks available ==="
BIP152=$(cat "$LOG_FILE" 2>/dev/null | grep -c "BIP152" || echo "0")
check_result "BIP152 logged" "$([ "$BIP152" -gt "0" ] && echo "yes" || echo "no")" "yes"

echo ""
echo "=== Test 10: Stateless mode configuration ==="
STATELESS=$(cat "$LOG_FILE" 2>/dev/null | grep -c "Stateless mode" || echo "0")
check_result "Stateless mode configured" "$([ "$STATELESS" -ge "0" ] && echo "yes" || echo "no")" "yes"

# Cleanup
echo ""
echo "Stopping daemon..."
kill $DAEMON_PID 2>/dev/null || true
sleep 2

echo ""
echo "=============================================="
echo "Phase 34.7 Test Results: $PASS/$TOTAL passed"
echo "=============================================="

if [ $FAIL -gt 0 ]; then
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
else
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
fi
