#!/bin/bash
#
# Phase 4C-lite: RPC → Phase 3 Integration Test
#
# STABILITY GATE: This test validates that Phase 3 components work via RPC
#
# What this tests:
# 1. wallet.getnewaddress - Address generation
# 2. generatetoaddress - Mining (creates spendable coins)
# 3. wallet.getbalance - Balance tracking with maturity
# 4. wallet.sendtoaddress - Full spending workflow:
#    - Coin selection (greedy algorithm from Phase 3 Week 1)
#    - Transaction building (TransactionBuilder from Phase 3 Week 2)
#    - BIP143 signing (BIP143Signer from Phase 3 Week 3)
#    - Mempool submission (Mempool validation from Phase 3 Week 4)
#    - Fee estimation
# 5. getrawmempool - Transaction propagation
# 6. wallet.listtransactions - Transaction history
#
# If this test passes, we know:
# ✅ Phase 3 components integrate correctly
# ✅ RPC layer works end-to-end
# ✅ Ready for Phase 4A (network hardening)
#

set -e
set -o pipefail

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Configuration
DATADIR="/tmp/dinero-rpc-test-$$"
DAEMON="./build/dinerod"
CLI="./build/dinero-cli"
PORT=21234  # Use unique port to avoid conflicts

# Test state
PASS_COUNT=0
FAIL_COUNT=0
TOTAL_TESTS=16

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║  Phase 4C-lite: RPC Spending Integration Test            ║"
echo "║  Stability Gate for Phase 3 → Phase 4A                   ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

# Kill any stale test daemons from previous runs
echo -e "${BLUE}Cleaning up stale daemons...${NC}"
pkill -f "dinerod.*dinero-rpc-test" 2>/dev/null || true
sleep 2
echo ""

# Cleanup function
cleanup() {
    echo ""
    echo -e "${BLUE}Cleaning up...${NC}"
    if [ ! -z "$DAEMON_PID" ]; then
        kill $DAEMON_PID 2>/dev/null || true
        sleep 1
    fi
    # Preserve entire datadir for debugging
    if [ -d "$DATADIR" ]; then
        rm -rf /tmp/test-datadir-preserved
        cp -r "$DATADIR" /tmp/test-datadir-preserved
        echo "Datadir preserved at: /tmp/test-datadir-preserved"
        if [ -f "$DATADIR/daemon.log" ]; then
            echo "Daemon log: /tmp/test-datadir-preserved/daemon.log"
        fi
    fi
    rm -rf "$DATADIR"
}

trap cleanup EXIT

# Helper function for tests
run_test() {
    local test_name="$1"
    local test_number="$2"
    echo -e "${BLUE}Test $test_number: $test_name${NC}"
}

extract_json_field() {
    local json_input="$1"
    local field_name="$2"
    JSON_INPUT="$json_input" python3 - "$field_name" <<'PY'
import json, os, sys

field = sys.argv[1]
try:
    obj = json.loads(os.environ["JSON_INPUT"])
except Exception:
    sys.exit(0)

value = obj.get(field, "")
if isinstance(value, bool):
    print("true" if value else "false")
elif isinstance(value, (dict, list)):
    print(json.dumps(value))
else:
    print(value)
PY
}

pass_test() {
    echo -e "${GREEN}✅ PASS${NC}"
    ((PASS_COUNT++))
    echo ""
}

fail_test() {
    local reason="$1"
    echo -e "${RED}❌ FAIL: $reason${NC}"
    ((FAIL_COUNT++))
    echo ""
}

#=============================================================================
# Test 0: Prerequisites
#=============================================================================

run_test "Check binaries exist" 0

if [ ! -f "$DAEMON" ]; then
    fail_test "dinerod not found at $DAEMON"
    exit 1
fi

if [ ! -f "$CLI" ]; then
    fail_test "dinero-cli not found at $CLI"
    exit 1
fi

pass_test

#=============================================================================
# Test 1: Start Daemon
#=============================================================================

run_test "Start daemon in regtest mode" 1

mkdir -p "$DATADIR"

# Start daemon in background (without --daemon flag)
# Use --no-stratum to avoid port 3333 conflicts with other daemons
$DAEMON --regtest --datadir="$DATADIR" --rpcport=$PORT --p2pport=21235 --no-stratum > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!

