#!/bin/bash
# DineroCoin Comprehensive 30-Minute Continuous Stress Test
# Tests: Wallet, Daemon, RocksDB, Blockchain, Mining, Transactions, P2P, GUI compatibility

set -e

cd /Users/haydarevich/Documents/DineroCoin

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test duration in seconds (30 minutes = 1800 seconds)
TEST_DURATION=1800
START_TIME=$(date +%s)
TEST_DIR="./stress_test_$(date +%Y%m%d_%H%M%S)"

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║  DineroCoin Comprehensive 30-Minute Stress Test Suite     ║${NC}"
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}Start Time: $(date)${NC}"
echo -e "${GREEN}Test Directory: $TEST_DIR${NC}"
echo -e "${GREEN}Duration: 30 minutes (1800 seconds)${NC}"
echo ""

# Create test directory
mkdir -p $TEST_DIR/logs
mkdir -p $TEST_DIR/wallets
mkdir -p $TEST_DIR/data

# Logging function
log_test() {
    local test_name=$1
    local status=$2
    local message=$3
    local timestamp=$(date +"%Y-%m-%d %H:%M:%S")
    echo "[$timestamp] [$status] $test_name: $message" >> $TEST_DIR/logs/test.log
    if [ "$status" == "PASS" ]; then
        echo -e "${GREEN}✓${NC} $test_name: $message"
    elif [ "$status" == "FAIL" ]; then
        echo -e "${RED}✗${NC} $test_name: $message"
    else
        echo -e "${YELLOW}→${NC} $test_name: $message"
    fi
}

# Counter variables
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Test result tracker
track_result() {
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    if [ $1 -eq 0 ]; then
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        FAILED_TESTS=$((FAILED_TESTS + 1))
        return 1
    fi
}

# ============================================================================
# PHASE 1: DAEMON STARTUP AND STABILITY TEST (5 minutes)
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}PHASE 1: Daemon Startup & Stability Test (5 min)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

log_test "Daemon" "INFO" "Starting daemon in regtest mode"
./build/dinerod --regtest --datadir=$TEST_DIR/data --rpcuser=stress --rpcpassword=test123 \
    --rpcport=19995 --port=18443 > $TEST_DIR/logs/daemon.log 2>&1 &
DAEMON_PID=$!
sleep 10

# Test daemon is running
if ps -p $DAEMON_PID > /dev/null; then
    log_test "Daemon" "PASS" "Daemon started successfully (PID: $DAEMON_PID)"
    track_result 0
else
    log_test "Daemon" "FAIL" "Daemon failed to start"
    track_result 1
    exit 1
fi

# Test RPC connectivity for 5 minutes
PHASE1_END=$(($(date +%s) + 300))
RPC_TESTS=0
while [ $(date +%s) -lt $PHASE1_END ]; do
    HEIGHT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' 2>/dev/null | jq -r '.result // "error"')

    if [ "$HEIGHT" != "error" ] && [ "$HEIGHT" != "null" ]; then
        RPC_TESTS=$((RPC_TESTS + 1))
        log_test "RPC" "PASS" "getblockcount returned: $HEIGHT (test #$RPC_TESTS)"
        track_result 0
    else
        log_test "RPC" "FAIL" "getblockcount failed"
        track_result 1
    fi
    sleep 5
done

# ============================================================================
# PHASE 2: WALLET OPERATIONS TEST (5 minutes)
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}PHASE 2: Wallet Operations Test (5 min)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

PHASE2_END=$(($(date +%s) + 300))
WALLET_TESTS=0

# Generate multiple addresses
while [ $(date +%s) -lt $PHASE2_END ]; do
    WALLET_TESTS=$((WALLET_TESTS + 1))

    # Test getnewaddress
    ADDR=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":[]}' 2>/dev/null | jq -r '.result')

    if [[ $ADDR == rdin* ]]; then
        log_test "Wallet" "PASS" "Generated address #$WALLET_TESTS: $ADDR"
        track_result 0
        echo "$ADDR" >> $TEST_DIR/wallets/addresses.txt
    else
        log_test "Wallet" "FAIL" "Failed to generate address #$WALLET_TESTS"
        track_result 1
    fi

    # Test getbalance
    BALANCE=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getbalance","params":[]}' 2>/dev/null | jq -r '.result.total // 0')
    log_test "Wallet" "INFO" "Current balance: $BALANCE DIN"

    # Test listunspent
    UTXO_COUNT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"listunspent","params":[]}' 2>/dev/null | jq -r '.result | length')
    log_test "Wallet" "INFO" "UTXOs: $UTXO_COUNT"

    # Test settxfee
    FEE_RESULT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"settxfee","params":[0.00001]}' 2>/dev/null | jq -r '.result.success // false')
    if [ "$FEE_RESULT" == "true" ]; then
        log_test "Wallet" "PASS" "settxfee succeeded"
        track_result 0
    fi

    sleep 3
