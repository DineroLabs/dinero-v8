#!/usr/bin/env bash
#
# CSN restart/resume/checkpoint regression.
#
# Proves three boring but critical properties for stateless sync:
#   1) restart during sync restores the same stump/tip state before resuming
#   2) resumed CSN still syncs through historical spend blocks to the bridge tip
#   3) restart after tip restores the same final stump/tip state
#
# Usage:
#   ./test_csn_restart_resume_checkpoint.sh
#   PRELOAD_BLOCKS=190 RESTART_HEIGHT=100 TIMEOUT=240 ./test_csn_restart_resume_checkpoint.sh
#

set -euo pipefail

SYNC_TIMEOUT=${TIMEOUT:-240}
RESTART_HEIGHT=${RESTART_HEIGHT:-100}
PRELOAD_BLOCKS=${PRELOAD_BLOCKS:-400}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ -n "${DINEROD:-}" ]]; then
    # CTest supplies this (ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"), so the
    # test follows the build directory wherever it is. Honour it and require it
    # to be real — never silently fall through to a guessed path.
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 1; }
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    # Manual/local convenience only.
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    # Fail HERE, naming the paths tried. Launching a non-existent binary and
    # then waiting on its RPC turns a missing file into a 30s timeout reported
    # as "RPC never came up", which reads like a consensus failure.
    echo "dinerod not found (tried: \$DINEROD unset, ${PROJECT_ROOT}/build/dinerod, ${PROJECT_ROOT}/dinerod)" >&2
    echo "set DINEROD=/path/to/dinerod to override" >&2
    exit 1
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

