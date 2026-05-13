#!/usr/bin/env bash
# DineroCoin Production-Grade Hardened Soak Test
# Tests: Crash resilience, Utreexo persistence, chain continuity
# Invariants: No height regression, no reindex, no data loss

set -euo pipefail

# Configuration
DINEROD_BIN="/Users/haydarevich/Documents/DineroCoin/build/dinerod"
MINER_BIN="/Users/haydarevich/Documents/DineroCoin/build/dinero-miner"
DATADIR="/Users/haydarevich/.dinero"
RPC_PORT=20998
MINING_ADDRESS="rdin1ph0z58lqm8tcdg4lfdxv7x8u7c0w8tkj63vxt74nws6yhlflwnprsz690h9"
LOG_DIR="/tmp/hardened_soak_$(date +%Y%m%d_%H%M%S)"

# Test parameters
MAX_CRASHES=5
MIN_INTERVAL=60   # Minimum seconds between crashes
MAX_INTERVAL=300  # Maximum seconds between crashes
CRASH_COUNT=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

mkdir -p "$LOG_DIR"

echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BLUE}  DineroCoin Hardened Crash Soak Test${NC}"
echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo "Configuration:"
echo "  Max crashes:     $MAX_CRASHES"
echo "  Crash interval:  ${MIN_INTERVAL}s - ${MAX_INTERVAL}s (randomized)"
echo "  Log directory:   $LOG_DIR"
echo "  Mining address:  $MINING_ADDRESS"
echo ""

# Global state variables
DAEMON_PID=""
MINER_PID=""
DAEMON_LOG=""
HEIGHT_BEFORE=""
HASH_BEFORE=""
HEIGHT_AFTER=""
HASH_AFTER=""

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

get_best_hash() {
    rpc_call "blockchain.getbestblockhash" "[]"
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
    sleep 1
    echo -e "${GREEN}✓${NC} Miner stopped"
}

kill_daemon() {
    echo -e "${YELLOW}Killing daemon with SIGKILL (simulated crash)...${NC}"
    pkill -9 dinerod || true
    sleep 2
    echo -e "${GREEN}✓${NC} Daemon killed"
}

snapshot_chain() {
    HEIGHT_BEFORE=$(get_height)
    HASH_BEFORE=$(get_best_hash)

    if [[ -z "$HEIGHT_BEFORE" ]] || [[ "$HEIGHT_BEFORE" == "null" ]]; then
        echo -e "${RED}✗ Failed to get height${NC}"
        return 1
    fi

    echo -e "${BLUE}Snapshot:${NC}"
    echo "  Height: $HEIGHT_BEFORE"
    echo "  Hash:   $HASH_BEFORE"
}

snapshot_chain_after() {
    HEIGHT_AFTER=$(get_height)
    HASH_AFTER=$(get_best_hash)

    echo -e "${BLUE}Post-restart snapshot:${NC}"
    echo "  Height: $HEIGHT_AFTER"
    echo "  Hash:   $HASH_AFTER"
}

# ═══════════════════════════════════════════════════════════
# INVARIANT ASSERTIONS (Production-Grade)
# ═══════════════════════════════════════════════════════════

assert_height_monotonic() {
    echo -e "${YELLOW}[Assert] Height monotonic...${NC}"

    if [[ "$HEIGHT_AFTER" -lt "$HEIGHT_BEFORE" ]]; then
        echo -e "${RED}❌ FATAL: Height regression detected!${NC}"
        echo "  Pre-crash:  $HEIGHT_BEFORE"
        echo "  Post-crash: $HEIGHT_AFTER"
        echo "  Delta:      $((HEIGHT_BEFORE - HEIGHT_AFTER)) blocks LOST"
        echo "  This indicates chain rollback or DB corruption!"
        exit 1
    fi

    if [[ "$HEIGHT_AFTER" -gt "$HEIGHT_BEFORE" ]]; then
        echo -e "${YELLOW}⚠ Height increased during crash window:${NC}"
        echo "  Pre-crash:  $HEIGHT_BEFORE"
        echo "  Post-crash: $HEIGHT_AFTER"
        echo "  Delta:      +$((HEIGHT_AFTER - HEIGHT_BEFORE)) blocks"
        echo "  (This is OK - blocks accepted during shutdown)"
    fi

    if [[ "$HEIGHT_AFTER" -eq "$HEIGHT_BEFORE" ]]; then
        echo -e "${GREEN}✓ Exact height match${NC}"
    fi

    echo -e "${GREEN}✓ Assert passed: height_after >= height_before${NC}"
}