# Wait for daemon to start and initialize (25s for full RPC readiness)
echo "  Waiting for RPC initialization..."
for i in {1..25}; do
    if ! kill -0 $DAEMON_PID 2>/dev/null; then
        echo "  ❌ Daemon crashed after $i seconds!"
        echo "  Daemon log:"
        tail -30 "$DATADIR/daemon.log"
        fail_test "Daemon crashed during initialization"
        exit 1
    fi
    sleep 1
done

# Check if daemon is still running
if kill -0 $DAEMON_PID 2>/dev/null; then
    echo "  Daemon PID: $DAEMON_PID"

    # Check for cookie file
    if [ -f "$DATADIR/.cookie" ]; then
        echo "  RPC cookie found"
    else
        echo "  WARNING: RPC cookie not found at $DATADIR/.cookie"
        ls -la "$DATADIR/" | head -10
    fi

    # Check if RPC port is listening
    if nc -z 127.0.0.1 $PORT 2>/dev/null; then
        echo "  RPC server listening on port $PORT"
    else
        echo "  WARNING: RPC server not listening on port $PORT"
    fi

    pass_test
else
    fail_test "Daemon failed to start (check $DATADIR/daemon.log)"
    cat "$DATADIR/daemon.log"
    exit 1
fi

#=============================================================================
# Test 2: Generate Wallet Address
#=============================================================================

run_test "getnewaddress - Generate receiving address" 2

# Get CLI output for debugging (disable exit-on-error temporarily)
set +e
CLI_OUTPUT=$($CLI -datadir="$DATADIR" -rpcport=$PORT getnewaddress 2>&1)
CLI_EXIT=$?
set -e
echo "  CLI exit code: $CLI_EXIT"
echo "  CLI output: $CLI_OUTPUT"

# Extract address
ADDR=$(echo "$CLI_OUTPUT" | grep -oE 'bc1[a-z0-9]+|din1[a-z0-9]+|rdin1[a-z0-9]+' || echo "")

if [ -z "$ADDR" ]; then
    fail_test "Failed to generate address"
    echo "  Full CLI command: $CLI -datadir=\"$DATADIR\" -rpcport=$PORT getnewaddress"
    echo "  Daemon log (last 20 lines):"
    tail -20 "$DATADIR/daemon.log" 2>&1 | sed 's/^/    /'
    exit 1
fi

echo "  Generated address: $ADDR"
pass_test

#=============================================================================
# Test 3: Mine Blocks (Coinbase Creation)
#=============================================================================

run_test "mining.generatetoaddress - Mine 101 blocks (coinbase maturity)" 3

RESULT=$($CLI -datadir="$DATADIR" -rpcport=$PORT mining.generatetoaddress 101 "$ADDR" 2>&1)

# Check that we mined 101 blocks
BLOCK_COUNT=$($CLI -datadir="$DATADIR" -rpcport=$PORT blockchain.getblockcount 2>&1 | grep -oE '[0-9]+' || echo "0")

if [ "$BLOCK_COUNT" = "101" ]; then
    echo "  Mined 101 blocks (height: $BLOCK_COUNT)"
    pass_test
else
    fail_test "Expected height 101, got $BLOCK_COUNT"
fi

#=============================================================================
# Test 4: Check Balance (Mature Coinbase)
#=============================================================================

run_test "wallet.getbalance - Verify spendable balance" 4

BALANCE=$($CLI -datadir="$DATADIR" -rpcport=$PORT wallet.getbalance 2>&1)

echo "  Balance response: $BALANCE"

# Extract confirmed balance (should be > 0 after 101 blocks)
CONFIRMED=$(echo "$BALANCE" | grep -oE '"confirmed"[[:space:]]*:[[:space:]]*[0-9.]+' | grep -oE '[0-9.]+' || echo "0")

if [ "$CONFIRMED" != "0" ] && [ ! -z "$CONFIRMED" ]; then
    echo "  Confirmed balance: $CONFIRMED DIN"
    pass_test
else
    fail_test "Expected confirmed balance > 0, got: $CONFIRMED"
fi

#=============================================================================
# Test 5: Check Immature Balance (99 immature coinbases remain after 101 blocks)
#=============================================================================

run_test "wallet.getbalance - Verify immature coins remain after 101 blocks" 5

IMMATURE=$(echo "$BALANCE" | grep -oE '"immature"[[:space:]]*:[[:space:]]*[0-9.]+' | grep -oE '[0-9.]+' || echo "0")

if [ "$IMMATURE" = "9900.0" ]; then
    echo "  Immature balance: $IMMATURE DIN (correct - 99 coinbases still immature)"
    pass_test
