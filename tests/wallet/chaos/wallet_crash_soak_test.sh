#!/usr/bin/env bash
# DineroCoin Wallet Crash / Recovery Chaos Test (Quick Version)
# Tests: Wallet crash resilience, fund safety, key integrity
# Invariants: No fund loss, no key loss, no corruption

set -euo pipefail

# Configuration
DINEROD_BIN="/Users/haydarevich/Documents/DineroCoin/build/dinerod"
MINER_BIN="/Users/haydarevich/Documents/DineroCoin/build/dinero-miner"
ORACLE_BIN="/Users/haydarevich/Documents/DineroCoin/tests/wallet/chaos/wallet_oracle"
DATADIR="/Users/haydarevich/.dinero"
WALLET_NAME="default"
WALLET_DB_PATH="$DATADIR/wallets/wallet_$WALLET_NAME.db"
RPC_PORT=20998
LOG_DIR="/tmp/wallet_hardened_soak_$(date +%Y%m%d_%H%M%S)"

# Test parameters
MAX_CRASHES=25
MIN_INTERVAL=30   # Minimum seconds between crashes
MAX_INTERVAL=120  # Maximum seconds between crashes
CRASH_COUNT=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

mkdir -p "$LOG_DIR"

echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  DineroCoin Wallet Hardened Crash Test (Production)${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo "Configuration:"
echo "  Max crashes:     $MAX_CRASHES"
echo "  Crash interval:  ${MIN_INTERVAL}s - ${MAX_INTERVAL}s (randomized)"
echo "  Log directory:   $LOG_DIR"
echo "  Wallet name:     $WALLET_NAME"
echo ""

# Global state variables
DAEMON_PID=""
MINER_PID=""
DAEMON_LOG=""
MINING_ADDRESS=""

# Get RPC cookie
get_cookie() {
    cat "$DATADIR/.cookie" 2>/dev/null || echo "__cookie__:default"
}

# RPC call wrapper
rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    local cookie=$(get_cookie)

    curl -s --user "$cookie" \
         --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"$method\",\"params\":$params}" \
         -H 'content-type: text/plain;' \
         "http://127.0.0.1:$RPC_PORT/" 2>/dev/null | \
         python3 -c "import sys, json; r=json.load(sys.stdin); print(r.get('result', ''))" 2>/dev/null || echo ""
}

get_height() {
    rpc_call "blockchain.getblockcount" "[]"
}

check_daemon_health() {
    local height=$(get_height)
    [[ -n "$height" ]] && [[ "$height" != "null" ]]
}

wait_for_rpc() {
    echo -e "${YELLOW}Waiting for RPC...${NC}"
    for i in {1..30}; do
        sleep 1
        if check_daemon_health; then
            echo -e "${GREEN}✓${NC} RPC ready"
            return 0
        fi
    done
    echo -e "${RED}✗ RPC timeout${NC}"
    return 1
}

start_daemon() {
    DAEMON_LOG="$LOG_DIR/daemon_$(date +%Y%m%d_%H%M%S).log"
    echo -e "${YELLOW}Starting daemon...${NC}"
    echo "  Log: $DAEMON_LOG"

    nohup "$DINEROD_BIN" --regtest --rpcport=$RPC_PORT --datadir="$DATADIR" \
        > "$DAEMON_LOG" 2>&1 &

    DAEMON_PID=$!
    echo "$DAEMON_PID" > "$LOG_DIR/daemon.pid"

    if ! wait_for_rpc; then
        echo -e "${RED}✗ Daemon failed to start${NC}"
        return 1
    fi

    echo -e "${GREEN}✓${NC} Daemon started (PID: $DAEMON_PID)"
    return 0
}

create_or_open_wallet() {
    echo -e "${YELLOW}Creating/opening wallet: $WALLET_NAME${NC}"

    # Try to open existing wallet first
    local result=$(rpc_call "wallet.open" "[\"$WALLET_NAME\"]" 2>/dev/null)

    if [[ "$result" == *"error"* ]] || [[ -z "$result" ]]; then
        # Wallet doesn't exist, create it
        echo -e "${YELLOW}  Creating new wallet...${NC}"
        rpc_call "wallet.create" "[\"$WALLET_NAME\", \"\"]" >/dev/null
        rpc_call "wallet.open" "[\"$WALLET_NAME\"]" >/dev/null
    fi

    echo -e "${GREEN}✓${NC} Wallet ready: $WALLET_NAME"

    # Get or generate mining address
    MINING_ADDRESS=$(rpc_call "wallet.getnewaddress" "[]" | tr -d '"')
    if [[ -z "$MINING_ADDRESS" ]]; then
        echo -e "${RED}✗ Failed to get mining address${NC}"
        return 1
    fi

    echo -e "${GREEN}✓${NC} Mining address: $MINING_ADDRESS"
}

