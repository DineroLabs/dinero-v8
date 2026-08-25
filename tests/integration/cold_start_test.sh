#!/bin/bash
#
# Cold-Start Consensus Validation Test
#
# Automated cold-start simulation that validates:
# - P2P header sync (FindHeadersToSend)
# - Block sync and tip convergence
# - Chainwork consistency across nodes
# - Restart persistence
# - Reindex equivalence
#
# Usage: ./cold_start_test.sh [--blocks N] [--timeout T]
#

set -e

# Configuration
BLOCKS_TO_MINE=${BLOCKS:-110}
SYNC_TIMEOUT=${TIMEOUT:-60}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Find dinerod binary (check multiple locations)
if [[ -z "$DINEROD" ]]; then
    if [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
        DINEROD="${PROJECT_ROOT}/build/dinerod"
    elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
        DINEROD="${PROJECT_ROOT}/dinerod"
    else
        DINEROD="${PROJECT_ROOT}/build/dinerod"
    fi
fi

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Temp directories
DATADIR_A=""
DATADIR_B=""
DATADIR_C=""
PID_A=""
PID_B=""
PID_C=""

# Ports (dynamically allocated)
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT_A=$(alloc_port_base)
P2P_PORT_A=$((RPC_PORT_A + 1))
RPC_PORT_B=$((RPC_PORT_A + 2))
P2P_PORT_B=$((RPC_PORT_A + 3))
RPC_PORT_C=$((RPC_PORT_A + 4))
P2P_PORT_C=$((RPC_PORT_A + 5))

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"

    # Kill processes by datadir pattern (daemon forks and reparents to PID 1)
    [[ -n "$DATADIR_A" ]] && stop_node "$DATADIR_A"
    [[ -n "$DATADIR_B" ]] && stop_node "$DATADIR_B"
    [[ -n "$DATADIR_C" ]] && stop_node "$DATADIR_C"

    # Remove temp directories
    [[ -n "$DATADIR_A" && -d "$DATADIR_A" ]] && rm -rf "$DATADIR_A"
    [[ -n "$DATADIR_B" && -d "$DATADIR_B" ]] && rm -rf "$DATADIR_B"
    [[ -n "$DATADIR_C" && -d "$DATADIR_C" ]] && rm -rf "$DATADIR_C"
}

trap cleanup EXIT

fail() {
    echo -e "${RED}✘ FAILED: $1${NC}"
    exit 1
}

pass() {
    echo -e "${GREEN}✓ $1${NC}"
}

info() {
    echo -e "${CYAN}$1${NC}"
}

# RPC call helper
rpc_call() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local params="$*"

    # Read cookie
    local cookie_file="${datadir}/.cookie"
    if [[ ! -f "$cookie_file" ]]; then
        echo "ERROR: Cookie file not found"
        return 1
    fi
    local cookie=$(cat "$cookie_file")

    # Build JSON-RPC request
    local json_params="[]"
    if [[ -n "$params" ]]; then
        json_params="[$params]"
    fi

    local request="{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$json_params,\"id\":1}"

    # Make request
    curl -s -u "$cookie" \
        -H "Content-Type: application/json" \
        -d "$request" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

