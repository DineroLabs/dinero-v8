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
TMP_ROOT="$(mktemp -d /tmp/dinero_reorg_marker_aligned_restart_equiv.XXXXXX)"
DATA_DIR="${TMP_ROOT}/datadir"
LOG_BASE="${TMP_ROOT}/base.log"
LOG_RESTART="${TMP_ROOT}/restart.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -120 "${LOG_BASE}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -160 "${LOG_RESTART}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${TMP_ROOT}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v sqlite3 >/dev/null || fail "sqlite3 is required"
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
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" \
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
    if rpc_has_error "${result}"; then
        fail "generatetoaddress failed: ${result}"
    fi
}

sync_health() {
    local result
    result="$(rpc_call "${DATA_DIR}" "blockchain.getsynchealth" '[]')"
    if rpc_has_error "${result}"; then
        fail "blockchain.getsynchealth failed: ${result}"
    fi
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

utxo_db_path() {
    find "${DATA_DIR}" -path '*/blockchain/utxo' | head -1
}

reorg_marker_value() {
    local utxo_db="$1"
    sqlite3 "${utxo_db}" "SELECT value FROM utxo_metadata WHERE key='reorg_in_progress' LIMIT 1;" 2>/dev/null || true
}

wait_for_reorg_marker_clear() {
    local utxo_db="$1"
    local label="$2"
    for _ in $(seq 1 60); do
        local marker
        marker="$(reorg_marker_value "${utxo_db}")"
        if [[ -z "${marker}" ]]; then
            return 0
        fi
        sleep 1
    done
    fail "${label}: stale reorg_in_progress marker still present"
}

require_tools

RPC_PORT=$((42000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node "${LOG_BASE}"
wait_rpc || fail "base daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
if rpc_has_error "${ADDR_RESULT}"; then
    fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
fi
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

info "Building a canonical chain before injecting a stale reorg marker"
mine_blocks 24 "${MINER_ADDR}"
wait_for_sync_state 24 "pre-restart steady state"
pass "Baseline canonical state is aligned at height 24"

stop_node

UTXO_DB="$(utxo_db_path)"
[[ -n "${UTXO_DB}" ]] || fail "Could not find UTXO sqlite database under ${DATA_DIR}"

sqlite3 "${UTXO_DB}" "INSERT OR REPLACE INTO utxo_metadata(key, value) VALUES ('reorg_in_progress', '24:24:24');"
[[ "$(reorg_marker_value "${UTXO_DB}")" == "24:24:24" ]] || fail "failed to inject stale reorg marker into ${UTXO_DB}"
pass "Injected stale but aligned reorg marker into persisted UTXO metadata"

start_node "${LOG_RESTART}"
wait_rpc || fail "restarted daemon did not reach RPC readiness"
wait_for_sync_state 24 "restart steady state"
wait_for_reorg_marker_clear "${UTXO_DB}" "healthy restart"
pass "Healthy restart auto-cleared stale reorg_in_progress marker"

mine_blocks 1 "${MINER_ADDR}"
wait_for_sync_state 25 "post-restart mining"
[[ -z "$(reorg_marker_value "${UTXO_DB}")" ]] || fail "reorg marker returned after post-restart mining"
pass "Post-restart mining kept canonical state aligned without reviving the marker"

stop_node