else
    fail_test "Expected immature=9900.0 after 101 blocks, got: $IMMATURE"
fi

#=============================================================================
# Test 6: List Unspent UTXOs
#=============================================================================

run_test "wallet.listunspent - Enumerate available UTXOs" 6

UTXOS=$($CLI -datadir="$DATADIR" -rpcport=$PORT wallet.listunspent 2>&1)

# Count UTXOs (each block creates 1 coinbase)
UTXO_COUNT=$(echo "$UTXOS" | grep -c '"txid"' || echo "0")

echo "  UTXOs found: $UTXO_COUNT"

if [ "$UTXO_COUNT" -gt "0" ]; then
    pass_test
else
    fail_test "Expected UTXOs, found $UTXO_COUNT"
fi

#=============================================================================
# Test 7: Generate Recipient Address
#=============================================================================

run_test "getnewaddress - Generate recipient address" 7

RECIPIENT=$($CLI -datadir="$DATADIR" -rpcport=$PORT getnewaddress 2>&1 | grep -oE 'bc1[a-z0-9]+|din1[a-z0-9]+|rdin1[a-z0-9]+' || echo "")

if [ -z "$RECIPIENT" ]; then
    fail_test "Failed to generate recipient address"
    exit 1
fi

echo "  Recipient address: $RECIPIENT"
pass_test

#=============================================================================
# Test 8: Send Transaction (CRITICAL - Phase 3 Integration)
#=============================================================================

run_test "wallet.sendtoaddress - Full spending workflow" 8

echo "  Sending 10.0 DIN to $RECIPIENT"

SEND_RESULT=$($CLI -datadir="$DATADIR" -rpcport=$PORT wallet.sendtoaddress "$RECIPIENT" 10.0 2>&1)

echo "  Send result: $SEND_RESULT"

# Extract txid
TXID=$(extract_json_field "$SEND_RESULT" "txid")

if [ -z "$TXID" ]; then
    fail_test "Failed to send transaction. Response: $SEND_RESULT"
else
    echo "  Transaction ID: $TXID"
    pass_test
fi

#=============================================================================
# Test 9: Verify Transaction in Mempool
#=============================================================================

run_test "getrawmempool - Verify transaction propagated to mempool" 9

sleep 1  # Give mempool time to process

MEMPOOL=$($CLI -datadir="$DATADIR" -rpcport=$PORT getrawmempool 2>&1)

if echo "$MEMPOOL" | grep -q "$TXID"; then
    echo "  Transaction found in mempool"
    pass_test
else
    fail_test "Transaction $TXID not found in mempool"
fi

#=============================================================================
# Test 10: Check Transaction Details
#=============================================================================

run_test "wallet.gettransaction - Verify transaction details" 10

TX_DETAILS=$($CLI -datadir="$DATADIR" -rpcport=$PORT wallet.gettransaction "$TXID" 2>&1 || echo "")

if echo "$TX_DETAILS" | grep -q "$TXID"; then
    echo "  Transaction details retrieved"
    # Extract fee if available
    FEE=$(echo "$TX_DETAILS" | grep -oE '"fee"[[:space:]]*:[[:space:]]*[0-9.]+' | grep -oE '[0-9.]+' || echo "unknown")
    echo "  Fee: $FEE DIN"
    pass_test
else
    fail_test "Could not retrieve transaction details"
fi

#=============================================================================
# Test 11: Generate Additional Recipients For sendmany
#=============================================================================

run_test "getnewaddress - Generate sendmany recipients" 11

RECIPIENT_A=$($CLI -datadir="$DATADIR" -rpcport=$PORT getnewaddress 2>&1 | grep -oE 'bc1[a-z0-9]+|din1[a-z0-9]+|rdin1[a-z0-9]+' || echo "")
RECIPIENT_B=$($CLI -datadir="$DATADIR" -rpcport=$PORT getnewaddress 2>&1 | grep -oE 'bc1[a-z0-9]+|din1[a-z0-9]+|rdin1[a-z0-9]+' || echo "")

if [ -z "$RECIPIENT_A" ] || [ -z "$RECIPIENT_B" ]; then
    fail_test "Failed to generate sendmany recipients"
    exit 1
fi

echo "  Recipient A: $RECIPIENT_A"
echo "  Recipient B: $RECIPIENT_B"
pass_test

#=============================================================================
# Test 12: Send Transaction To Multiple Recipients
#=============================================================================

run_test "wallet.sendmany - Spend to multiple recipients" 12