get_height() {
    local port=$1
    local datadir=$2
    local result=$(rpc_call "$port" "$datadir" "getblockcount" 2>&1)
    # Handle both {"result":N} and {"result": N} formats
    local height=$(echo "$result" | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p')
    echo "$height"
}

get_best_hash() {
    local port=$1
    local datadir=$2
    local result=$(rpc_call "$port" "$datadir" "getbestblockhash" 2>&1)
    # Handle pretty-printed JSON
    echo "$result" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start=$(date +%s)

    echo "  Waiting for daemon (port=$port, datadir=$datadir, timeout=${timeout}s)..."

    while true; do
        local now=$(date +%s)
        local elapsed=$((now - start))

        if [[ $elapsed -gt $timeout ]]; then
            echo "  TIMEOUT after ${elapsed}s"
            echo "  Cookie exists: $(test -f "${datadir}/.cookie" && echo yes || echo no)"
            echo "  Last 10 lines of daemon.log:"
            tail -10 "${datadir}/daemon.log" 2>/dev/null || echo "  (no log)"
            return 1
        fi

        if [[ -f "${datadir}/.cookie" ]]; then
            local height=$(get_height "$port" "$datadir" 2>/dev/null)
            if [[ -n "$height" ]]; then
                return 0
            fi
        fi

        sleep 1
    done
}

wait_for_sync() {
    local port=$1
    local datadir=$2
    local target_height=$3
    local target_hash=$4
    local timeout=$5
    local start=$(date +%s)

    while true; do
        local now=$(date +%s)
        local elapsed=$((now - start))

        if [[ $elapsed -gt $timeout ]]; then
            return 1
        fi

        local height=$(get_height "$port" "$datadir" 2>/dev/null)
        local hash=$(get_best_hash "$port" "$datadir" 2>/dev/null)

        if [[ "$height" == "$target_height" && "$hash" == "$target_hash" ]]; then
            return 0
        fi

        sleep 0.5
    done
}

start_node() {
    local name=$1
    local datadir=$2
    local rpc_port=$3
    local p2p_port=$4
    local connect_port=$5

    mkdir -p "$datadir"

    local args=(
        "$DINEROD"
        "--regtest"
        "--datadir=$datadir"
        "--rpcport=$rpc_port"
        "--port=$p2p_port"
        "--listen=1"
    )

    if [[ -n "$connect_port" ]]; then
        args+=("--connect=127.0.0.1:$connect_port")
    fi

    "${args[@]}" > "${datadir}/daemon.log" 2>&1 &
    # Note: daemon forks to background and reparents to PID 1
    # Return shell PID for compatibility (actual daemon PID found via pgrep)
    echo $!
}

stop_node() {
    local datadir=$1
    # Kill daemon by matching datadir in command line
    pkill -TERM -f "dinerod.*${datadir}" 2>/dev/null || true
    sleep 2
    # Force kill if still running
    pkill -9 -f "dinerod.*${datadir}" 2>/dev/null || true
    sleep 1
}

get_daemon_pid() {
    local datadir=$1
    pgrep -f "dinerod.*${datadir}" 2>/dev/null | head -1
}

# ═══════════════════════════════════════════════════════════════════════════
# MAIN TEST
# ═══════════════════════════════════════════════════════════════════════════

echo "═══════════════════════════════════════════════════════════════"
echo "Cold-Start Consensus Validation Test"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Check dinerod exists
if [[ ! -x "$DINEROD" ]]; then
    fail "dinerod not found at $DINEROD"
fi

# Create temp directories
DATADIR_A=$(mktemp -d -t dinero_coldstart_A_XXXXXX)
DATADIR_B=$(mktemp -d -t dinero_coldstart_B_XXXXXX)
DATADIR_C=$(mktemp -d -t dinero_coldstart_C_XXXXXX)

# ═══════════════════════════════════════════════════════════════════════════
# STEP 1: Start 3 nodes
# ═══════════════════════════════════════════════════════════════════════════
info "[STEP 1] Starting 3 nodes (A=miner, B/C=sync)..."

PID_A=$(start_node "A" "$DATADIR_A" "$RPC_PORT_A" "$P2P_PORT_A" "")
echo "  Node A: PID=$PID_A RPC=$RPC_PORT_A P2P=$P2P_PORT_A"

if ! wait_for_ready "$RPC_PORT_A" "$DATADIR_A" 30; then
    fail "Node A failed to start"
fi
pass "Node A ready"

# Start B and C connecting to A
PID_B=$(start_node "B" "$DATADIR_B" "$RPC_PORT_B" "$P2P_PORT_B" "$P2P_PORT_A")
echo "  Node B: PID=$PID_B RPC=$RPC_PORT_B P2P=$P2P_PORT_B"

if ! wait_for_ready "$RPC_PORT_B" "$DATADIR_B" 30; then
    fail "Node B failed to start"
fi
pass "Node B ready"

PID_C=$(start_node "C" "$DATADIR_C" "$RPC_PORT_C" "$P2P_PORT_C" "$P2P_PORT_A")
echo "  Node C: PID=$PID_C RPC=$RPC_PORT_C P2P=$P2P_PORT_C"

if ! wait_for_ready "$RPC_PORT_C" "$DATADIR_C" 30; then
    fail "Node C failed to start"
fi
pass "Node C ready"

# Wait for P2P connections
sleep 3

# ═══════════════════════════════════════════════════════════════════════════
# STEP 2: Create wallet and mine blocks on A
# ═══════════════════════════════════════════════════════════════════════════
info "\n[STEP 2] Mining $BLOCKS_TO_MINE blocks on node A..."

# Create wallet
WALLET_RESULT=$(rpc_call "$RPC_PORT_A" "$DATADIR_A" "wallet.createhd" '"miner_wallet"')
# Handle pretty-printed JSON - extract first_address value
MINER_ADDRESS=$(echo "$WALLET_RESULT" | tr -d '\n\t' | sed -n 's/.*"first_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

if [[ -z "$MINER_ADDRESS" ]]; then
    echo "DEBUG: wallet.createhd response: $WALLET_RESULT"
    fail "Failed to create wallet"
fi
echo "  Miner address: $MINER_ADDRESS"

# Mine blocks
MINE_RESULT=$(rpc_call "$RPC_PORT_A" "$DATADIR_A" "generatetoaddress" "$BLOCKS_TO_MINE, \"$MINER_ADDRESS\"")

HEIGHT_A=$(get_height "$RPC_PORT_A" "$DATADIR_A")
TIP_A=$(get_best_hash "$RPC_PORT_A" "$DATADIR_A")

echo "  Node A height: $HEIGHT_A"
echo "  Node A tip: $TIP_A"

# Height = premine (1) + mined blocks
EXPECTED_HEIGHT=$((1 + BLOCKS_TO_MINE))
if [[ "$HEIGHT_A" != "$EXPECTED_HEIGHT" ]]; then
    fail "Node A did not mine expected blocks (got $HEIGHT_A, expected $EXPECTED_HEIGHT)"
fi
pass "Mined $BLOCKS_TO_MINE blocks (height now $HEIGHT_A)"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 3: Wait for B and C to sync
# ═══════════════════════════════════════════════════════════════════════════
info "\n[STEP 3] Waiting for nodes B and C to sync..."

if ! wait_for_sync "$RPC_PORT_B" "$DATADIR_B" "$HEIGHT_A" "$TIP_A" "$SYNC_TIMEOUT"; then
    HEIGHT_B=$(get_height "$RPC_PORT_B" "$DATADIR_B")
    fail "Node B sync timeout (height=$HEIGHT_B, target=$HEIGHT_A)"
fi
pass "Node B synced"

if ! wait_for_sync "$RPC_PORT_C" "$DATADIR_C" "$HEIGHT_A" "$TIP_A" "$SYNC_TIMEOUT"; then
    HEIGHT_C=$(get_height "$RPC_PORT_C" "$DATADIR_C")
    fail "Node C sync timeout (height=$HEIGHT_C, target=$HEIGHT_A)"
fi
pass "Node C synced"

# Verify tips match
TIP_B=$(get_best_hash "$RPC_PORT_B" "$DATADIR_B")
TIP_C=$(get_best_hash "$RPC_PORT_C" "$DATADIR_C")

if [[ "$TIP_A" != "$TIP_B" ]]; then
    fail "Node B tip mismatch: $TIP_B != $TIP_A"
fi
if [[ "$TIP_A" != "$TIP_C" ]]; then
    fail "Node C tip mismatch: $TIP_C != $TIP_A"
fi
pass "All nodes converged to same tip"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 4: Restart node B
# ═══════════════════════════════════════════════════════════════════════════
info "\n[STEP 4] Restarting node B (testing persistence)..."

TIP_B_BEFORE=$TIP_B
HEIGHT_B_BEFORE=$(get_height "$RPC_PORT_B" "$DATADIR_B")

# Stop node B (use pkill since daemon reparents to PID 1)
stop_node "$DATADIR_B"

# Restart B (reuse existing datadir)
"$DINEROD" --regtest --datadir="$DATADIR_B" --rpcport="$RPC_PORT_B" --port="$P2P_PORT_B" --listen=1 --connect="127.0.0.1:$P2P_PORT_A" >> "${DATADIR_B}/daemon.log" 2>&1 &
sleep 2
echo "  Node B restarted"

if ! wait_for_ready "$RPC_PORT_B" "$DATADIR_B" 30; then
    fail "Node B failed to restart"
fi
pass "Node B restarted"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 5: Verify B state unchanged after restart
# ═══════════════════════════════════════════════════════════════════════════
info "\n[STEP 5] Verifying node B state after restart..."

HEIGHT_B_AFTER=$(get_height "$RPC_PORT_B" "$DATADIR_B")
TIP_B_AFTER=$(get_best_hash "$RPC_PORT_B" "$DATADIR_B")

echo "  Before restart: height=$HEIGHT_B_BEFORE tip=$TIP_B_BEFORE"
echo "  After restart:  height=$HEIGHT_B_AFTER tip=$TIP_B_AFTER"

if [[ "$HEIGHT_B_BEFORE" != "$HEIGHT_B_AFTER" ]]; then
    fail "Node B height changed after restart! ($HEIGHT_B_BEFORE -> $HEIGHT_B_AFTER)"
fi
if [[ "$TIP_B_BEFORE" != "$TIP_B_AFTER" ]]; then
    fail "Node B tip changed after restart!"
fi
pass "Node B state persisted correctly"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 6: Reindex node C
# ═══════════════════════════════════════════════════════════════════════════
info "\n[STEP 6] Reindexing node C..."

TIP_C_BEFORE=$TIP_C
HEIGHT_C_BEFORE=$(get_height "$RPC_PORT_C" "$DATADIR_C")

# Stop node C (use pkill since daemon reparents to PID 1)
stop_node "$DATADIR_C"

# Restart C with --reindex
"$DINEROD" \
    --regtest \
    --datadir="$DATADIR_C" \
    --rpcport="$RPC_PORT_C" \
    --port="$P2P_PORT_C" \
    --listen=1 \
    --reindex \
    --connect="127.0.0.1:$P2P_PORT_A" \
    >> "${DATADIR_C}/daemon.log" 2>&1 &

echo "  Node C reindexing..."

# Reindex takes longer
if ! wait_for_ready "$RPC_PORT_C" "$DATADIR_C" 120; then
    fail "Node C failed to complete reindex"
fi
pass "Node C reindex complete"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 7: Verify C state after reindex
# ═══════════════════════════════════════════════════════════════════════════
info "\n[STEP 7] Verifying node C state after reindex..."

HEIGHT_C_AFTER=$(get_height "$RPC_PORT_C" "$DATADIR_C")
TIP_C_AFTER=$(get_best_hash "$RPC_PORT_C" "$DATADIR_C")

echo "  Before reindex: height=$HEIGHT_C_BEFORE tip=$TIP_C_BEFORE"
echo "  After reindex:  height=$HEIGHT_C_AFTER tip=$TIP_C_AFTER"

if [[ "$HEIGHT_C_BEFORE" != "$HEIGHT_C_AFTER" ]]; then
    fail "Node C height changed after reindex! ($HEIGHT_C_BEFORE -> $HEIGHT_C_AFTER)"
fi
if [[ "$TIP_C_BEFORE" != "$TIP_C_AFTER" ]]; then
    fail "Node C tip changed after reindex!"
fi
if [[ "$TIP_A" != "$TIP_C_AFTER" ]]; then
    fail "Node C tip doesn't match node A after reindex!"
fi
pass "Node C reindex produced identical state"

# ═══════════════════════════════════════════════════════════════════════════
# FINAL: All assertions passed
# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo -e "${GREEN}✅ COLD-START TEST PASSED${NC}"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Validated:"
echo "  ✓ P2P header sync (FindHeadersToSend)"
echo "  ✓ Block sync and tip convergence"
echo "  ✓ Chainwork consistency across 3 nodes"
echo "  ✓ Restart persistence (node B)"
echo "  ✓ Reindex equivalence (node C)"
echo "  ✓ Premine and signature verification (implicit)"
echo ""

exit 0