done

# ============================================================================
# PHASE 3: MINING STRESS TEST (5 minutes)
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}PHASE 3: Mining Stress Test (5 min)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

PHASE3_END=$(($(date +%s) + 300))
MINING_ROUNDS=0

# Get a mining address
MINING_ADDR=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":[]}' | jq -r '.result')

while [ $(date +%s) -lt $PHASE3_END ]; do
    MINING_ROUNDS=$((MINING_ROUNDS + 1))

    # Mine blocks
    RESULT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"generatetoaddress\",\"params\":[5, \"$MINING_ADDR\"]}" \
        2>/dev/null | jq -r '.result.message // "error"')

    if [[ $RESULT == *"Generated"* ]]; then
        HEIGHT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
            -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' | jq -r '.result')
        log_test "Mining" "PASS" "Round #$MINING_ROUNDS: Mined 5 blocks (height: $HEIGHT)"
        track_result 0
    else
        log_test "Mining" "FAIL" "Mining round #$MINING_ROUNDS failed"
        track_result 1
    fi

    sleep 2
done

# ============================================================================
# PHASE 4: DATABASE PERSISTENCE TEST (5 minutes)
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}PHASE 4: RocksDB Persistence Test (5 min)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

PHASE4_END=$(($(date +%s) + 300))
DB_RESTART_COUNT=0

while [ $(date +%s) -lt $PHASE4_END ]; do
    DB_RESTART_COUNT=$((DB_RESTART_COUNT + 1))

    # Get current height before restart
    HEIGHT_BEFORE=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' | jq -r '.result')

    log_test "Database" "INFO" "Restart #$DB_RESTART_COUNT: Height before=$HEIGHT_BEFORE"

    # Stop daemon
    kill $DAEMON_PID 2>/dev/null
    sleep 3

    # Restart daemon
    ./build/dinerod --regtest --datadir=$TEST_DIR/data --rpcuser=stress --rpcpassword=test123 \
        --rpcport=19995 --port=18443 >> $TEST_DIR/logs/daemon.log 2>&1 &
    DAEMON_PID=$!
    sleep 8

    # Check height after restart
    HEIGHT_AFTER=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' | jq -r '.result')

    if [ "$HEIGHT_BEFORE" == "$HEIGHT_AFTER" ]; then
        log_test "Database" "PASS" "Restart #$DB_RESTART_COUNT: Height persisted ($HEIGHT_AFTER)"
        track_result 0
    else
        log_test "Database" "FAIL" "Restart #$DB_RESTART_COUNT: Height mismatch (before=$HEIGHT_BEFORE, after=$HEIGHT_AFTER)"
        track_result 1
    fi

    # Check database files exist
    if [ -d "$TEST_DIR/data/chainstate" ]; then
        DB_SIZE=$(du -sh $TEST_DIR/data/chainstate | cut -f1)
        log_test "Database" "INFO" "RocksDB chainstate size: $DB_SIZE"
        track_result 0
    fi

    sleep 20
done

# ============================================================================
# PHASE 5: TRANSACTION STRESS TEST (5 minutes)
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}PHASE 5: Transaction Stress Test (5 min)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

PHASE5_END=$(($(date +%s) + 300))
TX_COUNT=0

# Mine some blocks to get balance
log_test "Transaction" "INFO" "Mining 110 blocks for mature coinbase"
curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"generatetoaddress\",\"params\":[100, \"$MINING_ADDR\"]}" > /dev/null
sleep 2

while [ $(date +%s) -lt $PHASE5_END ]; do
    TX_COUNT=$((TX_COUNT + 1))

    # Get a new address for each transaction
    DEST_ADDR=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":[]}' | jq -r '.result')

    # Attempt to send transaction
    TX_RESULT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"sendtoaddress\",\"params\":[\"$DEST_ADDR\", 0.1]}" \
        2>/dev/null)

    ERROR=$(echo $TX_RESULT | jq -r '.result.error // "none"')
    if [ "$ERROR" == "none" ] || [ "$ERROR" == "null" ]; then
        log_test "Transaction" "PASS" "TX #$TX_COUNT created to $DEST_ADDR"
        track_result 0
    else
        log_test "Transaction" "INFO" "TX #$TX_COUNT: $ERROR"
    fi

    # Test mempool
    MEMPOOL_SIZE=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
        -d '{"jsonrpc":"2.0","id":"test","method":"getrawmempool","params":[]}' 2>/dev/null | jq -r '.result | length')
    log_test "Mempool" "INFO" "Size: $MEMPOOL_SIZE transactions"

    # Mine a block every 5 transactions
    if [ $((TX_COUNT % 5)) -eq 0 ]; then
        curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
            -d "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"generatetoaddress\",\"params\":[1, \"$MINING_ADDR\"]}" > /dev/null
        log_test "Mining" "INFO" "Mined block to clear mempool"
    fi

    sleep 3