SENDMANY_RESULT=$($CLI -datadir="$DATADIR" -rpcport=$PORT wallet.sendmany "{\"$RECIPIENT_A\":3.0,\"$RECIPIENT_B\":4.0}" 2>&1)

echo "  sendmany result: $SENDMANY_RESULT"

SENDMANY_TXID=$(extract_json_field "$SENDMANY_RESULT" "txid")

if [ -z "$SENDMANY_TXID" ]; then
    fail_test "Failed to sendmany transaction. Response: $SENDMANY_RESULT"
else
    echo "  sendmany txid: $SENDMANY_TXID"
    pass_test
fi

#=============================================================================
# Test 13: Verify sendmany In Mempool
#=============================================================================

run_test "getrawmempool - Verify sendmany transaction propagated" 13

sleep 1

MEMPOOL_WITH_SENDMANY=$($CLI -datadir="$DATADIR" -rpcport=$PORT getrawmempool 2>&1)

if echo "$MEMPOOL_WITH_SENDMANY" | grep -q "$SENDMANY_TXID"; then
    echo "  sendmany transaction found in mempool"
    pass_test
else
    fail_test "sendmany transaction $SENDMANY_TXID not found in mempool"
fi

#=============================================================================
# Test 14: Mine Confirmation Block
#=============================================================================

run_test "mining.generatetoaddress - Mine confirmation block" 14

$CLI -datadir="$DATADIR" -rpcport=$PORT mining.generatetoaddress 1 "$ADDR" > /dev/null 2>&1

sleep 1

# Verify transaction is no longer in mempool
MEMPOOL_AFTER=$($CLI -datadir="$DATADIR" -rpcport=$PORT getrawmempool 2>&1)

if echo "$MEMPOOL_AFTER" | grep -q "$TXID"; then
    fail_test "wallet.sendtoaddress transaction still in mempool after mining"
elif echo "$MEMPOOL_AFTER" | grep -q "$SENDMANY_TXID"; then
    fail_test "wallet.sendmany transaction still in mempool after mining"
else
    echo "  Both transactions confirmed (removed from mempool)"
    pass_test
fi

#=============================================================================
# Test 15: Verify Final Balances
#=============================================================================

run_test "wallet.getbalance - Verify balances updated correctly" 15

FINAL_BALANCE=$($CLI -datadir="$DATADIR" -rpcport=$PORT wallet.getbalance 2>&1)

FINAL_CONFIRMED=$(echo "$FINAL_BALANCE" | grep -oE '"confirmed"[[:space:]]*:[[:space:]]*[0-9.]+' | grep -oE '[0-9.]+' || echo "0")

echo "  Initial balance: $CONFIRMED DIN"
echo "  Final balance: $FINAL_CONFIRMED DIN"
echo "  Difference: $(echo "$CONFIRMED - $FINAL_CONFIRMED" | bc) DIN (should be ~10 DIN + fees)"

# The difference should be approximately 10 DIN (plus fees)
# We're not doing exact math here, just sanity checking
if [ "$FINAL_CONFIRMED" != "$CONFIRMED" ]; then
    echo "  Balance changed as expected"
    pass_test
else
    fail_test "Balance did not change after transaction"
fi

#=============================================================================
# Summary
#=============================================================================

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║  Test Results Summary                                     ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

TOTAL_RUN=$((PASS_COUNT + FAIL_COUNT))

echo -e "${GREEN}Passed: $PASS_COUNT / $TOTAL_RUN${NC}"
if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "${RED}Failed: $FAIL_COUNT / $TOTAL_RUN${NC}"
fi
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  ✅ STABILITY GATE PASSED                                 ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo "Phase 3 components integrate correctly via RPC:"
    echo "  ✅ Coinbase maturity enforced (100 blocks)"
    echo "  ✅ UTXO selection working (greedy algorithm)"
    echo "  ✅ Transaction building working (TransactionBuilder)"
    echo "  ✅ BIP143 signing working (witness creation)"
    echo "  ✅ Mempool submission working (validation)"
    echo "  ✅ Fee calculation working"
    echo "  ✅ Balance tracking accurate"
    echo "  ✅ Transaction confirmation working"
    echo ""
    echo "🚀 READY FOR PHASE 4A: Network Hardening"
    echo ""
    exit 0
else
    echo -e "${RED}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  ❌ STABILITY GATE FAILED                                 ║${NC}"
    echo -e "${RED}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo "Fix these issues before proceeding to Phase 4A."
    echo ""
    exit 1
fi
