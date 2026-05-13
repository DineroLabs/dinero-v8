#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"
BASE_PORT="${BASE_PORT:-35200}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_C_RPC=$((BASE_PORT + 2))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
NODE_C_P2P=$((BASE_PORT + 102))
DATA_A="/tmp/dinero_parallel_dl_a_$$"
DATA_B="/tmp/dinero_parallel_dl_b_$$"
DATA_C="/tmp/dinero_parallel_dl_c_$$"
LOG_A="${DATA_A}.log"
LOG_B="${DATA_B}.log"
LOG_C="${DATA_C}.log"
PID_A=""
PID_B=""
PID_C=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -80 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -80 "${LOG_B}" >&2 || true; }
    [[ -f "${LOG_C}" ]] && { printf -- '--- node C log tail ---\n' >&2; tail -80 "${LOG_C}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID_A}" ]] && kill "${PID_A}" 2>/dev/null || true
    [[ -n "${PID_B}" ]] && kill "${PID_B}" 2>/dev/null || true
    [[ -n "${PID_C}" ]] && kill "${PID_C}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_A}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_B}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_C}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_A}" "${DATA_B}" "${DATA_C}" "${LOG_A}" "${LOG_B}" "${LOG_C}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
        return 0
    fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
        return 0
    fi
    return 1
}

rpc_call() {
    local rpc_port="$1"
    local datadir="$2"
    local method="$3"
    local params_json="$4"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${rpc_port}/"
}

wait_rpc() {
    local rpc_port="$1"
    local datadir="$2"
    for _ in $(seq 1 60); do
        if rpc_call "${rpc_port}" "${datadir}" "getblockcount" '[]' | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_condition() {
    local cmd="$1"
    local message="$2"
    for _ in $(seq 1 60); do
        if eval "${cmd}"; then
            return 0
        fi
        sleep 1
    done
    fail "${message}"
}

wait_log_contains() {
    local logfile="$1"
    local pattern="$2"
    local message="$3"
    for _ in $(seq 1 60); do
        if grep -Fq "${pattern}" "${logfile}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    fail "${message}"
}

start_node() {
    local datadir="$1"
    local rpc_port="$2"
    local p2p_port="$3"
    local logfile="$4"

    mkdir -p "${datadir}"
    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

require_tools

PID_A="$(start_node "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}")"
PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"
PID_C="$(start_node "${DATA_C}" "${NODE_C_RPC}" "${NODE_C_P2P}" "${LOG_C}")"

wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
wait_rpc "${NODE_C_RPC}" "${DATA_C}" || fail "Node C RPC did not come up"
pass "All regtest nodes are up"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_C_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 2 ]]" \
    "Node B never established both peer connections"
pass "Node B connected to nodes A and C"

rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.createhd" "[\"parallel-download\"]" >/dev/null 2>&1 || true
ADDR_A="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.getnewaddress" '[]' | jq -r '.result.address // .result // empty')"
[[ -n "${ADDR_A}" ]] || fail "Failed to obtain mining address on node A"

MINE_RESULT="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "generatetoaddress" "[1,\"${ADDR_A}\"]")"
jq -e '.error == null' <<<"${MINE_RESULT}" >/dev/null || fail "Node A failed to mine a block: ${MINE_RESULT}"
pass "Node A mined one block"

wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getblockcount\" '[]' | jq -r '.result // -1') -ge 1 ]]" \
    "Node B did not download the mined block"
pass "Node B downloaded the announced block"

if grep -Fq "[Relay] OnGetData received from" "${LOG_A}" 2>/dev/null; then
    wait_log_contains "${LOG_A}" "[BlockRelayManager::HandleGetData] Block SENT to" \
        "Node A received GETDATA but never served the announced block back to node B"
    if grep -Fq "[Relay] OnGetData received from" "${LOG_C}" 2>/dev/null; then
        fail "Node C unexpectedly received GETDATA despite never announcing the block"
    fi
    pass "Parallel block download requested data from the announcing peer only"
else
    wait_log_contains "${LOG_A}" "[BlockRelay] Broadcast cmpctblock to" \
        "Node A neither received GETDATA nor broadcast a compact block"
    wait_log_contains "${LOG_B}" "cmd='cmpctblock'" \
        "Node B did not receive a compact block announcement from the announcer"
    pass "Announcing peer delivered the block directly via compact-block relay"
fi