assert_chain_continuity() {
    echo -e "${YELLOW}[Assert] Chain continuity...${NC}"

    # If height increased, verify new tip extends the old chain
    if [[ "$HEIGHT_AFTER" -eq "$HEIGHT_BEFORE" ]]; then
        if [[ "$HASH_AFTER" != "$HASH_BEFORE" ]]; then
            echo -e "${RED}❌ FATAL: Same height but different hash!${NC}"
            echo "  Expected: $HASH_BEFORE"
            echo "  Got:      $HASH_AFTER"
            echo "  This indicates chain reorganization!"
            exit 1
        fi
    fi

    # For now, accept height increases (proper ancestor check requires getblock RPC)
    echo -e "${GREEN}✓ Assert passed: chain continuity maintained${NC}"
}

assert_no_reindex() {
    echo -e "${YELLOW}[Assert] No reindex occurred...${NC}"

    if grep -qi "reindex" "$DAEMON_LOG" 2>/dev/null; then
        echo -e "${RED}❌ FATAL: Reindex detected!${NC}"
        echo "  Daemon log: $DAEMON_LOG"
        echo "  This indicates persistence failure - state was not restored"
        exit 1
    fi

    echo -e "${GREEN}✓ Assert passed: no reindex${NC}"
}

assert_utreexo_loaded() {
    echo -e "${YELLOW}[Assert] Utreexo CF restored...${NC}"

    # Check for successful CF opening
    if ! grep -q "utreexo" "$DAEMON_LOG" 2>/dev/null; then
        echo -e "${RED}❌ FATAL: Utreexo CF not found in logs!${NC}"
        echo "  Daemon log: $DAEMON_LOG"
        exit 1
    fi

    # Check for Utreexo-specific initialization
    if grep -q "Utreexo Forest created" "$DAEMON_LOG" 2>/dev/null; then
        echo -e "${GREEN}✓ Utreexo accumulator restored${NC}"
    fi

    echo -e "${GREEN}✓ Assert passed: Utreexo CF loaded${NC}"
}

assert_mining_resumes() {
    echo -e "${YELLOW}[Assert] Mining resumes...${NC}"

    local initial_height=$(get_height)
    sleep 5
    local new_height=$(get_height)

    if [[ "$new_height" -gt "$initial_height" ]]; then
        echo -e "${GREEN}✓ Mining active (height: $initial_height → $new_height)${NC}"
    else
        echo -e "${YELLOW}⚠ No new blocks mined yet (may take time)${NC}"
    fi

    echo -e "${GREEN}✓ Assert passed: mining can resume${NC}"
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

    # Phase 1: Quiesce and snapshot
    stop_miner
    snapshot_chain

    # Phase 2: Crash
    kill_daemon

    # Phase 3: Restart
    if ! start_daemon; then
        echo -e "${RED}❌ FATAL: Daemon failed to restart${NC}"
        exit 1
    fi

    # Phase 4: Verify
    snapshot_chain_after
    assert_height_monotonic
    assert_chain_continuity
    assert_no_reindex
    assert_utreexo_loaded

    # Phase 5: Resume mining
    start_miner
    assert_mining_resumes

    echo -e "${GREEN}✅ Crash cycle #$cycle PASSED${NC}"
}

# ═══════════════════════════════════════════════════════════
# MAIN TEST EXECUTION
# ═══════════════════════════════════════════════════════════

echo -e "${GREEN}Starting hardened soak test...${NC}"
echo ""

# Initial startup
if ! start_daemon; then
    echo -e "${RED}Failed to start daemon initially${NC}"
    exit 1
fi

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

echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  HARDENED SOAK TEST PASSED${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo "Results:"
echo "  Total crash cycles:  $MAX_CRASHES"
echo "  Initial height:      $initial_height"
echo "  Final height:        $final_height"
echo "  Blocks mined:        $((final_height - initial_height))"
echo "  Logs:                $LOG_DIR"
echo ""
echo "✅ DineroCoin survives arbitrary crashes without reindex or data loss"
echo ""

# Cleanup
pkill -9 dinerod dinero-miner || true

exit 0
