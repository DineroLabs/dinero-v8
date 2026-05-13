#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"
RUN_ID=$$
BASE_DATA_DIR="/tmp/dinero_connecttip_restart_equiv_${RUN_ID}"
DATA_DIR=""
LOG_CRASH=""
LOG_RESTART=""
RPC_PORT=0
P2P_PORT=0
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_CRASH}" ]] && { printf -- '--- crash log tail ---\n' >&2; tail -80 "${LOG_CRASH}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -120 "${LOG_RESTART}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${BASE_DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${BASE_DATA_DIR}"*
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
    local datadir="$1"
    local method="$2"
    local params_json="$3"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

wait_rpc() {
    for _ in $(seq 1 60); do
        if rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 30); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    env "$@" "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --listen=0 \
        --p2p.offline=1 \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

assert_canonical_alignment() {
    local label="$1"
    local health active_height db_height active_hash db_hash
    health="$(rpc_call "${DATA_DIR}" "blockchain.getsynchealth" '[]')"
    jq -e '.error == null' <<<"${health}" >/dev/null || fail "[${label}] blockchain.getsynchealth failed: ${health}"

    [[ "$(jq -r '.result.canonical_state_aligned // false' <<<"${health}")" == "true" ]] || \
        fail "[${label}] canonical state not aligned: ${health}"

    active_height="$(jq -r '.result.active_height // -1' <<<"${health}")"
    db_height="$(jq -r '.result.chaindb_tip_height // -2' <<<"${health}")"
    active_hash="$(jq -r '.result.active_best_hash // empty' <<<"${health}")"
    db_hash="$(jq -r '.result.chaindb_tip_hash // empty' <<<"${health}")"

    [[ "${active_height}" == "${db_height}" ]] || \
        fail "[${label}] active height ${active_height} != chaindb height ${db_height}: ${health}"
    [[ -n "${active_hash}" && "${active_hash}" == "${db_hash}" ]] || \
        fail "[${label}] active hash ${active_hash} != chaindb hash ${db_hash}: ${health}"
}

run_hook_case() {
    local hook="$1"
    local idx="$2"
    local safe_hook="${hook//[^A-Za-z0-9]/_}"
    DATA_DIR="${BASE_DATA_DIR}/${idx}_${safe_hook}"
    LOG_CRASH="${DATA_DIR}.crash.log"
    LOG_RESTART="${DATA_DIR}.restart.log"
    RPC_PORT=$((33000 + (RUN_ID % 1000) * 10 + idx * 3))
    P2P_PORT=$((RPC_PORT + 1))
    PID=""

    rm -rf "${DATA_DIR}" "${LOG_CRASH}" "${LOG_RESTART}"

    info "[${hook}] starting crash daemon"
    start_node "${LOG_CRASH}" DINERO_CRASH_AT="${hook}"
    wait_rpc || fail "[${hook}] crash-test daemon did not reach RPC readiness"

    local addr_result miner_addr
    addr_result="$(rpc_call "${DATA_DIR}" "getnewaddress" '[]')"
    jq -e '.error == null' <<<"${addr_result}" >/dev/null || fail "[${hook}] getnewaddress failed: ${addr_result}"
    miner_addr="$(jq -r '.result.address // .result // empty' <<<"${addr_result}")"
    [[ -n "${miner_addr}" && "${miner_addr}" != "null" ]] || fail "[${hook}] empty miner address"

    info "[${hook}] triggering ConnectTip crash"
    set +e
    local crash_trigger_result
    crash_trigger_result="$(rpc_call "${DATA_DIR}" "mining.generatetoaddress" "[1,\"${miner_addr}\"]" 2>/dev/null)"
    set -e
    if [[ -n "${crash_trigger_result}" ]] && jq -e '.error != null' <<<"${crash_trigger_result}" >/dev/null 2>&1; then
        fail "[${hook}] mining.generatetoaddress failed before crash trigger: ${crash_trigger_result}"
    fi
    wait_dead "${PID}" || fail "[${hook}] daemon did not crash"
    PID=""

    grep -q "DINERO_CRASH" "${LOG_CRASH}" || fail "[${hook}] crash log did not show named crash hook"
    pass "[${hook}] crash hook triggered"

    start_node "${LOG_RESTART}"
    wait_rpc || fail "[${hook}] restarted daemon did not reach RPC readiness"
    assert_canonical_alignment "${hook} restart"
    pass "[${hook}] restart recovered canonical tip alignment"

    local height_before mine_result height_after health_after
    height_before="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
    mine_result="$(rpc_call "${DATA_DIR}" "mining.generatetoaddress" "[1,\"${miner_addr}\"]")"
    jq -e '.error == null' <<<"${mine_result}" >/dev/null || fail "[${hook}] post-restart generatetoaddress failed: ${mine_result}"
    height_after="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
    [[ "${height_after}" == "$((height_before + 1))" ]] || \
        fail "[${hook}] height did not advance by one after restart mining: before=${height_before} after=${height_after}"

    health_after="$(rpc_call "${DATA_DIR}" "blockchain.getsynchealth" '[]')"
    jq -e '.result.canonical_state_aligned == true' <<<"${health_after}" >/dev/null || \
        fail "[${hook}] canonical state drifted after post-restart mining: ${health_after}"
    pass "[${hook}] post-restart mining preserved alignment"

    stop_node
    rm -rf "${DATA_DIR}" "${LOG_CRASH}" "${LOG_RESTART}"
}

require_tools

if [[ -n "${CONNECTTIP_CRASH_HOOKS:-}" ]]; then
    read -r -a HOOKS <<<"${CONNECTTIP_CRASH_HOOKS}"
else
    HOOKS=(
        after_undo_before_tip
        after_unified_batch_before_frontier_write
        after_tip_before_checkpoint
        after_height_index_before_header
        after_header_before_position_index
    )
fi

idx=0
for hook in "${HOOKS[@]}"; do
    idx=$((idx + 1))
    run_hook_case "${hook}" "${idx}"
done

pass "ConnectTip restart oracle passed for ${#HOOKS[@]} crash hooks"
