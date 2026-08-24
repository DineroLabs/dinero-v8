#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${ROOT_DIR}/build/dinerod"
fi
DATA_DIR="/tmp/dinero_position_index_restart_equiv_$$"
LOG_BASE="${DATA_DIR}.base.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_RESTART="${DATA_DIR}.restart.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -120 "${LOG_BASE}" >&2 || true; }
    [[ -f "${LOG_CRASH}" ]] && { printf -- '--- crash log tail ---\n' >&2; tail -120 "${LOG_CRASH}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -160 "${LOG_RESTART}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_BASE}" "${LOG_CRASH}" "${LOG_RESTART}"
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

rpc_has_error() {
    local compact
    compact="$(echo "$1" | tr -d '\n\t ')"
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 60); do
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
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --utreexo.checkpoint_interval=5 \
        --p2p.offline=1 \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "${DATA_DIR}" "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${stop_result}" ]] && rpc_has_error "${stop_result}"; then
        kill "${PID}" 2>/dev/null || true
    fi
    wait_dead "${PID}" || kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

rpc_address() {
    local response="$1"
    jq -r '.result.address // .result // empty' <<<"${response}"
}

mine_blocks() {
    local blocks="$1"
    local address="$2"
    local result
    result="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[${blocks},\"${address}\"]")"
    rpc_has_error "${result}" && fail "generatetoaddress failed: ${result}"
    return 0
}

sync_health() {
    local result
    result="$(rpc_call "${DATA_DIR}" "blockchain.getsynchealth" '[]')"
    rpc_has_error "${result}" && fail "blockchain.getsynchealth failed: ${result}"
    jq -c '.result' <<<"${result}"
}

wait_for_sync_state() {
    local expected_height="$1"
    local label="$2"
    for _ in $(seq 1 90); do
        local health
        health="$(sync_health)"
        if jq -e \
            --argjson expected_height "${expected_height}" \
            --argjson expected_ckpt "$(( expected_height - (expected_height % 5) ))" \
            '
            .active_height == $expected_height and
            .chaindb_tip_height == $expected_height and
            .canonical_state_aligned == true and
            .latest_utreexo_checkpoint_found == true and
            .latest_utreexo_checkpoint_height == $expected_ckpt and
            .latest_utreexo_checkpoint_has_checksum == true and
            .utreexo_checksum_version == "1" and
            .forest_tip_marker_found == true and
            .forest_tip_marker_height == $expected_height and
            (.active_best_hash | type) == "string" and
            (.active_best_hash | length) > 0 and
            .chaindb_tip_hash == .active_best_hash and
            .forest_tip_marker_hash == .active_best_hash
            ' <<<"${health}" >/dev/null; then
            return 0
        fi
        sleep 1
    done
    fail "${label}: timed out waiting for canonical state ${expected_height}"
}

get_block_hash() {
    local height="$1"
    local result
    result="$(rpc_call "${DATA_DIR}" "getblockhash" "[${height}]")"
    rpc_has_error "${result}" && fail "getblockhash(${height}) failed: ${result}"
    jq -r '.result // empty' <<<"${result}"
}

get_coinbase_outpoint() {
    local block_hash="$1"
    local result
    result="$(rpc_call "${DATA_DIR}" "getblock" "[\"${block_hash}\",1]")"
    rpc_has_error "${result}" && fail "getblock(${block_hash}) failed: ${result}"
    jq -r '
        .result.tx[0] as $coinbase |
        if ($coinbase | type) == "object" then
            "\($coinbase.txid) 0"
        else
            "\($coinbase) 0"
        end
    ' <<<"${result}" | head -n 1
}

assert_utxoproof_success() {
    local txid="$1"
    local vout="$2"
    local label="$3"
    local result
    result="$(rpc_call "${DATA_DIR}" "blockchain.getutxoproof" "[\"${txid}\",${vout}]")"
    rpc_has_error "${result}" && fail "${label}: getutxoproof returned top-level RPC error: ${result}"
    jq -e '
        (.result.error | not) and
        (.result.leaf_hash | type) == "string" and
        (.result.leaf_hash | length) == 64 and
        (.result.position | type) == "number" and
        (.result.accumulator_root | type) == "string" and
        (.result.accumulator_root | length) == 64 and
        (.result.block_hash | type) == "string" and
        (.result.block_hash | length) == 64 and
        (.result.height | type) == "number"
    ' <<<"${result}" >/dev/null || fail "${label}: invalid proof payload: ${result}"
}

require_tools

RPC_PORT=$((40000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node "${LOG_BASE}"
wait_rpc || fail "base daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

info "Building a checkpointed chain before position-index crash"
mine_blocks 24 "${MINER_ADDR}"
wait_for_sync_state 24 "pre-crash steady state"
pass "Pre-crash chain reached height 24 with canonical state aligned"

stop_node

start_node "${LOG_CRASH}" DINERO_CRASH_AT=after_header_before_position_index
wait_rpc || fail "crash-test daemon did not reach RPC readiness"
wait_for_sync_state 24 "pre-trigger state on crash daemon"

info "Triggering crash after header persistence but before position-index update"
set +e
CRASH_TRIGGER_RESULT="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[1,\"${MINER_ADDR}\"]" 2>/dev/null)"
set -e
if [[ -n "${CRASH_TRIGGER_RESULT}" ]] && rpc_has_error "${CRASH_TRIGGER_RESULT}"; then
    fail "generatetoaddress failed before crash trigger: ${CRASH_TRIGGER_RESULT}"
fi
wait_dead "${PID}" || fail "daemon did not crash at after_header_before_position_index"
PID=""

grep -q "DINERO_CRASH" "${LOG_CRASH}" || fail "crash log did not show named crash hook"
pass "Crash hook triggered after header persistence"

start_node "${LOG_RESTART}"
wait_rpc || fail "restarted daemon did not reach RPC readiness"
wait_for_sync_state 25 "post-restart recovery"

BLOCK_25_HASH="$(get_block_hash 25)"
[[ -n "${BLOCK_25_HASH}" ]] || fail "height 25 hash missing after restart"

COINBASE_OUTPOINT="$(get_coinbase_outpoint "${BLOCK_25_HASH}")"
[[ -n "${COINBASE_OUTPOINT}" ]] || fail "could not identify the crash-block coinbase outpoint at height 25"
COINBASE_TXID="$(awk '{print $1}' <<<"${COINBASE_OUTPOINT}")"
COINBASE_VOUT="$(awk '{print $2}' <<<"${COINBASE_OUTPOINT}")"
[[ -n "${COINBASE_TXID}" && -n "${COINBASE_VOUT}" ]] || fail "coinbase outpoint parsing failed: ${COINBASE_OUTPOINT}"

assert_utxoproof_success "${COINBASE_TXID}" "${COINBASE_VOUT}" "post-restart proof generation"
pass "Restart rebuilt the position index and restored proof serving for crash-block UTXO"

mine_blocks 1 "${MINER_ADDR}"
wait_for_sync_state 26 "post-restart mining"
assert_utxoproof_success "${COINBASE_TXID}" "${COINBASE_VOUT}" "post-restart proof stability"
pass "Post-restart mining preserved proof-serving side-state"

stop_node
