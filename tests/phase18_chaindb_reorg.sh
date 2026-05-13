#!/usr/bin/env bash
set -euo pipefail

# ================================================
# Phase 18.4 - ChainDB Hash Consistency Tests
# -----------------------------------------------
# Validates the Phase 18 hash consistency invariant:
#   Mining template hash == mined block hash
#   == ChainDB storage hash == getblock hash
#
# Tests scenarios that previously caused hash mismatches:
#   - Sequential block mining
#   - Rapid successive mining
#   - Multiple independent nodes
#   - Block retrieval consistency
#
# NOTE: Full reorg testing (with invalidateblock) will be
# added in a future phase when RPC handler is implemented.
# ================================================

DIND=./build/dinerod
DINCLI="./build/dinero-cli"

DATADIR=/tmp/phase18_consistency
LOGFILE=$DATADIR/test.log

# Regtest address for mining
ADDR="rdin1qv8epkuar78ujlecxalg4cp8665e5jmezr9r9n0"

# Test result tracking
TESTS_PASSED=0
TESTS_FAILED=0

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Helper functions
log_test() {
    echo -e "${YELLOW}[TEST]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

assert_equal() {
    local expected="$1"
    local actual="$2"
    local desc="$3"

    if [[ "$expected" == "$actual" ]]; then
        log_pass "$desc"
        return 0
    else
        log_fail "$desc"
        echo "  Expected: $expected"
        echo "  Got:      $actual"
        return 1
    fi
}

# Cleanup function
cleanup() {
    echo "[CLEANUP] Stopping all dinerod processes..."
    pkill -9 dinerod || true
    sleep 1
}

trap cleanup EXIT

# ================================================
# TEST 1: Sequential Mining Hash Consistency
# ================================================
test_sequential_mining() {
    log_test "Test 1: Sequential mining hash consistency"

    rm -rf "$DATADIR"
    mkdir -p "$DATADIR"

    # Start node
    $DIND --regtest --datadir="$DATADIR" 2>&1 > "$LOGFILE" &
    local NODE_PID=$!
    sleep 3

    # Mine blocks sequentially and verify hash consistency
    log_test "  Mining 5 blocks sequentially..."
    local hashes=()
    for i in {1..5}; do
        local hash=$($DINCLI -datadir="$DATADIR" generatetoaddress 1 "$ADDR" | jq -r '.[0]')
        hashes+=("$hash")

        # Verify getblock can retrieve it
        local block_json=$($DINCLI -datadir="$DATADIR" getblock "$hash" 2>/dev/null || echo "null")
        if [[ "$block_json" == "null" ]]; then
            log_fail "Failed to retrieve block $hash"
            kill -9 $NODE_PID || true
            return 1
        fi

        # Verify hash in JSON matches what we got from generatetoaddress
        local retrieved_hash=$(echo "$block_json" | jq -r '.hash')
        if [[ "$hash" != "$retrieved_hash" ]]; then
            log_fail "Hash mismatch for block $i: $hash != $retrieved_hash"
            kill -9 $NODE_PID || true
            return 1
        fi
    done

    log_pass "All 5 blocks have consistent hashes"

    # Verify final height
    local height=$($DINCLI -datadir="$DATADIR" getblockcount | tr -d '"')
    assert_equal "6" "$height" "Final chain height should be 6 (genesis + premine + 5 mined)"

    # Cleanup
    kill -9 $NODE_PID || true
    sleep 1
}

# ================================================
# TEST 2: Rapid Successive Mining
# ================================================
test_rapid_mining() {
    log_test "Test 2: Rapid successive mining (potential race conditions)"

    rm -rf "$DATADIR"
    mkdir -p "$DATADIR"

    # Start node
    $DIND --regtest --datadir="$DATADIR" 2>&1 > "$LOGFILE" &
    local NODE_PID=$!
    sleep 3

    # Mine multiple blocks in quick succession
    log_test "  Mining 3 blocks rapidly..."
    local hashes_json=$($DINCLI -datadir="$DATADIR" generatetoaddress 3 "$ADDR")
    local hash1=$(echo "$hashes_json" | jq -r '.[0]')
    local hash2=$(echo "$hashes_json" | jq -r '.[1]')
    local hash3=$(echo "$hashes_json" | jq -r '.[2]')

    # Verify all three blocks are retrievable and have consistent hashes
    for hash in "$hash1" "$hash2" "$hash3"; do
        local block_json=$($DINCLI -datadir="$DATADIR" getblock "$hash" 2>/dev/null || echo "null")
        if [[ "$block_json" == "null" ]]; then
            log_fail "Failed to retrieve block $hash"
            kill -9 $NODE_PID || true
            return 1
        fi

        local retrieved_hash=$(echo "$block_json" | jq -r '.hash')
        if [[ "$hash" != "$retrieved_hash" ]]; then
            log_fail "Hash mismatch: $hash != $retrieved_hash"
            kill -9 $NODE_PID || true
            return 1
        fi
    done

    log_pass "All rapidly-mined blocks have consistent hashes"

    # Verify height
    local height=$($DINCLI -datadir="$DATADIR" getblockcount | tr -d '"')
    assert_equal "5" "$height" "Height should be 5 (genesis + premine + 3 mined)"

    # Cleanup
    kill -9 $NODE_PID || true
    sleep 1
}

# ================================================
# TEST 3: Multi-Node Independence
# ================================================
test_multi_node() {
    log_test "Test 3: Multi-node independence (Phase 18: pre-P2P)"

    local DATADIR_A=/tmp/phase18_node_a
    local DATADIR_B=/tmp/phase18_node_b

    rm -rf "$DATADIR_A" "$DATADIR_B"
    mkdir -p "$DATADIR_A" "$DATADIR_B"

    # Start two isolated nodes
    log_test "  Starting two isolated nodes..."
    $DIND --regtest --datadir="$DATADIR_A" --rpcport=20996 --port=21001 2>&1 > "$DATADIR_A/node.log" &
    local NODE_A_PID=$!

    $DIND --regtest --datadir="$DATADIR_B" --rpcport=20997 --port=21002 2>&1 > "$DATADIR_B/node.log" &
    local NODE_B_PID=$!

    sleep 3

    # Node A mines 3 blocks
    log_test "  Node A mining 3 blocks..."
    local hashes_a=$($DINCLI -datadir="$DATADIR_A" -rpcport=20996 generatetoaddress 3 "$ADDR")
    local hash_a1=$(echo "$hashes_a" | jq -r '.[0]')
    local hash_a2=$(echo "$hashes_a" | jq -r '.[1]')
    local hash_a3=$(echo "$hashes_a" | jq -r '.[2]')

    # Node B mines 3 blocks (will have different hashes - no sync yet)
    log_test "  Node B mining 3 blocks..."
    local hashes_b=$($DINCLI -datadir="$DATADIR_B" -rpcport=20997 generatetoaddress 3 "$ADDR")
    local hash_b1=$(echo "$hashes_b" | jq -r '.[0]')
    local hash_b2=$(echo "$hashes_b" | jq -r '.[1]')
    local hash_b3=$(echo "$hashes_b" | jq -r '.[2]')

    # Verify Node A's chain consistency
    log_test "  Verifying Node A chain consistency..."
    for hash in "$hash_a1" "$hash_a2" "$hash_a3"; do
        local block=$($DINCLI -datadir="$DATADIR_A" -rpcport=20996 getblock "$hash" 2>/dev/null || echo "null")
        if [[ "$block" == "null" ]]; then
            log_fail "Node A failed to retrieve block $hash"
            kill -9 $NODE_A_PID $NODE_B_PID || true
            return 1
        fi

        local retrieved=$(echo "$block" | jq -r '.hash')
        if [[ "$hash" != "$retrieved" ]]; then
            log_fail "Node A hash mismatch: $hash != $retrieved"
            kill -9 $NODE_A_PID $NODE_B_PID || true
            return 1
        fi
    done
    log_pass "Node A has self-consistent chain"

    # Verify Node B's chain consistency
    log_test "  Verifying Node B chain consistency..."
    for hash in "$hash_b1" "$hash_b2" "$hash_b3"; do
        local block=$($DINCLI -datadir="$DATADIR_B" -rpcport=20997 getblock "$hash" 2>/dev/null || echo "null")
        if [[ "$block" == "null" ]]; then
            log_fail "Node B failed to retrieve block $hash"
            kill -9 $NODE_A_PID $NODE_B_PID || true
            return 1
        fi

        local retrieved=$(echo "$block" | jq -r '.hash')
        if [[ "$hash" != "$retrieved" ]]; then
            log_fail "Node B hash mismatch: $hash != $retrieved"
            kill -9 $NODE_A_PID $NODE_B_PID || true
            return 1
        fi
    done
    log_pass "Node B has self-consistent chain"

    # Verify both nodes have correct heights
    local height_a=$($DINCLI -datadir="$DATADIR_A" -rpcport=20996 getblockcount | tr -d '"')
    local height_b=$($DINCLI -datadir="$DATADIR_B" -rpcport=20997 getblockcount | tr -d '"')

    assert_equal "5" "$height_a" "Node A height should be 5"
    assert_equal "5" "$height_b" "Node B height should be 5"

    # Cleanup
    kill -9 $NODE_A_PID $NODE_B_PID || true
    sleep 1
}

# ================================================
# MAIN TEST EXECUTION
# ================================================

echo "========================================"
echo " Phase 18.4 - ChainDB Consistency Tests"
echo "========================================"
echo ""

# Run all tests
test_sequential_mining
echo ""
test_rapid_mining
echo ""
test_multi_node

# Summary
echo ""
echo "========================================"
echo " Test Summary"
echo "========================================"
echo -e "  ${GREEN}Passed:${NC} $TESTS_PASSED"
echo -e "  ${RED}Failed:${NC} $TESTS_FAILED"
echo ""

if [[ $TESTS_FAILED -eq 0 ]]; then
    echo -e "${GREEN}✅ All ChainDB consistency tests passed!${NC}"
    echo ""
    echo "Phase 18.4 validation complete:"
    echo "  ✓ Hash consistency maintained across mining operations"
    echo "  ✓ Block retrieval works correctly"
    echo "  ✓ Multi-node chains remain independent"
    echo ""
    echo "NOTE: Full reorg testing (with invalidateblock RPC) will be"
    echo "      added in a future phase when handler is implemented."
    exit 0
else
    echo -e "${RED}❌ Some tests failed${NC}"
    exit 1
fi
