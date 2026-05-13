#!/bin/bash
#
# CSN Reorg Churn Stress Test
#
# Repeatedly triggers bridge-side reorgs with invalidateblock/reconsiderblock
# while a stateless CSN node is syncing, then verifies convergence.
#
# Usage:
#   ./test_csn_reorg_churn.sh
#   PRELOAD_BLOCKS=200 CHURN_ROUNDS=6 TIMEOUT=240 ./test_csn_reorg_churn.sh
#

set -euo pipefail

PRELOAD_BLOCKS=${PRELOAD_BLOCKS:-120}
CHURN_ROUNDS=${CHURN_ROUNDS:-4}
ROUND_ADVANCE_BLOCKS=${ROUND_ADVANCE_BLOCKS:-12}
ROUND_REBUILD_BLOCKS=${ROUND_REBUILD_BLOCKS:-8}
SYNC_TIMEOUT=${TIMEOUT:-180}
INVALIDATION_TIMEOUT=${INVALIDATION_TIMEOUT:-60}
RPC_TIMEOUT=${RPC_TIMEOUT:-30}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [[ -n "${DINEROD:-}" && -x "${DINEROD}" ]]; then
    DINEROD="${DINEROD}"
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    echo "dinerod not found"
    exit 1
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

DATADIR_BRIDGE=""
DATADIR_CSN=""
EXIT_CODE=0

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    [[ -n "$DATADIR_BRIDGE" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    [[ -n "$DATADIR_CSN" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    sleep 1

    if [[ $EXIT_CODE -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 40 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -40 "${DATADIR_BRIDGE}/daemon.log"
        echo -e "\n${RED}=== CSN daemon.log (last 40 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -40 "${DATADIR_CSN}/daemon.log"
        if [[ "$KEEP_TMP_ON_FAIL" == "1" ]]; then
            echo -e "\n${YELLOW}Keeping temp dirs (KEEP_TMP_ON_FAIL=1):${NC}"
            [[ -n "$DATADIR_BRIDGE" ]] && echo "  Bridge: ${DATADIR_BRIDGE}"
            [[ -n "$DATADIR_CSN" ]] && echo "  CSN:    ${DATADIR_CSN}"
            return
        fi
    fi

    [[ -n "$DATADIR_BRIDGE" && -d "$DATADIR_BRIDGE" ]] && rm -rf "$DATADIR_BRIDGE"
    [[ -n "$DATADIR_CSN" && -d "$DATADIR_CSN" ]] && rm -rf "$DATADIR_CSN"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }
warn() { echo -e "${YELLOW}WARN: $1${NC}"; }

rpc_call() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local params="$*"
    local cookie
    cookie=$(cat "${datadir}/.cookie" 2>/dev/null || true)
    [[ -z "$cookie" ]] && return 1

    local json_params="[]"
    [[ -n "$params" ]] && json_params="[$params]"

    curl -s --connect-timeout 2 --max-time "${RPC_TIMEOUT}" -u "$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$json_params,\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

rpc_has_error() {
    local compact
    compact=$(echo "$1" | tr -d '\n\t ')
    [[ "$compact" == *"\"error\":null"* ]] && return 1
    [[ "$compact" == *"\"error\":"* ]] && return 0
    return 1
}

mine_blocks() {
    local port=$1
    local datadir=$2
    local count=$3
    local address=$4
    local result
    result=$(rpc_call "$port" "$datadir" "generatetoaddress" "$count, \"$address\"")
    rpc_has_error "$result" && return 1
    return 0
}

get_height() {
    local result
    result=$(rpc_call "$1" "$2" "getblockcount")
    echo "$result" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p'
}

get_best_hash() {
    local result
    result=$(rpc_call "$1" "$2" "getbestblockhash")
    echo "$result" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
}

get_hash_at_height() {
    local result
    result=$(rpc_call "$1" "$2" "getblockhash" "$3")
    echo "$result" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p'
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
        [[ $elapsed -gt $timeout ]] && return 1
        if [[ -f "${datadir}/.cookie" ]]; then
            local h
            h=$(get_height "$port" "$datadir" 2>/dev/null || true)
            [[ -n "$h" ]] && return 0
        fi
        sleep 1
    done
}

wait_for_tip_match() {
    local bridge_port=$1
    local bridge_datadir=$2
    local csn_port=$3
    local csn_datadir=$4
    local timeout=$5
    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
        [[ $elapsed -gt $timeout ]] && return 1
        local bridge_h bridge_tip csn_h csn_tip
        bridge_h=$(get_height "$bridge_port" "$bridge_datadir" 2>/dev/null || true)
        bridge_tip=$(get_best_hash "$bridge_port" "$bridge_datadir" 2>/dev/null || true)
        csn_h=$(get_height "$csn_port" "$csn_datadir" 2>/dev/null || true)
        csn_tip=$(get_best_hash "$csn_port" "$csn_datadir" 2>/dev/null || true)
        if [[ -n "$bridge_h" && -n "$csn_h" && "$bridge_h" == "$csn_h" && "$bridge_tip" == "$csn_tip" ]]; then
            return 0
        fi
        if (( elapsed % 15 == 0 )); then
            echo "    waiting... bridge=${bridge_h:-?} csn=${csn_h:-?}"
        fi
        sleep 1
    done
}

wait_for_invalidation_effect() {
    local port=$1
    local datadir=$2
    local invalid_height=$3
    local invalid_hash=$4
    local timeout=$5

    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
        [[ $elapsed -gt $timeout ]] && return 1

        local h
        h=$(get_height "$port" "$datadir" 2>/dev/null || true)
        if [[ -z "$h" ]]; then
            sleep 1
            continue
        fi

        # Rewound below invalid height: invalidated block is no longer active.
        if [[ "$h" -lt "$invalid_height" ]]; then
            echo "$h"
            return 0
        fi

        # At/above invalid height: ensure active chain hash changed there.
        local active_hash_at_height
        active_hash_at_height=$(get_hash_at_height "$port" "$datadir" "$invalid_height" 2>/dev/null || true)
        if [[ -n "$active_hash_at_height" && "$active_hash_at_height" != "$invalid_hash" ]]; then
            echo "$h"
            return 0
        fi

        sleep 1
    done
}

echo ""
echo "================================================================="
echo "  CSN REORG CHURN STRESS TEST"
echo "================================================================="
echo "  preload blocks:      $PRELOAD_BLOCKS"
echo "  churn rounds:        $CHURN_ROUNDS"
echo "  round advance:       $ROUND_ADVANCE_BLOCKS"
echo "  round rebuild:       $ROUND_REBUILD_BLOCKS"
echo "  sync timeout:        $SYNC_TIMEOUT s"
echo "  invalidate timeout:  $INVALIDATION_TIMEOUT s"
echo "  rpc timeout:         $RPC_TIMEOUT s"
echo "================================================================="
echo ""

RPC_PORT_BRIDGE=$((29000 + RANDOM % 1000))
P2P_PORT_BRIDGE=$((RPC_PORT_BRIDGE + 1))
RPC_PORT_CSN=$((RPC_PORT_BRIDGE + 2))
P2P_PORT_CSN=$((RPC_PORT_BRIDGE + 3))

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_churn_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_churn_XXXXXX)

info "[1/6] Starting Bridge node..."
"$DINEROD" --regtest \
    --datadir="$DATADIR_BRIDGE" \
    --rpcport="$RPC_PORT_BRIDGE" \
    --port="$P2P_PORT_BRIDGE" \
    --listen=1 \
    --utreexo=1 \
    --utreexo-bridge=1 \
    >> "${DATADIR_BRIDGE}/daemon.log" 2>&1 &
wait_for_ready "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" 30 || fail "Bridge failed to start"
pass "Bridge node ready"

info "\n[2/6] Mining preload chain on Bridge..."
WALLET_RESULT=$(rpc_call "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "wallet.createhd" '"bridge_churn"')
MINER_ADDRESS=$(echo "$WALLET_RESULT" | tr -d '\n\t' | sed -n 's/.*"first_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[[ -z "$MINER_ADDRESS" ]] && fail "Failed to create bridge wallet"

mine_blocks "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "$PRELOAD_BLOCKS" "$MINER_ADDRESS" \
    || fail "Failed to mine preload blocks"
BRIDGE_HEIGHT=$(get_height "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE")
pass "Bridge preloaded to height $BRIDGE_HEIGHT"

info "\n[3/6] Starting CSN node and beginning sync..."
"$DINEROD" --regtest \
    --datadir="$DATADIR_CSN" \
    --rpcport="$RPC_PORT_CSN" \
    --port="$P2P_PORT_CSN" \
    --listen=1 \
    --utreexo=1 \
    --utreexo-stateless=1 \
    --connect="127.0.0.1:$P2P_PORT_BRIDGE" \
    >> "${DATADIR_CSN}/daemon.log" 2>&1 &
wait_for_ready "$RPC_PORT_CSN" "$DATADIR_CSN" 30 || fail "CSN failed to start"
pass "CSN node ready"

info "\n[4/6] Running reorg churn rounds during CSN sync..."
for ((round=1; round<=CHURN_ROUNDS; round++)); do
    echo "  Round ${round}/${CHURN_ROUNDS}"

    mine_blocks "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "$ROUND_ADVANCE_BLOCKS" "$MINER_ADDRESS" \
        || fail "Round ${round}: failed to mine advance blocks"

    local_tip_height=$(get_height "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE")
    local_tip_hash=$(get_best_hash "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE")
    echo "    Advanced to height ${local_tip_height} tip=${local_tip_hash:0:16}..."

    if [[ "$local_tip_height" -lt 10 ]]; then
        fail "Round ${round}: bridge height unexpectedly low (${local_tip_height})"
    fi

    fork_height=$((local_tip_height - 3))
    fork_hash=$(get_hash_at_height "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "$fork_height")
    [[ -z "$fork_hash" ]] && fail "Round ${round}: failed to get fork hash at height ${fork_height}"

    inv_result=$(rpc_call "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "invalidateblock" "\"$fork_hash\"")
    if rpc_has_error "$inv_result"; then
        fail "Round ${round}: invalidateblock failed at height ${fork_height}"
    fi

    bridge_after_invalidate=$(wait_for_invalidation_effect \
        "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "$fork_height" "$fork_hash" "$INVALIDATION_TIMEOUT" || true)

    if [[ -z "$bridge_after_invalidate" ]]; then
        bridge_after_invalidate=$(get_height "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" 2>/dev/null || true)
        warn "Round ${round}: invalidate effect not observed within ${INVALIDATION_TIMEOUT}s; continuing (bridge height=${bridge_after_invalidate:-?})"
    elif [[ "$bridge_after_invalidate" -lt "$local_tip_height" ]]; then
        echo "    Invalidated height ${fork_height}; bridge rewound to ${bridge_after_invalidate}"
    else
        echo "    Invalidated height ${fork_height}; bridge switched branch at height ${bridge_after_invalidate}"
    fi

    mine_blocks "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "$ROUND_REBUILD_BLOCKS" "$MINER_ADDRESS" \
        || fail "Round ${round}: failed to mine rebuild blocks"

    reconsider_result=$(rpc_call "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "reconsiderblock" "\"$fork_hash\"")
    if rpc_has_error "$reconsider_result"; then
        fail "Round ${round}: reconsiderblock failed"
    fi

    mine_blocks "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "1" "$MINER_ADDRESS" \
        || fail "Round ${round}: failed to mine final block after reconsider"

    bridge_now=$(get_height "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE")
    csn_now=$(get_height "$RPC_PORT_CSN" "$DATADIR_CSN")
    echo "    Heights after churn: bridge=${bridge_now} csn=${csn_now}"
done

info "\n[5/6] Waiting for final Bridge/CSN tip convergence..."
if wait_for_tip_match "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE" "$RPC_PORT_CSN" "$DATADIR_CSN" "$SYNC_TIMEOUT"; then
    pass "Bridge and CSN converged to identical tip"
else
    BRIDGE_H=$(get_height "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE")
    BRIDGE_TIP=$(get_best_hash "$RPC_PORT_BRIDGE" "$DATADIR_BRIDGE")
    CSN_H=$(get_height "$RPC_PORT_CSN" "$DATADIR_CSN")
    CSN_TIP=$(get_best_hash "$RPC_PORT_CSN" "$DATADIR_CSN")
    echo "  Bridge: height=${BRIDGE_H} tip=${BRIDGE_TIP:0:16}..."
    echo "  CSN:    height=${CSN_H} tip=${CSN_TIP:0:16}..."
    fail "Final tip convergence timeout"
fi

info "\n[6/6] Validating CSN reorg markers and safety assertions..."
ALL_LOGS="${DATADIR_CSN}/daemon.log ${DATADIR_CSN}/p2p.log"
REORG_MARKERS=$(cat $ALL_LOGS 2>/dev/null | grep -cE "STATELESS reorg|RewindToCheckpoint|REORG DETECTED" || true)
REORG_MARKERS=${REORG_MARKERS:-0}
echo "  Reorg markers in CSN logs: $REORG_MARKERS"
[[ "$REORG_MARKERS" -lt 1 ]] && fail "Expected at least one CSN reorg marker in logs"

if cat $ALL_LOGS 2>/dev/null | grep -q "COMMITMENT MISMATCH"; then
    fail "Detected commitment mismatch during churn"
fi
pass "No commitment mismatch detected"

echo ""
echo "================================================================="
echo -e "${GREEN}  CSN REORG CHURN STRESS TEST PASSED${NC}"
echo "================================================================="
echo ""
echo "Validated:"
echo "  - Repeated invalidateblock/reconsiderblock churn during CSN sync"
echo "  - CSN remained live and converged to bridge tip"
echo "  - Reorg handling executed without commitment mismatch"
echo ""

exit 0
