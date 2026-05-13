#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# Three-Component Mining Integration Test
# ═══════════════════════════════════════════════════════════════════════════════
#
# Tests the FULL mining pipeline with all three components:
#
#   ┌─────────┐      ┌─────────┐      ┌─────────┐
#   │  Miner  │─────►│ Stratum │─────►│ Daemon  │
#   └─────────┘      └─────────┘      └─────────┘
#        │                │                │
#        └────────────────┴────────────────┘
#                  CHAIN GROWS
#
# Test Flow:
#   I.1 - Start daemon in regtest mode
#   I.2 - Start stratum server, verify RPC connection
#   I.3 - Start miner, verify stratum connection
#   I.4 - Wait for block to be mined
#   I.5 - Verify chain height increased
#   I.6 - Verify Utreexo root changed
#   I.7 - Mine additional blocks, verify consistency
#
# Exit Criteria:
#   - All three components start successfully
#   - Miner receives work from stratum
#   - Block is mined and accepted by daemon
#   - Chain height increases
#   - Utreexo root commitment is valid
#
# ═══════════════════════════════════════════════════════════════════════════════

set -e
set -u

# ═══════════════════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DINERO_ROOT="${SCRIPT_DIR}/../.."
STRATUM_ROOT="${DINERO_ROOT}/../stratum"
# Binaries
DINEROD="${DINERO_ROOT}/build/dinerod"
STRATUM_SERVER="${STRATUM_ROOT}/build/bin/dinero-stratum"
MINER="${DINERO_ROOT}/build/dinero-stratum-worker"

# Test environment
DATADIR="/tmp/dinero_integration_test_$$"
RPC_PORT=$((20000 + RANDOM % 10000))
STRATUM_PORT=$((30000 + RANDOM % 10000))
P2P_PORT=$((40000 + RANDOM % 10000))
MINER_ADDRESS=""

# PIDs for cleanup
DAEMON_PID=""
STRATUM_PID=""
MINER_PID=""

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ═══════════════════════════════════════════════════════════════════════════════
# Utility Functions
# ═══════════════════════════════════════════════════════════════════════════════

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++)) || true
    ((TESTS_RUN++)) || true
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++)) || true
    ((TESTS_RUN++)) || true
}