DATADIR_BRIDGE=""
DATADIR_CSN=""
PID_BRIDGE=""
PID_CSN=""
EXIT_CODE=0

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    [[ -n "${PID_CSN}" ]] && kill "${PID_CSN}" 2>/dev/null || true
    [[ -n "${PID_BRIDGE}" ]] && kill "${PID_BRIDGE}" 2>/dev/null || true
    [[ -n "${DATADIR_BRIDGE}" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    [[ -n "${DATADIR_CSN}" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    sleep 1

    if [[ ${EXIT_CODE} -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 80 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -80 "${DATADIR_BRIDGE}/daemon.log"
        echo -e "\n${RED}=== CSN daemon.log (last 80 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -80 "${DATADIR_CSN}/daemon.log"
        if [[ "${KEEP_TMP_ON_FAIL}" == "1" ]]; then
            echo -e "\n${YELLOW}Keeping temp dirs for debugging:${NC}"
            [[ -n "${DATADIR_BRIDGE}" ]] && echo "  Bridge: ${DATADIR_BRIDGE}"
            [[ -n "${DATADIR_CSN}" ]] && echo "  CSN:    ${DATADIR_CSN}"
            return
        fi
    fi

    [[ -n "${DATADIR_BRIDGE}" && -d "${DATADIR_BRIDGE}" ]] && rm -rf "${DATADIR_BRIDGE}"
    [[ -n "${DATADIR_CSN}" && -d "${DATADIR_CSN}" ]] && rm -rf "${DATADIR_CSN}"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }

RPC_PORT_BRIDGE=$((30000 + RANDOM % 1000))
P2P_PORT_BRIDGE=$((RPC_PORT_BRIDGE + 1))
RPC_PORT_CSN=$((RPC_PORT_BRIDGE + 2))
P2P_PORT_CSN=$((RPC_PORT_BRIDGE + 3))
WALLET_PORT_BRIDGE=$((RPC_PORT_BRIDGE + 4))
WALLET_PORT_CSN=$((RPC_PORT_BRIDGE + 5))

rpc_call() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local params="$*"
    local cookie
    cookie=$(cat "${datadir}/.cookie" 2>/dev/null || true)
    [[ -z "${cookie}" ]] && return 1
    local json_params="[]"
    if [[ -n "${params}" ]]; then
        if [[ "${params}" == \[*\] ]]; then
            json_params="${params}"
        else
            json_params="[${params}]"
        fi
    fi
    curl -s --connect-timeout 2 --max-time 20 \
        -u "${cookie}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

rpc_has_error() {
    local compact
    compact=$(echo "$1" | tr -d '\n\t ')
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

rpc_scalar() {
    local port=$1
    local datadir=$2
    local method=$3
    local jq_expr=$4
    shift 4
    local response
    response=$(rpc_call "${port}" "${datadir}" "${method}" "$@")
    rpc_has_error "${response}" && return 1
    echo "${response}" | jq -r "${jq_expr} // empty"
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        if [[ -f "${datadir}/.cookie" ]]; then
            local height
            height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result' 2>/dev/null || true)
            [[ -n "${height}" ]] && return 0
        fi
        sleep 1
    done
}

wait_for_height_at_least() {
    local port=$1
    local datadir=$2
    local min_height=$3
    local timeout=$4
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local height
        height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result' 2>/dev/null || true)
        if [[ -n "${height}" && "${height}" -ge "${min_height}" ]]; then
            return 0
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
    local start
    start=$(date +%s)
    while true; do
        [[ $(($(date +%s) - start)) -gt ${timeout} ]] && return 1
        local height hash
        height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result' 2>/dev/null || true)
        hash=$(rpc_scalar "${port}" "${datadir}" "getbestblockhash" '.result' 2>/dev/null || true)
        [[ "${height}" == "${target_height}" && "${hash}" == "${target_hash}" ]] && return 0
        sleep 1
    done
}

assert_mid_sync_state() {
    local state_json=$1
    local label=$2
    local height
    height=$(echo "${state_json}" | jq -r '.height')
    [[ -n "${height}" ]] || fail "${label}: missing height"
    [[ "${height}" -ge "${RESTART_HEIGHT}" ]] || fail "${label}: height ${height} below restart height ${RESTART_HEIGHT}"
    [[ "${height}" -lt "${HEIGHT_BRIDGE}" ]] || fail "${label}: height ${height} already reached tip ${HEIGHT_BRIDGE}"
}

mine_blocks_to_address() {
    local port=$1
    local datadir=$2
    local total=$3
    local address=$4
    local result
    result=$(rpc_call "${port}" "${datadir}" "generatetoaddress" "${total}, \"${address}\"")
    rpc_has_error "${result}" && return 1
    return 0
}

capture_state_json() {
    local port=$1
    local datadir=$2
    local height tip commitment roots

    height=$(rpc_scalar "${port}" "${datadir}" "getblockcount" '.result')
    tip=$(rpc_scalar "${port}" "${datadir}" "getbestblockhash" '.result')
    commitment=$(rpc_call "${port}" "${datadir}" "blockchain.getutreexocommitment" "[]")
    roots=$(rpc_call "${port}" "${datadir}" "blockchain.getutreexoroots" "[]")

    jq -n \
        --argjson height "${height}" \
        --arg tip "${tip}" \
        --arg commitment "$(echo "${commitment}" | jq -r '.result.commitment // empty')" \
        --argjson num_leaves "$(echo "${commitment}" | jq -r '.result.num_leaves // 0')" \
        --argjson num_roots "$(echo "${commitment}" | jq -r '.result.num_roots // 0')" \
        --argjson roots "$(echo "${roots}" | jq -c '.result.roots // []')" \
        '{
            height: $height,
            tip: $tip,
            commitment: $commitment,
            num_leaves: $num_leaves,
            num_roots: $num_roots,
            roots: $roots
        }'
}

assert_same_state() {
    local lhs_json=$1
    local rhs_json=$2
    local label=$3
    local mismatch
    mismatch=$(jq -n \
        --argjson lhs "${lhs_json}" \
        --argjson rhs "${rhs_json}" \
        '{
            height: ($lhs.height == $rhs.height),
            tip: ($lhs.tip == $rhs.tip),
            commitment: ($lhs.commitment == $rhs.commitment),
            num_leaves: ($lhs.num_leaves == $rhs.num_leaves),
            num_roots: ($lhs.num_roots == $rhs.num_roots),
            roots: ($lhs.roots == $rhs.roots)
        }')
    if [[ "$(echo "${mismatch}" | jq -r 'all(.[]; . == true)')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "lhs=$(echo "${lhs_json}" | jq -c '.')"
        echo "rhs=$(echo "${rhs_json}" | jq -c '.')"
        echo "eq =$(echo "${mismatch}" | jq -c '.')"
        fail "${label} state mismatch"
    fi
}

start_bridge() {
    "$DINEROD" --regtest \
        --datadir="${DATADIR_BRIDGE}" \
        --rpcport="${RPC_PORT_BRIDGE}" \
        --port="${P2P_PORT_BRIDGE}" \
        --wallet-socket-port="${WALLET_PORT_BRIDGE}" \
        --listen=1 \
        --connect="127.0.0.1:${P2P_PORT_CSN}" \
        --utreexo=1 \
        --utreexo-bridge=1 \
        >> "${DATADIR_BRIDGE}/daemon.log" 2>&1 &
    PID_BRIDGE=$!
    wait_for_ready "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 30 || fail "Bridge node failed to start"
}

start_csn() {
    "$DINEROD" --regtest \
        --datadir="${DATADIR_CSN}" \
        --rpcport="${RPC_PORT_CSN}" \
        --port="${P2P_PORT_CSN}" \
        --wallet-socket-port="${WALLET_PORT_CSN}" \
        --listen=1 \
        --utreexo=1 \
        --utreexo-stateless=1 \
        --connect="127.0.0.1:${P2P_PORT_BRIDGE}" \
        >> "${DATADIR_CSN}/daemon.log" 2>&1 &
    PID_CSN=$!
    wait_for_ready "${RPC_PORT_CSN}" "${DATADIR_CSN}" 30 || fail "CSN node failed to start"
}

stop_node() {
    local pid_var=$1
    local datadir=$2
    local rpc_port=${3:-}
    local pid="${!pid_var:-}"
    if [[ -n "${rpc_port}" && -f "${datadir}/.cookie" ]]; then
        rpc_call "${rpc_port}" "${datadir}" "stop" "[]" >/dev/null 2>&1 || true
    fi
    if [[ -n "${pid}" ]]; then
        for _ in $(seq 1 20); do
            if ! kill -0 "${pid}" 2>/dev/null; then
                break
            fi
            sleep 1
        done
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
        printf -v "${pid_var}" '%s' ""
    fi
    pkill -TERM -f "dinerod.*${datadir}" 2>/dev/null || true
    sleep 2
    pkill -9 -f "dinerod.*${datadir}" 2>/dev/null || true
    sleep 1
}

echo ""
echo "================================================================="
echo "  CSN Restart / Resume / Checkpoint Test"
echo "================================================================="
echo "  preload blocks:      ${PRELOAD_BLOCKS}"
echo "  restart height:      ${RESTART_HEIGHT}"
echo "  sync timeout:        ${SYNC_TIMEOUT} s"
echo "================================================================="
echo ""

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_restart_resume_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_restart_resume_XXXXXX)

info "[STEP 1] Starting bridge node..."
start_bridge
pass "Bridge node ready"

info "\n[STEP 2] Mining chain with a real spend block..."
WALLET_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.createhd" '"bridge_restart_resume"')
MINER_ADDRESS=$(echo "${WALLET_RESULT}" | jq -r '.result.first_address // empty')
[[ -n "${MINER_ADDRESS}" ]] || fail "Failed to create bridge wallet"

mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 110 "${MINER_ADDRESS}" || fail "Failed to mine maturity blocks"

RECIPIENT_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.getnewaddress" "[]")
RECIPIENT_ADDRESS=$(echo "${RECIPIENT_RESULT}" | jq -r '.result.address // .result // empty')
[[ -n "${RECIPIENT_ADDRESS}" ]] || fail "Failed to derive recipient address"

SEND_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.sendtoaddress" "[\"${RECIPIENT_ADDRESS}\",1.0]")
rpc_has_error "${SEND_RESULT}" && fail "wallet.sendtoaddress failed"

mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 1 "${MINER_ADDRESS}" || fail "Failed to mine spend block"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" $((PRELOAD_BLOCKS - 111)) "${MINER_ADDRESS}" || fail "Failed to mine trailing blocks"

HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
[[ -n "${HEIGHT_BRIDGE}" && -n "${TIP_BRIDGE}" ]] || fail "Failed to read bridge tip"
[[ "${HEIGHT_BRIDGE}" -gt "${RESTART_HEIGHT}" ]] || fail "restart height must be below bridge tip"
pass "Bridge tip prepared at height ${HEIGHT_BRIDGE}"

info "\n[STEP 3] Starting fresh CSN and syncing to restart point..."
start_csn
pass "CSN node ready"
wait_for_height_at_least "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${RESTART_HEIGHT}" "${SYNC_TIMEOUT}" || fail "CSN failed to reach restart height"
LIVE_PROGRESS_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
pass "Observed live mid-sync progress at height $(echo "${LIVE_PROGRESS_STATE}" | jq -r '.height')"

info "\n[STEP 4] Freezing a mid-sync checkpoint and verifying offline restart determinism..."
stop_node PID_CSN "${DATADIR_CSN}" "${RPC_PORT_CSN}"
stop_node PID_BRIDGE "${DATADIR_BRIDGE}" "${RPC_PORT_BRIDGE}"
start_csn
MID_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
assert_mid_sync_state "${MID_STATE}" "first mid-sync restore"
pass "Restored persisted mid-sync checkpoint at height $(echo "${MID_STATE}" | jq -r '.height')"

stop_node PID_CSN "${DATADIR_CSN}" "${RPC_PORT_CSN}"
start_csn
RESTORED_MID_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
assert_same_state "${MID_STATE}" "${RESTORED_MID_STATE}" "mid-sync checkpoint restore"
pass "Repeated offline restart restored identical mid-sync stump/tip state"

info "\n[STEP 5] Bringing bridge back and resuming to tip..."
start_bridge
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" || fail "CSN failed to resume to bridge tip"
FINAL_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
BRIDGE_STATE=$(capture_state_json "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}")
assert_same_state "${BRIDGE_STATE}" "${FINAL_STATE}" "final bridge/csn convergence"
pass "CSN resumed and converged to bridge tip"

info "\n[STEP 6] Restarting synced CSN offline and verifying final checkpoint restore..."
stop_node PID_CSN "${DATADIR_CSN}" "${RPC_PORT_CSN}"
stop_node PID_BRIDGE "${DATADIR_BRIDGE}" "${RPC_PORT_BRIDGE}"
start_csn
RESTORED_FINAL_STATE=$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")
assert_same_state "${FINAL_STATE}" "${RESTORED_FINAL_STATE}" "tip checkpoint restore"
pass "Post-tip restart restored identical stump/tip state"
stop_node PID_CSN "${DATADIR_CSN}" "${RPC_PORT_CSN}"

echo ""
echo "================================================================="
echo -e "${GREEN}  CSN RESTART / RESUME / CHECKPOINT TEST PASSED${NC}"
echo "================================================================="
echo ""
