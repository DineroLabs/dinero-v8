#!/usr/bin/env bash
#
# Long historical-range bridge -> CSN utxoblk soak.
#
# Builds a deeper historical chain with recurring real spend blocks, then
# verifies that a fresh stateless CSN can sync the entire range via bridge
# proof serving without missing-utreexo-data, proof failures, or NOTFOUND.
#

set -euo pipefail

SYNC_TIMEOUT=${TIMEOUT:-360}
TOTAL_BLOCKS=${TOTAL_BLOCKS:-360}
SPEND_INTERVAL=${SPEND_INTERVAL:-7}
MULTI_SPEND_INTERVAL=${MULTI_SPEND_INTERVAL:-21}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Honour $DINEROD first (and require it to be executable); the chain
# below never consulted it, so it CLOBBERED the caller's choice and an
# arbitrary build directory could not be used.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
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
PID_BRIDGE=""
PID_CSN=""
EXIT_CODE=0

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    [[ -n "${PID_CSN}" ]] && kill "${PID_CSN}" 2>/dev/null || true
    [[ -n "${PID_BRIDGE}" ]] && kill "${PID_BRIDGE}" 2>/dev/null || true
    [[ -n "${DATADIR_CSN}" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    [[ -n "${DATADIR_BRIDGE}" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    sleep 1
    if [[ ${EXIT_CODE} -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 100 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -100 "${DATADIR_BRIDGE}/daemon.log"
        echo -e "\n${RED}=== CSN daemon.log (last 120 lines) ===${NC}"
        [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -120 "${DATADIR_CSN}/daemon.log"
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

RPC_PORT_BRIDGE=$((34000 + RANDOM % 1000))
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
    curl -s --connect-timeout 2 --max-time 30 \
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
    if rpc_has_error "${response}"; then
        return 1
    fi
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
    sleep 1
    pkill -9 -f "dinerod.*${datadir}" 2>/dev/null || true
    sleep 1
}

echo ""
echo "================================================================="
echo "  Bridge <-> CSN Historical Range Soak"
echo "================================================================="
echo "  total blocks:         ${TOTAL_BLOCKS}"
echo "  spend interval:       ${SPEND_INTERVAL}"
echo "  multi-spend interval: ${MULTI_SPEND_INTERVAL}"
echo "  sync timeout:         ${SYNC_TIMEOUT} s"
echo "================================================================="
echo ""

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_hist_soak_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_hist_soak_XXXXXX)

info "[STEP 1] Starting bridge node..."
start_bridge
pass "Bridge node ready"

info "\n[STEP 2] Mining long historical chain with recurring spend blocks..."
WALLET_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.createhd" '"bridge_hist_range"')
MINER_ADDRESS=$(echo "${WALLET_RESULT}" | jq -r '.result.first_address // empty')
[[ -n "${MINER_ADDRESS}" ]] || fail "Failed to create bridge wallet"
mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 110 "${MINER_ADDRESS}" || fail "Failed to mine maturity blocks"

SPEND_BLOCKS=0
MULTI_SPEND_BLOCKS=0
TXS_CREATED=0
for ((height=111; height<=TOTAL_BLOCKS; height++)); do
    spends_this_block=0
    if (( height % SPEND_INTERVAL == 0 )); then
        spends_this_block=1
        if (( height % MULTI_SPEND_INTERVAL == 0 )); then
            spends_this_block=3
            MULTI_SPEND_BLOCKS=$((MULTI_SPEND_BLOCKS + 1))
        fi
        SPEND_BLOCKS=$((SPEND_BLOCKS + 1))
        for ((n=1; n<=spends_this_block; n++)); do
            RECIPIENT_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.getnewaddress" "[]")
            RECIPIENT_ADDRESS=$(echo "${RECIPIENT_RESULT}" | jq -r '.result.address // .result // empty')
            [[ -n "${RECIPIENT_ADDRESS}" ]] || fail "Failed to derive recipient address"
            amount=$(awk "BEGIN { printf \"%.2f\", 0.20 + (${n} * 0.05) }")
            SEND_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.sendtoaddress" "[\"${RECIPIENT_ADDRESS}\",${amount}]")
            rpc_has_error "${SEND_RESULT}" && fail "wallet.sendtoaddress failed while building historical range"
            TXS_CREATED=$((TXS_CREATED + 1))
        done
    fi
    mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 1 "${MINER_ADDRESS}" || fail "Failed to mine block ${height}"
done

HEIGHT_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
TIP_BRIDGE=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
[[ -n "${HEIGHT_BRIDGE}" && -n "${TIP_BRIDGE}" ]] || fail "Failed to read bridge tip"
pass "Bridge prepared ${HEIGHT_BRIDGE} blocks with ${SPEND_BLOCKS} spend blocks (${MULTI_SPEND_BLOCKS} multi-spend, ${TXS_CREATED} txs)"

info "\n[STEP 3] Starting fresh CSN node..."
start_csn
pass "CSN node ready"

info "\n[STEP 4] Waiting for full historical sync via utxoblk..."
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${HEIGHT_BRIDGE}" "${TIP_BRIDGE}" "${SYNC_TIMEOUT}" || fail "CSN failed to sync long historical range"
pass "CSN synced full historical range to height ${HEIGHT_BRIDGE}"

info "\n[STEP 5] Verifying proof-serving markers across the full range..."
CSN_PROOF_HITS=$(cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null | grep -cE "validated with (transition|batch) proof" || true)
BRIDGE_SERVE_HITS=$(cat "${DATADIR_BRIDGE}/daemon.log" "${DATADIR_BRIDGE}/p2p.log" 2>/dev/null | grep -c "Sent utreexo block" || true)
NOTFOUND_HITS=$(cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null | grep -c "NOTFOUND" || true)

if cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null | grep -q "missing-utreexo-data"; then
    fail "CSN hit missing-utreexo-data during long historical range sync"
fi
if cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null | grep -q "FAIL step"; then
    fail "CSN hit proof validation failure during long historical range sync"
fi
if [[ ${NOTFOUND_HITS:-0} -gt 0 ]]; then
    fail "CSN saw NOTFOUND during long historical range sync (${NOTFOUND_HITS})"
fi
[[ ${BRIDGE_SERVE_HITS:-0} -ge $((HEIGHT_BRIDGE - 10)) ]] || fail "Bridge served too few utxoblk payloads (${BRIDGE_SERVE_HITS} for height ${HEIGHT_BRIDGE})"
[[ ${CSN_PROOF_HITS:-0} -ge $((HEIGHT_BRIDGE - 10)) ]] || fail "CSN validated too few proof-carrying blocks (${CSN_PROOF_HITS} for height ${HEIGHT_BRIDGE})"
pass "Bridge served ${BRIDGE_SERVE_HITS} utxoblk payloads"
pass "CSN validated ${CSN_PROOF_HITS} proof-carrying blocks with zero NOTFOUND"

stop_node PID_CSN "${DATADIR_CSN}" "${RPC_PORT_CSN}"
stop_node PID_BRIDGE "${DATADIR_BRIDGE}" "${RPC_PORT_BRIDGE}"

echo ""
echo "================================================================="
echo -e "${GREEN}  HISTORICAL RANGE SOAK PASSED${NC}"
echo "================================================================="
echo ""
