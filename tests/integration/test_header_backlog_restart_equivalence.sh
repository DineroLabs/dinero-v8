#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DINERO_BUILD_DIR:-${ROOT_DIR}/build}"
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${BUILD_DIR}/dinerod"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
BACKLOG_INJECTOR="${BUILD_DIR}/tests/integration/header_store_backlog_injector"
if [[ ! -x "${BACKLOG_INJECTOR}" ]]; then
    BACKLOG_INJECTOR="${BUILD_DIR}/header_store_backlog_injector"
fi
TMP_ROOT="$(mktemp -d /tmp/dinero_header_backlog_restart_equiv.XXXXXX)"
DATA_DIR="${TMP_ROOT}/datadir"
LOG_BASE="${TMP_ROOT}/base.log"
LOG_RESTART1="${TMP_ROOT}/restart1.log"
LOG_RESTART2="${TMP_ROOT}/restart2.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -120 "${LOG_BASE}" >&2 || true; }
    [[ -f "${LOG_RESTART1}" ]] && { printf -- '--- restart1 log tail ---\n' >&2; tail -160 "${LOG_RESTART1}" >&2 || true; }
    [[ -f "${LOG_RESTART2}" ]] && { printf -- '--- restart2 log tail ---\n' >&2; tail -160 "${LOG_RESTART2}" >&2 || true; }
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
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
    [[ -x "${BACKLOG_INJECTOR}" ]] || fail "header_store_backlog_injector not built at ${BACKLOG_INJECTOR}"
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
    rpc_has_error "${result}" && fail "blockchain.getsynchealth failed: ${result}"
    jq -c '.result' <<<"${result}"
}

assert_backlog_state() {
    local expected_active_height="$1"
    local expected_header_height="$2"
    local expected_header_count="$3"
    local expected_active_hash="$4"
    local label="$5"

    local health
    health="$(sync_health)"

    jq -e \
        --argjson active_height "${expected_active_height}" \
        --argjson header_height "${expected_header_height}" \
        --argjson header_count "${expected_header_count}" \
        --arg active_hash "${expected_active_hash}" \
        --argjson expected_ckpt "$(( expected_active_height - (expected_active_height % 5) ))" \
        '
        .active_height == $active_height and
        .chaindb_tip_height == $active_height and
        .latest_utreexo_checkpoint_found == true and
        .latest_utreexo_checkpoint_height == $expected_ckpt and
        .latest_utreexo_checkpoint_has_checksum == true and
        .forest_tip_marker_found == true and
        .forest_tip_marker_height == $active_height and
        .canonical_state_aligned == true and
        .active_best_hash == $active_hash and
        .chaindb_tip_hash == $active_hash and
        .forest_tip_marker_hash == $active_hash and
        .header_selector.available == true and
        .header_selector.best_height == $header_height and
        .header_selector.header_count == $header_count and
        (
            (.header_selector.best_height == $active_height and .header_selector.best_hash == $active_hash) or
            (.header_selector.best_height > $active_height and .header_selector.best_hash != $active_hash)
        ) and
        .header_store.available == true and
        .header_store.persisted_best_height == $header_height and
        .header_store.persisted_best_hash == .header_selector.best_hash and
        .header_store.persisted_header_count == $header_count
        ' <<<"${health}" >/dev/null || fail "${label}: unexpected sync health ${health}"
}

require_tools

# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT=$(alloc_port_base)
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node "${LOG_BASE}"
wait_rpc || fail "base daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

info "Building canonical chain before injecting header-only backlog"
mine_blocks 24 "${MINER_ADDR}"
BASE_HASH_RESULT="$(rpc_call "${DATA_DIR}" "getbestblockhash" '[]')"
rpc_has_error "${BASE_HASH_RESULT}" && fail "getbestblockhash failed: ${BASE_HASH_RESULT}"
BASE_HASH="$(jq -r '.result // empty' <<<"${BASE_HASH_RESULT}")"
[[ -n "${BASE_HASH}" ]] || fail "baseline best hash missing"
assert_backlog_state 24 24 25 "${BASE_HASH}" "baseline state"
pass "Baseline canonical state is aligned at height 24"

stop_node

info "Injecting a 2-header backlog into the persisted HeaderStore without block bodies"
INJECT_OUTPUT="$("${BACKLOG_INJECTOR}" "${DATA_DIR}/headers" --count 2 --network regtest)"
[[ "${INJECT_OUTPUT}" == *"mode=header-backlog-injected"* ]] || fail "injector did not report success: ${INJECT_OUTPUT}"
[[ "${INJECT_OUTPUT}" == *"final_height=26"* ]] || fail "injector did not advance best header to 26: ${INJECT_OUTPUT}"
pass "Header-only backlog injected into persisted HeaderStore"

start_node "${LOG_RESTART1}"
wait_rpc || fail "restart1 daemon did not reach RPC readiness"
assert_backlog_state 24 26 27 "${BASE_HASH}" "restart1 state"
pass "Restart 1 preserved header backlog without promoting canonical tip"

stop_node

start_node "${LOG_RESTART2}"
wait_rpc || fail "restart2 daemon did not reach RPC readiness"
assert_backlog_state 24 26 27 "${BASE_HASH}" "restart2 state"
pass "Restart 2 preserved the same separation between header backlog and canonical state"

stop_node