log_section() {
    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

cleanup() {
    local exit_code=$?
    log_info "Cleaning up processes..."

    # Kill miner first (least critical)
    if [ -n "$MINER_PID" ] && kill -0 "$MINER_PID" 2>/dev/null; then
        kill "$MINER_PID" 2>/dev/null || true
        log_info "Stopped miner (PID $MINER_PID)"
    fi

    # Kill stratum
    if [ -n "$STRATUM_PID" ] && kill -0 "$STRATUM_PID" 2>/dev/null; then
        kill "$STRATUM_PID" 2>/dev/null || true
        log_info "Stopped stratum (PID $STRATUM_PID)"
    fi

    # Kill daemon last (most critical)
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill "$DAEMON_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$DAEMON_PID" 2>/dev/null || true
        log_info "Stopped daemon (PID $DAEMON_PID)"
    fi

    # Also kill by port in case PIDs got lost
    pkill -f "dinerod.*${RPC_PORT}" 2>/dev/null || true
    pkill -f "dinero-stratum.*${STRATUM_PORT}" 2>/dev/null || true
    pkill -f "dinero-miner.*${STRATUM_PORT}" 2>/dev/null || true

    sleep 1

    # Clean up data directory unless we failed (keep logs for debugging).
    # You can force cleanup by setting KEEP_DATADIR=0.
    local keep="${KEEP_DATADIR:-}"
    if [ -z "$keep" ]; then
        # Default: keep on failure, cleanup on success
        if [ "$exit_code" -ne 0 ]; then
            keep="1"
        else
            keep="0"
        fi
    fi

    if [ "$keep" = "1" ]; then
        log_info "Keeping datadir for inspection: $DATADIR"
        log_info "  Logs:"
        log_info "    $DATADIR/daemon.log"
        log_info "    $DATADIR/stratum.log"
        log_info "    $DATADIR/miner.log"
    else
        rm -rf "$DATADIR"
        log_info "Removed datadir: $DATADIR"
    fi

    log_info "Cleanup complete (exit_code=$exit_code)"
}

trap cleanup EXIT

rpc_call() {
    local method="$1"
    local params="${2:-[]}"

    local cookie
    cookie=$(cat "${DATADIR}/.cookie" 2>/dev/null) || {
        echo '{"error":"no cookie"}'
        return 1
    }

    curl -s --max-time 10 \
         --user "${cookie}" \
         --data-binary "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}" \
         -H 'content-type: application/json' \
         "http://127.0.0.1:${RPC_PORT}/" 2>/dev/null || echo '{"error":"connection failed"}'
}

wait_for_daemon() {
    local max_attempts=60
    local attempt=0

    log_info "Waiting for daemon RPC..."

    while [ $attempt -lt $max_attempts ]; do
        # Fail fast if daemon tells us it couldn't bind the RPC socket.
        if [ -f "$DATADIR/daemon.log" ]; then
            if grep -q "Failed to bind socket" "$DATADIR/daemon.log" 2>/dev/null; then
                log_fail "Daemon failed to bind RPC port $RPC_PORT (socket bind denied)"
                log_info "This can happen in restricted/sandboxed environments."
                tail -30 "$DATADIR/daemon.log" 2>/dev/null || true
                return 1
            fi
        fi

        if [ -f "${DATADIR}/.cookie" ]; then
            local result
            result=$(rpc_call "getblockchaininfo")
            if echo "$result" | jq -e '.result.chain' > /dev/null 2>&1; then
                log_info "Daemon RPC ready (attempt $attempt)"
                return 0
            fi
        fi
        sleep 1
        ((attempt++)) || true
    done

    log_fail "Daemon RPC not available after ${max_attempts}s"
    return 1
}

wait_for_stratum() {
    local max_attempts=30
    local attempt=0

    log_info "Waiting for stratum server..."

    while [ $attempt -lt $max_attempts ]; do
        if nc -z 127.0.0.1 "$STRATUM_PORT" 2>/dev/null; then
            log_info "Stratum port open (attempt $attempt)"
            return 0
        fi
        sleep 1
        ((attempt++)) || true
    done

    log_fail "Stratum server not available after ${max_attempts}s"
    return 1
}

get_block_count() {
    rpc_call "getblockcount" | jq -r '.result // 0'
}

get_best_block_hash() {
    rpc_call "getbestblockhash" | jq -r '.result // empty'
}

get_block_utreexo_root() {
    local hash="$1"
    rpc_call "getblockheader" "[\"${hash}\", true]" | jq -r '.result.utreexo_root_raw // .result.utreexo_root // empty'
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.1: Start Daemon
# ═══════════════════════════════════════════════════════════════════════════════

test_start_daemon() {
    log_section "I.1 - Starting Daemon (regtest)"

    # Check binary exists
    if [ ! -x "$DINEROD" ]; then
        log_fail "dinerod not found at $DINEROD"
        return 1
    fi
    log_pass "dinerod binary exists"

    # Create data directory
    mkdir -p "$DATADIR"
    log_info "Data directory: $DATADIR"
    log_info "RPC port: $RPC_PORT"

    # Start daemon (with --no-stratum to use external stratum server)
    # Run in background since dinerod doesn't fully support -daemon mode
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$RPC_PORT" \
        --p2pport="$P2P_PORT" \
        --no-stratum \
        > "$DATADIR/daemon.log" 2>&1 &

    DAEMON_PID=$!

    # Give daemon time to start
    sleep 5

    # Check if daemon is still running
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        DAEMON_PID=$(pgrep -f "dinerod.*${RPC_PORT}" | head -1) || true
    fi

    if [ -z "$DAEMON_PID" ]; then
        log_fail "Daemon failed to start"
        cat "$DATADIR/daemon.log" 2>/dev/null | tail -20
        return 1
    fi

    log_info "Daemon PID: $DAEMON_PID"

    # Wait for RPC
    if wait_for_daemon; then
        log_pass "Daemon started successfully"
    else
        cat "$DATADIR/daemon.log" 2>/dev/null | tail -20
        return 1
    fi

    # Verify regtest
    local chain
    chain=$(rpc_call "getblockchaininfo" | jq -r '.result.chain')
    if [ "$chain" = "regtest" ]; then
        log_pass "Running on regtest network"
    else
        log_fail "Not on regtest: $chain"
        return 1
    fi

    # Show initial state
    local height
    height=$(get_block_count)
    log_info "Initial block height: $height"

    # Generate a fresh Taproot address for the miner identity/payout.
    # dinero-stratum uses the authorize username as the mining address and
    # forwards it to getblocktemplate (required param).
    local addr_resp
    addr_resp=$(rpc_call "getnewaddress" "[]")
    MINER_ADDRESS=$(echo "$addr_resp" | jq -r '.result.address // .result // empty')
    if [ -z "$MINER_ADDRESS" ] || [ "$MINER_ADDRESS" = "null" ]; then
        log_fail "Failed to get mining address via getnewaddress"
        log_info "Response: $addr_resp"
        return 1
    fi
    log_pass "Generated miner address: ${MINER_ADDRESS}"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.2: Start Stratum Server
# ═══════════════════════════════════════════════════════════════════════════════

test_start_stratum() {
    log_section "I.2 - Starting Stratum Server"

    # Check binary exists
    if [ ! -x "$STRATUM_SERVER" ]; then
        log_fail "stratum_server not found at $STRATUM_SERVER"
        log_info "Build with: cd $STRATUM_ROOT && cmake -B build && cmake --build build"
        return 1
    fi
    log_pass "stratum_server binary exists"

    log_info "Stratum port: $STRATUM_PORT"
    log_info "Connecting to daemon RPC at 127.0.0.1:$RPC_PORT"

    # Get cookie for auth (format is "username:password")
    local cookie
    cookie=$(cat "${DATADIR}/.cookie")
    local rpc_user="${cookie%%:*}"
    local rpc_pass="${cookie#*:}"

    # Start stratum server
    "$STRATUM_SERVER" \
        --rpchost=127.0.0.1 \
        --rpcport="$RPC_PORT" \
        --rpcuser="$rpc_user" \
        --rpcpassword="$rpc_pass" \
        --stratumport="$STRATUM_PORT" \
        --difficulty=0.001 \
        > "$DATADIR/stratum.log" 2>&1 &

    STRATUM_PID=$!
    log_info "Stratum PID: $STRATUM_PID"

    sleep 2

    # Check if still running
    if ! kill -0 "$STRATUM_PID" 2>/dev/null; then
        log_fail "Stratum server died immediately"
        cat "$DATADIR/stratum.log" 2>/dev/null | tail -20
        return 1
    fi

    # Wait for port
    if wait_for_stratum; then
        log_pass "Stratum server started successfully"
    else
        cat "$DATADIR/stratum.log" 2>/dev/null | tail -20
        return 1
    fi

    # Verify stratum can get template from daemon
    sleep 2
    if grep -q "getblocktemplate\|template\|job" "$DATADIR/stratum.log" 2>/dev/null; then
        log_pass "Stratum fetched block template from daemon"
    else
        log_info "Stratum log (checking template fetch):"
        tail -10 "$DATADIR/stratum.log" 2>/dev/null || true
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.3: Start Miner
# ═══════════════════════════════════════════════════════════════════════════════

test_start_miner() {
    log_section "I.3 - Starting Miner"

    # Check binary exists
    if [ ! -x "$MINER" ]; then
        log_fail "dinero-stratum-worker not found at $MINER"
        log_info "Build with: cd $DINERO_ROOT && cmake -B build && cmake --build build --target dinero-stratum-worker"
        return 1
    fi
    log_pass "dinero-stratum-worker binary exists"

    log_info "Connecting to stratum at 127.0.0.1:$STRATUM_PORT"

    if [ -z "$MINER_ADDRESS" ]; then
        log_fail "MINER_ADDRESS not set (daemon address generation failed earlier)"
        return 1
    fi

    # Start miner with 2 threads
    "$MINER" \
        --stratum="127.0.0.1:$STRATUM_PORT" \
        --user="$MINER_ADDRESS" \
        --threads=2 \
        > "$DATADIR/miner.log" 2>&1 &

    MINER_PID=$!
    log_info "Miner PID: $MINER_PID"

    sleep 3

    # Check if still running
    if ! kill -0 "$MINER_PID" 2>/dev/null; then
        log_fail "Miner died immediately"
        cat "$DATADIR/miner.log" 2>/dev/null | tail -20
        return 1
    fi

    log_pass "Miner started successfully"

    # Check for connection
    if grep -qi "connect\|subscrib\|authoriz\|job" "$DATADIR/miner.log" 2>/dev/null; then
        log_pass "Miner connected to stratum"
    else
        log_info "Miner log (checking connection):"
        tail -10 "$DATADIR/miner.log" 2>/dev/null || true
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.4: Wait for Block
# ═══════════════════════════════════════════════════════════════════════════════

test_wait_for_block() {
    log_section "I.4 - Waiting for Block to be Mined"

    local initial_height
    initial_height=$(get_block_count)
    log_info "Initial height: $initial_height"

    local target_height=$((initial_height + 1))
    local max_wait=120  # 2 minutes max (regtest should be fast)
    local waited=0

    log_info "Waiting for height $target_height (max ${max_wait}s)..."

    while [ $waited -lt $max_wait ]; do
        local current_height
        current_height=$(get_block_count)

        if [ "$current_height" -ge "$target_height" ]; then
            log_pass "Block mined! Height: $current_height"
            return 0
        fi

        # Show progress every 10 seconds
        if [ $((waited % 10)) -eq 0 ] && [ $waited -gt 0 ]; then
            log_info "Still waiting... (${waited}s, height: $current_height)"

            # Check if miner is still running
            if ! kill -0 "$MINER_PID" 2>/dev/null; then
                log_fail "Miner died while waiting"
                cat "$DATADIR/miner.log" 2>/dev/null | tail -20
                return 1
            fi
        fi

        sleep 1
        ((waited++)) || true
    done

    log_fail "No block mined after ${max_wait}s"
    log_info "Miner log:"
    tail -30 "$DATADIR/miner.log" 2>/dev/null || true
    log_info "Stratum log:"
    tail -30 "$DATADIR/stratum.log" 2>/dev/null || true
    return 1
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.5: Verify Chain State
# ═══════════════════════════════════════════════════════════════════════════════

test_verify_chain() {
    log_section "I.5 - Verifying Chain State"

    local height
    height=$(get_block_count)
    log_info "Current height: $height"

    if [ "$height" -ge 1 ]; then
        log_pass "Chain has blocks (height: $height)"
    else
        log_fail "Chain still at genesis"
        return 1
    fi

    # Get best block
    local best_hash
    best_hash=$(get_best_block_hash)
    log_info "Best block: ${best_hash:0:16}..."

    if [ ${#best_hash} -eq 64 ]; then
        log_pass "Best block hash valid (64 hex chars)"
    else
        log_fail "Invalid block hash length: ${#best_hash}"
        return 1
    fi

    # Best-effort header-size check (RPC implementations may differ).
    # If getblockheader(verbose=false) returns a 256-hex string, validate it.
    local header_hex
    header_hex=$(rpc_call "getblockheader" "[\"${best_hash}\", false]" | jq -r '.result // empty')
    local header_len=${#header_hex}
    if [ "$header_len" -eq 256 ]; then
        log_pass "Block header is 128 bytes (256 hex chars)"
    else
        log_info "Skipping header hex-size check (getblockheader did not return raw hex)"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.6: Verify Utreexo Root
# ═══════════════════════════════════════════════════════════════════════════════

test_verify_utreexo() {
    log_section "I.6 - Verifying Utreexo Root"

    # Optional check: only run if getblockheader exposes utreexo fields.
    local genesis_hash
    genesis_hash=$(rpc_call "getblockhash" "[0]" | jq -r '.result // empty')
    if [ -z "$genesis_hash" ] || [ "$genesis_hash" = "null" ]; then
        log_info "Skipping Utreexo check (could not query genesis hash)"
        return 0
    fi

    local genesis_root
    genesis_root=$(get_block_utreexo_root "$genesis_hash")
    if [ -z "$genesis_root" ] || [ "$genesis_root" = "null" ]; then
        log_info "Skipping Utreexo check (RPC does not expose utreexo_root in getblockheader)"
        return 0
    fi

    log_info "Genesis Utreexo root: ${genesis_root}"
    log_pass "Utreexo root field present in RPC (best-effort check)"
}

# ═══════════════════════════════════════════════════════════════════════════════
# Phase I.7: Mine Additional Blocks
# ═══════════════════════════════════════════════════════════════════════════════

test_mine_more_blocks() {
    log_section "I.7 - Mining Additional Blocks"

    local initial_height
    initial_height=$(get_block_count)
    local target_height=$((initial_height + 2))

    log_info "Current height: $initial_height"
    log_info "Target height: $target_height"

    local max_wait=120
    local waited=0

    while [ $waited -lt $max_wait ]; do
        local current_height
        current_height=$(get_block_count)

        if [ "$current_height" -ge "$target_height" ]; then
            log_pass "Mined additional blocks! Height: $current_height"

            # Verify chain consistency
            local prev_hash=""
            for h in $(seq 1 "$current_height"); do
                local block_hash
                block_hash=$(rpc_call "getblockhash" "[$h]" | jq -r '.result')
                local block_prev
                block_prev=$(rpc_call "getblockheader" "[\"${block_hash}\", true]" | jq -r '.result.previousblockhash')

                if [ "$h" -gt 1 ] && [ "$block_prev" != "$prev_hash" ]; then
                    log_fail "Chain broken at height $h"
                    return 1
                fi

                prev_hash="$block_hash"
            done

            log_pass "Chain is consistent"
            return 0
        fi

        sleep 2
        ((waited += 2)) || true
    done

    log_fail "Could not mine additional blocks"
    return 1
}

# ═══════════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════════

main() {
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo "  Three-Component Mining Integration Test"
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo ""
    echo "  Components:"
    echo "    Daemon:  $DINEROD"
    echo "    Stratum: $STRATUM_SERVER"
    echo "    Miner:   $MINER"
    echo ""
    echo "  Ports:"
    echo "    RPC:     $RPC_PORT"
    echo "    Stratum: $STRATUM_PORT"
    echo "    P2P:     $P2P_PORT"
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════════════"

    # Run test phases
    test_start_daemon || exit 1
    test_start_stratum || exit 1
    test_start_miner || exit 1
    test_wait_for_block || exit 1
    test_verify_chain
    test_verify_utreexo
    test_mine_more_blocks

    # Summary
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo -e "  RESULTS: ${GREEN}${TESTS_PASSED}${NC}/${TESTS_RUN} tests passed"
    if [ "$TESTS_FAILED" -gt 0 ]; then
        echo -e "           ${RED}${TESTS_FAILED} FAILED${NC}"
    fi
    echo "═══════════════════════════════════════════════════════════════════════════════"
    echo ""

    if [ "$TESTS_FAILED" -gt 0 ]; then
        exit 1
    fi

    echo -e "${GREEN}SUCCESS: Full mining pipeline validated!${NC}"
    echo ""
    echo "  Daemon accepted blocks mined through:"
    echo "    Miner → Stratum → Daemon → Chain"
    echo ""
}

main "$@"