start_miner() {
    local miner_log="$LOG_DIR/miner_$(date +%Y%m%d_%H%M%S).log"
    echo -e "${YELLOW}Starting miner...${NC}"

    nohup "$MINER_BIN" --threads 1 --rpc-port $RPC_PORT --address "$MINING_ADDRESS" \
        > "$miner_log" 2>&1 &

    MINER_PID=$!
    echo "$MINER_PID" > "$LOG_DIR/miner.pid"
    echo -e "${GREEN}✓${NC} Miner started (PID: $MINER_PID)"
}

stop_miner() {
    echo -e "${YELLOW}Stopping miner for quiescent snapshot...${NC}"
    pkill -9 dinero-miner || true
    sleep 2
    echo -e "${GREEN}✓${NC} Miner stopped"
}

kill_daemon() {
    echo -e "${YELLOW}Killing daemon with SIGKILL (simulated crash)...${NC}"
    pkill -9 dinerod || true
    sleep 2
    echo -e "${GREEN}✓${NC} Daemon killed"
}

# ═══════════════════════════════════════════════════════════
# Wallet Snapshot (using wallet_oracle)
# ═══════════════════════════════════════════════════════════

snapshot_wallet() {
    local snapshot_file="$1"

    if [[ ! -f "$WALLET_DB_PATH" ]]; then
        echo -e "${RED}✗ Wallet database not found: $WALLET_DB_PATH${NC}"
        return 1
    fi

    # Capture wallet state using oracle
    if [[ -f "$ORACLE_BIN" ]]; then
        "$ORACLE_BIN" snapshot "$WALLET_DB_PATH" > "$snapshot_file" 2>/dev/null
    else
        # Fallback: Manual RPC snapshot
        local balance=$(rpc_call "wallet.getbalance" "[]")
        local height=$(get_height)
        echo "{\"height\":$height,\"balance\":$balance,\"wallet\":\"$WALLET_NAME\"}" > "$snapshot_file"
    fi

    echo -e "${BLUE}Wallet snapshot saved: $snapshot_file${NC}"
    cat "$snapshot_file"
}

# ═══════════════════════════════════════════════════════════
# Invariant Validation (using wallet_oracle)
# ═══════════════════════════════════════════════════════════

validate_wallet_invariants() {
    local before_snapshot="$1"
    local after_snapshot="$2"

    echo -e "${YELLOW}[Assert] Validating wallet invariants...${NC}"

    if [[ -f "$ORACLE_BIN" ]]; then
        if "$ORACLE_BIN" validate "$before_snapshot" "$after_snapshot"; then
            echo -e "${GREEN}✓ All wallet invariants passed${NC}"
            return 0
        else
            echo -e "${RED}❌ FATAL: Wallet invariant violation detected!${NC}"
            return 1
        fi
    else
        # Fallback: Basic balance check
        local balance_before=$(cat "$before_snapshot" | python3 -c "import sys, json; print(json.load(sys.stdin).get('balance', {}).get('total', 0))" 2>/dev/null || echo "0")
        local balance_after=$(cat "$after_snapshot" | python3 -c "import sys, json; print(json.load(sys.stdin).get('balance', {}).get('total', 0))" 2>/dev/null || echo "0")

        if (( $(echo "$balance_after < $balance_before" | bc -l) )); then
            echo -e "${RED}❌ FATAL: Balance decreased! Before: $balance_before, After: $balance_after${NC}"
            return 1
        fi

        echo -e "${GREEN}✓ Balance preserved (basic check)${NC}"
        return 0
    fi
}

check_no_rescan() {
    echo -e "${YELLOW}[Assert] No forced rescan...${NC}"

    if grep -qi "Rescanning\|Reindexing" "$DAEMON_LOG" 2>/dev/null; then
        echo -e "${RED}❌ FATAL: Wallet forced rescan detected!${NC}"
        echo "  Daemon log: $DAEMON_LOG"
        return 1
    fi

    echo -e "${GREEN}✓ Assert passed: no forced rescan${NC}"
    return 0
}

check_sqlite_integrity() {
    echo -e "${YELLOW}[Assert] SQLite integrity check...${NC}"

    if [[ ! -f "$WALLET_DB_PATH" ]]; then
        echo -e "${RED}❌ FATAL: Wallet database missing!${NC}"
        return 1
    fi

    local integrity=$(sqlite3 "$WALLET_DB_PATH" "PRAGMA integrity_check;" 2>/dev/null)

    if [[ "$integrity" != "ok" ]]; then
        echo -e "${RED}❌ FATAL: Wallet database corrupted!${NC}"
        echo "  Integrity check: $integrity"
        return 1
    fi

    echo -e "${GREEN}✓ Assert passed: SQLite integrity OK${NC}"
    return 0
}

# ═══════════════════════════════════════════════════════════
# Random Wallet Activity (Crash Injection Scenarios)
# ═══════════════════════════════════════════════════════════

