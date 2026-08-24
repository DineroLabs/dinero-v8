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
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
# Honour $MUTATOR when set (and require it executable); the chain
# below never consulted it, so ctest could not point at a build
# directory and the final fallback resolved to a path that does
# not exist.
if [[ -n "${MUTATOR:-}" ]]; then
    [[ -x "${MUTATOR}" ]] || { echo "utreexo_checkpoint_mutator not executable at ${MUTATOR}" >&2; exit 1; }
elif [[ -x "${ROOT_DIR}/build/tests/integration/utreexo_checkpoint_mutator" ]]; then
    MUTATOR="${ROOT_DIR}/build/tests/integration/utreexo_checkpoint_mutator"
elif [[ -x "${ROOT_DIR}/build/utreexo_checkpoint_mutator" ]]; then
    MUTATOR="${ROOT_DIR}/build/utreexo_checkpoint_mutator"
else
    MUTATOR="${ROOT_DIR}/utreexo_checkpoint_mutator"
fi

TMP_ROOT="$(mktemp -d /tmp/dinero_reorg_fail_safe.XXXXXX)"
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
    [[ -f "${LOG_BASE}" ]] && { printf -- '--- base log tail ---\n' >&2; tail -80 "${LOG_BASE}" >&2 || true; }
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
    [[ -x "${MUTATOR}" ]] || fail "utreexo_checkpoint_mutator not built at ${MUTATOR}"
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
            .forest_tip_marker_found == true and
            .forest_tip_marker_height == $expected_height and
            .chaindb_tip_hash == .active_best_hash and
            .forest_tip_marker_hash == .active_best_hash
            ' <<<"${health}" >/dev/null; then
            return 0
        fi
        sleep 1
    done
    fail "${label}: timed out waiting for canonical state ${expected_height}"
}

require_tools

RPC_PORT=$((43000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node "${LOG_BASE}"
wait_rpc || fail "Initial daemon start did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
if rpc_has_error "${ADDR_RESULT}"; then
    fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
fi
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

info "Building a checkpointed chain before forcing interrupted-reorg fail-safe"
mine_blocks 260 "${MINER_ADDR}"
wait_for_sync_state 260 "pre-fail-safe steady state"
pass "Initial daemon startup succeeded with aligned state at height 260"

stop_node

CHAINDB_DIR="${DATA_DIR}/blockchain/chaindb"
[[ -d "${CHAINDB_DIR}" ]] || fail "Could not find ChainDB directory at ${CHAINDB_DIR}"
UTXO_DB="${DATA_DIR}/blockchain/utxo"
[[ -f "${UTXO_DB}" ]] || fail "Could not find UTXO sqlite database at ${UTXO_DB}"

MUTATOR_OUTPUT="$("${MUTATOR}" "${CHAINDB_DIR}" --source-offset-back 1)" || fail "Checkpoint mutator failed"
[[ "${MUTATOR_OUTPUT}" == *"mode=stale-latest-checkpoint"* ]] || fail "Checkpoint mutator did not report success: ${MUTATOR_OUTPUT}"
pass "Contaminated latest Utreexo checkpoint while preserving checksum validity"

sqlite3 "${UTXO_DB}" "INSERT OR REPLACE INTO utxo_metadata(key, value) VALUES ('reorg_in_progress', '260:260:260');"
[[ "$(sqlite3 "${UTXO_DB}" "SELECT value FROM utxo_metadata WHERE key='reorg_in_progress' LIMIT 1;")" == "260:260:260" ]] \
    || fail "Failed to inject interrupted reorg marker into ${UTXO_DB}"
pass "Injected interrupted reorg marker into ${UTXO_DB}"

start_node "${LOG_RESTART}"
sleep 5

if rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
    stop_node
    fail "Daemon reached RPC readiness despite inconsistent reorg marker state"
fi

if kill -0 "${PID}" 2>/dev/null; then
    wait_dead "${PID}" || true
fi
if kill -0 "${PID}" 2>/dev/null; then
    stop_node
    fail "Daemon stayed running despite interrupted reorg marker and corrupted checkpoint"
fi
PID=""

grep -q "Reorg marker found" "${LOG_RESTART}" || fail "Startup log did not mention interrupted reorg marker"
grep -q "Auto-recovery failed" "${LOG_RESTART}" || fail "Startup log did not report failed auto-recovery"
grep -q "Startup is aborted" "${LOG_RESTART}" || fail "Startup log did not confirm fail-safe abort"
[[ "$(sqlite3 "${UTXO_DB}" "SELECT value FROM utxo_metadata WHERE key='reorg_in_progress' LIMIT 1;")" == "260:260:260" ]] \
    || fail "Interrupted reorg marker was unexpectedly cleared after fail-safe abort"
pass "Daemon refused startup on interrupted reorg marker with inconsistent checkpoint state"
