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
DATA_DIR="/tmp/dinero_invalidity_crash_restart_$$"
LOG_BASE="${DATA_DIR}.base.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_RESTART="${DATA_DIR}.restart.log"
LOG_RECONSIDER_CRASH="${DATA_DIR}.reconsider_crash.log"
LOG_RECONSIDER_RESTART="${DATA_DIR}.reconsider_restart.log"
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
    [[ -f "${LOG_RECONSIDER_CRASH}" ]] && { printf -- '--- reconsider crash log tail ---\n' >&2; tail -120 "${LOG_RECONSIDER_CRASH}" >&2 || true; }
    [[ -f "${LOG_RECONSIDER_RESTART}" ]] && { printf -- '--- reconsider restart log tail ---\n' >&2; tail -160 "${LOG_RECONSIDER_RESTART}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_BASE}" "${LOG_CRASH}" "${LOG_RESTART}" \
            "${LOG_RECONSIDER_CRASH}" "${LOG_RECONSIDER_RESTART}"
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

get_block_hash() {
    local height="$1"
    local result
    result="$(rpc_call "${DATA_DIR}" "getblockhash" "[${height}]")"
    rpc_has_error "${result}" && fail "getblockhash(${height}) failed: ${result}"
    jq -r '.result // empty' <<<"${result}"
}

get_block_header() {
    local hash="$1"
    local result
    result="$(rpc_call "${DATA_DIR}" "getblockheader" "[\"${hash}\"]")"
    rpc_has_error "${result}" && fail "getblockheader(${hash}) failed: ${result}"
    jq -c '.' <<<"${result}"
}

assert_header_flags() {
    local hash="$1"
    local expected_failed_valid="$2"
    local expected_failed_child="$3"
    local label="$4"
    local header
    header="$(get_block_header "${hash}")"
    jq -e \
        --argjson expected_failed_valid "${expected_failed_valid}" \
        --argjson expected_failed_child "${expected_failed_child}" \
        '
        .error == null and
        .result.failed_valid == $expected_failed_valid and
        .result.failed_child == $expected_failed_child and
        (.result.status_flags | type) == "number"
        ' <<<"${header}" >/dev/null || fail "${label} flags mismatch: ${header}"
}

assert_height() {
    local expected="$1"
    local label="$2"
    local result
    result="$(rpc_call "${DATA_DIR}" "getblockcount" '[]')"
    rpc_has_error "${result}" && fail "getblockcount failed: ${result}"
    local actual
    actual="$(jq -r '.result' <<<"${result}")"
    [[ "${actual}" == "${expected}" ]] || fail "${label}: expected height ${expected}, got ${actual}"
}

assert_best_hash() {
    local expected="$1"
    local label="$2"
    local result
    result="$(rpc_call "${DATA_DIR}" "getbestblockhash" '[]')"
    rpc_has_error "${result}" && fail "getbestblockhash failed: ${result}"
    local actual
    actual="$(jq -r '.result // empty' <<<"${result}")"
    [[ "${actual}" == "${expected}" ]] || fail "${label}: expected best hash ${expected}, got ${actual}"
}

require_tools

RPC_PORT=$((37000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
INVALIDITY_RECONSIDER_CRASH="${INVALIDITY_RECONSIDER_CRASH:-0}"

start_node "${LOG_BASE}"
wait_rpc || fail "base daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

info "Building branch for invalidity crash/restart equivalence test"
mine_blocks 24 "${MINER_ADDR}"

TARGET_HASH="$(get_block_hash 22)"
CHILD_HASH="$(get_block_hash 23)"
TIP_HASH="$(get_block_hash 24)"
PARENT_HASH="$(get_block_hash 21)"

[[ -n "${TARGET_HASH}" && -n "${CHILD_HASH}" && -n "${TIP_HASH}" && -n "${PARENT_HASH}" ]] || \
    fail "failed to capture branch hashes"

stop_node

start_node "${LOG_CRASH}" DINERO_CRASH_AT=after_invalid_target_before_descendants
wait_rpc || fail "crash-test daemon did not reach RPC readiness"

info "Triggering crash after target invalidity persistence but before descendant propagation"
set +e
CRASH_TRIGGER_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.invalidateblock" "[\"${TARGET_HASH}\"]" 2>/dev/null)"
CRASH_TRIGGER_STATUS=$?
set -e
if [[ -n "${CRASH_TRIGGER_RESULT}" ]] && rpc_has_error "${CRASH_TRIGGER_RESULT}"; then
    fail "invalidateblock failed before crash trigger: ${CRASH_TRIGGER_RESULT}"
fi
wait_dead "${PID}" || fail "daemon did not crash at after_invalid_target_before_descendants"
PID=""

grep -q "DINERO_CRASH" "${LOG_CRASH}" || fail "crash log did not show named crash hook"
pass "Crash hook triggered after persisting the target invalid flag"

start_node "${LOG_RESTART}"
wait_rpc || fail "restarted daemon did not reach RPC readiness"

assert_height 21 "post-restart active height"
assert_best_hash "${PARENT_HASH}" "post-restart best hash"
assert_header_flags "${TARGET_HASH}" true false "target after restart"
assert_header_flags "${CHILD_HASH}" false true "child after restart"
assert_header_flags "${TIP_HASH}" false true "tip after restart"
pass "Restart completed descendant invalidity propagation after crash"

if [[ "${INVALIDITY_RECONSIDER_CRASH}" == "1" ]]; then
    stop_node

    start_node "${LOG_RECONSIDER_CRASH}" DINERO_CRASH_AT=after_reconsider_target_before_descendants
    wait_rpc || fail "reconsider-crash daemon did not reach RPC readiness"

    info "Triggering crash after staging reconsider target clear but before descendant propagation"
    set +e
    RECONSIDER_CRASH_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.reconsiderblock" "[\"${TARGET_HASH}\"]" 2>/dev/null)"
    set -e
    if [[ -n "${RECONSIDER_CRASH_RESULT}" ]] && rpc_has_error "${RECONSIDER_CRASH_RESULT}"; then
        fail "reconsiderblock failed before crash trigger: ${RECONSIDER_CRASH_RESULT}"
    fi
    wait_dead "${PID}" || fail "daemon did not crash at after_reconsider_target_before_descendants"
    PID=""

    grep -q "DINERO_CRASH" "${LOG_RECONSIDER_CRASH}" || fail "reconsider crash log did not show named crash hook"
    pass "Crash hook triggered during reconsider before descendant persistence"

    start_node "${LOG_RECONSIDER_RESTART}"
    wait_rpc || fail "reconsider-restart daemon did not reach RPC readiness"

    assert_height 21 "post-reconsider-crash active height"
    assert_best_hash "${PARENT_HASH}" "post-reconsider-crash best hash"
    assert_header_flags "${TARGET_HASH}" true false "target after reconsider crash restart"
    assert_header_flags "${CHILD_HASH}" false true "child after reconsider crash restart"
    assert_header_flags "${TIP_HASH}" false true "tip after reconsider crash restart"
    pass "Restart kept invalidity sticky after a mid-reconsider crash"
fi

RECONSIDER_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.reconsiderblock" "[\"${TARGET_HASH}\"]")"
rpc_has_error "${RECONSIDER_RESULT}" && fail "reconsiderblock failed: ${RECONSIDER_RESULT}"

assert_height 24 "post-reconsider active height"
assert_best_hash "${TIP_HASH}" "post-reconsider best hash"
assert_header_flags "${TARGET_HASH}" false false "target after reconsider"
assert_header_flags "${CHILD_HASH}" false false "child after reconsider"
assert_header_flags "${TIP_HASH}" false false "tip after reconsider"
pass "ReconsiderBlock restored the branch after crash-safe recovery"

stop_node
