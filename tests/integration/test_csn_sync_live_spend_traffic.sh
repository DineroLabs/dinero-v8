#!/usr/bin/env bash
#
# Fresh CSN sync under live spend traffic regression.
#
# Proves a fresh stateless CSN can bootstrap from historical proof-bearing
# blocks while the bridge keeps accepting spends and mining new tip blocks.
# The historical proof relay and live tip proof relay must coexist without
# missing-utreexo-data, NOTFOUND, or proof validation failures.
#

set -euo pipefail

SYNC_TIMEOUT=${TIMEOUT:-420}
PRELOAD_BLOCKS=${PRELOAD_BLOCKS:-220}
PRELOAD_SPEND_INTERVAL=${PRELOAD_SPEND_INTERVAL:-6}
LIVE_ROUNDS=${LIVE_ROUNDS:-24}
LIVE_TXS_PER_ROUND=${LIVE_TXS_PER_ROUND:-2}
LIVE_ROUND_SLEEP=${LIVE_ROUND_SLEEP:-1}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

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
PID_BRIDGE=""
PID_CSN=""
TRAFFIC_PID=""
OVERLAP_MARKER=""
EXIT_CODE=0

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    if [[ -n "${TRAFFIC_PID}" ]]; then
        kill "${TRAFFIC_PID}" 2>/dev/null || true
        wait "${TRAFFIC_PID}" 2>/dev/null || true
    fi
    if [[ -n "${PID_CSN}" ]]; then
        kill "${PID_CSN}" 2>/dev/null || true
        wait "${PID_CSN}" 2>/dev/null || true
    fi
    if [[ -n "${PID_BRIDGE}" ]]; then
        kill "${PID_BRIDGE}" 2>/dev/null || true
        wait "${PID_BRIDGE}" 2>/dev/null || true
    fi
    [[ -n "${DATADIR_CSN}" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    [[ -n "${DATADIR_BRIDGE}" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    sleep 1
    if [[ ${EXIT_CODE} -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 120 lines) ===${NC}"
        [[ -f "${DATADIR_BRIDGE}/daemon.log" ]] && tail -120 "${DATADIR_BRIDGE}/daemon.log"
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
    [[ -n "${OVERLAP_MARKER}" && -f "${OVERLAP_MARKER}" ]] && rm -f "${OVERLAP_MARKER}"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

RPC_PORT_BRIDGE=$((36000 + RANDOM % 1000))
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

mine_preload_history() {
    local miner_address=$1
    mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 110 "${miner_address}" \
        || fail "Failed to mine maturity blocks"

    local spend_blocks=0
    local txs_created=0
    for ((height=111; height<=PRELOAD_BLOCKS; height++)); do
        if (( height % PRELOAD_SPEND_INTERVAL == 0 )); then
            spend_blocks=$((spend_blocks + 1))
            local recipient_result recipient amount send_result
            recipient_result=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.getnewaddress" "[]")
            recipient=$(echo "${recipient_result}" | jq -r '.result.address // .result // empty')
            [[ -n "${recipient}" ]] || fail "Failed to derive preload recipient address"
            amount="0.25"
            send_result=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.sendtoaddress" "[\"${recipient}\",${amount}]")
            rpc_has_error "${send_result}" && fail "wallet.sendtoaddress failed while building preload history"
            txs_created=$((txs_created + 1))
        fi
        mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 1 "${miner_address}" \
            || fail "Failed to mine preload block ${height}"
    done
    pass "Bridge prepared ${PRELOAD_BLOCKS} historical blocks with ${spend_blocks} spend blocks (${txs_created} txs)"
}

run_live_traffic() {
    local miner_address=$1
    local marker=$2
    for ((round=1; round<=LIVE_ROUNDS; round++)); do
        for ((n=1; n<=LIVE_TXS_PER_ROUND; n++)); do
            local recipient_result recipient amount send_result
            recipient_result=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.getnewaddress" "[]")
            recipient=$(echo "${recipient_result}" | jq -r '.result.address // .result // empty')
            [[ -n "${recipient}" ]] || exit 1
            amount=$(awk "BEGIN { printf \"%.2f\", 0.18 + (${n} * 0.03) }")
            send_result=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.sendtoaddress" "[\"${recipient}\",${amount}]")
            rpc_has_error "${send_result}" && exit 1
        done

        mine_blocks_to_address "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" 1 "${miner_address}" || exit 1

        local bridge_height csn_height
        bridge_height=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result' 2>/dev/null || true)
        csn_height=$(rpc_scalar "${RPC_PORT_CSN}" "${DATADIR_CSN}" "getblockcount" '.result' 2>/dev/null || true)
        if [[ -n "${bridge_height}" && -n "${csn_height}" && "${csn_height}" -lt "${bridge_height}" ]]; then
            : > "${marker}"
        fi

        sleep "${LIVE_ROUND_SLEEP}"
    done
}

assert_clean_logs() {
    local csn_logs bridge_logs
    csn_logs=$(cat "${DATADIR_CSN}/daemon.log" "${DATADIR_CSN}/p2p.log" 2>/dev/null || true)
    bridge_logs=$(cat "${DATADIR_BRIDGE}/daemon.log" "${DATADIR_BRIDGE}/p2p.log" 2>/dev/null || true)

    echo "${csn_logs}" | grep -q "missing-utreexo-data" && fail "CSN hit missing-utreexo-data during live-traffic sync"
    echo "${csn_logs}" | grep -q "FAIL step" && fail "CSN hit proof validation failure during live-traffic sync"
    echo "${csn_logs}" | grep -q "NOTFOUND" && fail "CSN saw NOTFOUND during live-traffic sync"
    echo "${bridge_logs}" | grep -q "returned no proof" && fail "Bridge returned no proof during live-traffic sync"

    local csn_hits bridge_hits
    csn_hits=$(echo "${csn_logs}" | grep -cE "validated with (transition|batch) proof" || true)
    bridge_hits=$(echo "${bridge_logs}" | grep -c "Sent utreexo block" || true)
    [[ ${csn_hits:-0} -ge $((PRELOAD_BLOCKS + LIVE_ROUNDS - 10)) ]] \
        || fail "CSN validated too few proof-carrying blocks under live traffic (${csn_hits})"
    [[ ${bridge_hits:-0} -ge $((PRELOAD_BLOCKS + LIVE_ROUNDS - 10)) ]] \
        || fail "Bridge served too few utxoblk payloads under live traffic (${bridge_hits})"

    pass "Bridge served ${bridge_hits} proof-carrying blocks under live traffic"
    pass "CSN validated ${csn_hits} proof-carrying blocks under live traffic"
}

echo ""
echo "================================================================="
echo "  CSN Sync Under Live Spend Traffic"
echo "================================================================="
echo "  preload blocks:        ${PRELOAD_BLOCKS}"
echo "  preload spend interval:${PRELOAD_SPEND_INTERVAL}"
echo "  live rounds:           ${LIVE_ROUNDS}"
echo "  txs/round:             ${LIVE_TXS_PER_ROUND}"
echo "  sync timeout:          ${SYNC_TIMEOUT} s"
echo "================================================================="
echo ""

command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

DATADIR_BRIDGE=$(mktemp -d -t dinero_bridge_live_traffic_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_csn_live_traffic_XXXXXX)
OVERLAP_MARKER=$(mktemp -t dinero_live_traffic_overlap_XXXXXX)
rm -f "${OVERLAP_MARKER}"

info "[1/6] Starting bridge node"
start_bridge
pass "Bridge node ready"

info "\n[2/6] Building historical spend-heavy chain"
WALLET_RESULT=$(rpc_call "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "wallet.createhd" '"bridge_live_traffic"')
MINER_ADDRESS=$(echo "${WALLET_RESULT}" | jq -r '.result.first_address // empty')
[[ -n "${MINER_ADDRESS}" ]] || fail "Failed to create bridge wallet"
mine_preload_history "${MINER_ADDRESS}"

info "\n[3/6] Starting fresh CSN and launching live spend traffic"
start_csn
pass "CSN node ready"
run_live_traffic "${MINER_ADDRESS}" "${OVERLAP_MARKER}" >> "${DATADIR_BRIDGE}/traffic.log" 2>&1 &
TRAFFIC_PID=$!

info "\n[4/6] Waiting for live traffic to complete and CSN to converge"
wait "${TRAFFIC_PID}" || fail "Live spend traffic generator failed"
TRAFFIC_PID=""
FINAL_HEIGHT=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getblockcount" '.result')
FINAL_TIP=$(rpc_scalar "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}" "getbestblockhash" '.result')
[[ -n "${FINAL_HEIGHT}" && -n "${FINAL_TIP}" ]] || fail "Failed to read final bridge tip"
wait_for_sync "${RPC_PORT_CSN}" "${DATADIR_CSN}" "${FINAL_HEIGHT}" "${FINAL_TIP}" "${SYNC_TIMEOUT}" \
    || fail "CSN failed to sync bridge tip under live spend traffic"
pass "CSN converged to live bridge tip at height ${FINAL_HEIGHT}"

info "\n[5/6] Verifying overlapping sync and exact final state"
[[ -f "${OVERLAP_MARKER}" ]] || fail "Did not observe live traffic while CSN was behind the bridge tip"
assert_same_state \
    "$(capture_state_json "${RPC_PORT_BRIDGE}" "${DATADIR_BRIDGE}")" \
    "$(capture_state_json "${RPC_PORT_CSN}" "${DATADIR_CSN}")" \
    "bridge vs CSN final state"
pass "Observed live traffic while CSN sync was in progress"
pass "Final bridge/CSN stump state matches exactly"

info "\n[6/6] Checking proof-serving logs"
assert_clean_logs

echo ""
echo "================================================================="
echo -e "${GREEN}  LIVE TRAFFIC SYNC PASSED${NC}"
echo "================================================================="
echo ""
echo "Validated:"
echo "  - Fresh CSN synced historical proof-bearing blocks while the bridge kept mining spends"
echo "  - Live tip proof relay and historical proof relay coexisted without NOTFOUND or proof failures"
echo "  - Bridge and CSN finished on the exact same stump/tip state"
echo ""

exit 0