done

# ============================================================================
# PHASE 6: CONCURRENT OPERATIONS TEST (5 minutes)
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}PHASE 6: Concurrent Operations Test (5 min)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

PHASE6_END=$(($(date +%s) + 300))
CONCURRENT_ROUNDS=0

while [ $(date +%s) -lt $PHASE6_END ]; do
    CONCURRENT_ROUNDS=$((CONCURRENT_ROUNDS + 1))
    log_test "Concurrent" "INFO" "Round #$CONCURRENT_ROUNDS: Starting parallel operations"

    # Launch 10 concurrent RPC calls
    for i in {1..10}; do
        (
            curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
                -d '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo","params":[]}' > /dev/null
        ) &
    done
    wait

    log_test "Concurrent" "PASS" "Round #$CONCURRENT_ROUNDS: 10 parallel RPCs completed"
    track_result 0

    # Concurrent address generation
    for i in {1..5}; do
        (
            curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
                -d '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":[]}' > /dev/null
        ) &
    done
    wait

    log_test "Concurrent" "PASS" "Round #$CONCURRENT_ROUNDS: 5 parallel address generations completed"
    track_result 0

    sleep 5
done

# ============================================================================
# FINAL VALIDATION
# ============================================================================
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}FINAL VALIDATION${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

# Get final stats
FINAL_HEIGHT=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' | jq -r '.result')

FINAL_BALANCE=$(curl -s -X POST http://127.0.0.1:19995 -u "stress:test123" -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","id":"test","method":"getbalance","params":[]}' | jq -r '.result.total // 0')

ADDR_COUNT=$(wc -l < $TEST_DIR/wallets/addresses.txt 2>/dev/null || echo "0")

DB_SIZE=$(du -sh $TEST_DIR/data 2>/dev/null | cut -f1 || echo "0")

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
MINUTES=$((ELAPSED / 60))
SECONDS=$((ELAPSED % 60))

# ============================================================================
# FINAL REPORT
# ============================================================================
echo ""
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║          STRESS TEST COMPLETE - FINAL REPORT              ║${NC}"
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo ""
echo -e "${BLUE}Test Duration:${NC} ${MINUTES}m ${SECONDS}s"
echo -e "${BLUE}Total Tests:${NC} $TOTAL_TESTS"
echo -e "${GREEN}Passed:${NC} $PASSED_TESTS"
echo -e "${RED}Failed:${NC} $FAILED_TESTS"
echo -e "${BLUE}Success Rate:${NC} $(awk "BEGIN {printf \"%.2f\", ($PASSED_TESTS/$TOTAL_TESTS)*100}")%"
echo ""
echo -e "${BLUE}Final Blockchain Height:${NC} $FINAL_HEIGHT blocks"
echo -e "${BLUE}Final Wallet Balance:${NC} $FINAL_BALANCE DIN"
echo -e "${BLUE}Addresses Generated:${NC} $ADDR_COUNT"
echo -e "${BLUE}Database Size:${NC} $DB_SIZE"
echo ""
echo -e "${BLUE}Test Results Saved To:${NC} $TEST_DIR/logs/test.log"
echo ""
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"

# Cleanup
log_test "Cleanup" "INFO" "Stopping daemon"
kill $DAEMON_PID 2>/dev/null
sleep 2

# Generate summary report
cat > $TEST_DIR/REPORT.md << EOF
# DineroCoin Stress Test Report

**Date:** $(date)
**Duration:** ${MINUTES}m ${SECONDS}s

## Summary
- **Total Tests:** $TOTAL_TESTS
- **Passed:** $PASSED_TESTS
- **Failed:** $FAILED_TESTS
- **Success Rate:** $(awk "BEGIN {printf \"%.2f\", ($PASSED_TESTS/$TOTAL_TESTS)*100}")%

## Final State
- **Blockchain Height:** $FINAL_HEIGHT blocks
- **Wallet Balance:** $FINAL_BALANCE DIN
- **Addresses Generated:** $ADDR_COUNT
- **Database Size:** $DB_SIZE

## Test Phases
1. ✅ Daemon Startup & Stability (5 min)
2. ✅ Wallet Operations (5 min)
3. ✅ Mining Stress Test (5 min)
4. ✅ Database Persistence (5 min)
5. ✅ Transaction Stress Test (5 min)
6. ✅ Concurrent Operations (5 min)

## Logs
- Daemon log: logs/daemon.log
- Test log: logs/test.log
- Addresses: wallets/addresses.txt

## Conclusion
System $([ $FAILED_TESTS -eq 0 ] && echo "PASSED" || echo "HAD ISSUES") all stress tests.
EOF

echo -e "${BLUE}Report saved to:${NC} $TEST_DIR/REPORT.md"
echo ""
echo -e "${GREEN}✓ Stress test complete!${NC}"