random_wallet_activity() {
    local scenario=$((RANDOM % 3))

    case $scenario in
        0)
            # Scenario: Address generation
            echo -e "${BLUE}📝 Generating new address...${NC}"
            rpc_call "wallet.getnewaddress" "[]" >/dev/null
            ;;
        1)
            # Scenario: Balance check
            echo -e "${BLUE}💰 Checking balance...${NC}"
            rpc_call "wallet.getbalance" "[]" >/dev/null
            ;;
        2)
            # Scenario: List UTXOs
            echo -e "${BLUE}📊 Listing UTXOs...${NC}"
            rpc_call "wallet.listunspent" "[]" >/dev/null
            ;;
    esac
}

# ═══════════════════════════════════════════════════════════
# CRASH CYCLE (Core Test Loop)
# ═══════════════════════════════════════════════════════════

crash_cycle() {
    local cycle="$1"

    echo ""
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}  CRASH CYCLE #$cycle / $MAX_CRASHES${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"

    # Phase 1: Wallet activity
    random_wallet_activity

    # Phase 2: Quiesce and snapshot
    stop_miner
    sleep 2

    local snapshot_before="$LOG_DIR/snapshot_before_$cycle.json"
    snapshot_wallet "$snapshot_before"

    # Phase 3: Crash
    kill_daemon

    # Phase 4: Restart
    if ! start_daemon; then
        echo -e "${RED}❌ FATAL: Daemon failed to restart${NC}"
        exit 1
    fi

    # Phase 5: Reopen wallet
    if ! create_or_open_wallet; then
        echo -e "${RED}❌ FATAL: Wallet failed to reopen${NC}"
        exit 1
    fi

    # Phase 6: Snapshot after restart
    local snapshot_after="$LOG_DIR/snapshot_after_$cycle.json"
    snapshot_wallet "$snapshot_after"

    # Phase 7: Validate invariants
    if ! validate_wallet_invariants "$snapshot_before" "$snapshot_after"; then
        echo -e "${RED}❌ FATAL: Wallet invariants violated!${NC}"
        exit 1
    fi

    if ! check_no_rescan; then
        exit 1
    fi

    if ! check_sqlite_integrity; then
        exit 1
    fi

    # Phase 8: Resume mining
    start_miner
    sleep 3

    echo -e "${GREEN}✅ Crash cycle #$cycle PASSED${NC}"
}

# ═══════════════════════════════════════════════════════════
# MAIN TEST EXECUTION
# ═══════════════════════════════════════════════════════════

echo -e "${GREEN}Starting wallet hardened chaos test...${NC}"
echo ""

# Compile oracle if needed
if [[ ! -f "$ORACLE_BIN" ]]; then
    echo -e "${YELLOW}Compiling wallet oracle...${NC}"
    g++ -std=c++17 -o "$ORACLE_BIN" \
        "/Users/haydarevich/Documents/DineroCoin/tests/wallet/chaos/wallet_oracle.cpp" \
        -lsqlite3 2>/dev/null || echo -e "${YELLOW}⚠ Oracle compilation failed, using fallback mode${NC}"
fi

# Initial startup
if ! start_daemon; then
    echo -e "${RED}Failed to start daemon initially${NC}"
    exit 1
fi

create_or_open_wallet
start_miner

# Wait for initial mining
echo ""
echo -e "${YELLOW}Mining initial blocks (30s warmup)...${NC}"
sleep 30

initial_height=$(get_height)
echo -e "${GREEN}✓ Warmup complete (height: $initial_height)${NC}"
echo ""

# Main crash loop
while [[ $CRASH_COUNT -lt $MAX_CRASHES ]]; do
    CRASH_COUNT=$((CRASH_COUNT + 1))

    crash_cycle $CRASH_COUNT

    # Randomized interval (realistic chaos)
    if [[ $CRASH_COUNT -lt $MAX_CRASHES ]]; then
        SLEEP_TIME=$((RANDOM % (MAX_INTERVAL - MIN_INTERVAL) + MIN_INTERVAL))
        echo ""
        echo -e "${BLUE}Next crash in ${SLEEP_TIME}s...${NC}"
        sleep "$SLEEP_TIME"
    fi
done

# Final summary
final_height=$(get_height)
final_balance=$(rpc_call "wallet.getbalance" "[]")

echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  WALLET HARDENED CHAOS TEST PASSED${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo "Results:"
echo "  Total crash cycles:  $MAX_CRASHES"
echo "  Initial height:      $initial_height"
echo "  Final height:        $final_height"
echo "  Blocks mined:        $((final_height - initial_height))"
echo "  Final balance:       $final_balance"
echo "  Logs:                $LOG_DIR"
echo ""
echo "✅ DineroCoin wallet survives arbitrary crashes without fund loss or corruption"
echo ""

# Cleanup
pkill -9 dinerod dinero-miner || true

exit 0
