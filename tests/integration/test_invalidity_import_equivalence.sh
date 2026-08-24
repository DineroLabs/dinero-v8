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
LIVE_DIR="/tmp/dinero_invalidity_live_$$"
IMPORT_DIR="/tmp/dinero_invalidity_import_$$"
LOG_LIVE="${LIVE_DIR}.log"
LOG_IMPORT="${IMPORT_DIR}.log"
STATE_LIVE_FILE="${LIVE_DIR}.state.json"
STATE_IMPORT_FILE="${IMPORT_DIR}.state.json"
PID=""
KEEP_ON_FAIL=0
CURRENT_DATADIR=""
CURRENT_RPC_PORT=""
CURRENT_P2P_PORT=""
CURRENT_WALLET_PORT=""

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_LIVE}" ]] && { printf -- '--- live log tail ---\n' >&2; tail -120 "${LOG_LIVE}" >&2 || true; }
    [[ -f "${LOG_IMPORT}" ]] && { printf -- '--- import log tail ---\n' >&2; tail -160 "${LOG_IMPORT}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${LIVE_DIR}" 2>/dev/null || true
    pkill -f "dinerod.*${IMPORT_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${LIVE_DIR}" "${IMPORT_DIR}" "${LOG_LIVE}" "${LOG_IMPORT}"
        rm -f "${STATE_LIVE_FILE}" "${STATE_IMPORT_FILE}"
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
    local method="$1"
    local params_json="$2"
    local cookie_path
    cookie_path="$(cookie_file "${CURRENT_DATADIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${CURRENT_RPC_PORT}/"
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
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
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
    local datadir="$1"
    local log_file="$2"
    local rpc_port="$3"
    local p2p_port="$4"
    local wallet_port="$5"
    shift 5

    mkdir -p "${datadir}"
    CURRENT_DATADIR="${datadir}"
    CURRENT_RPC_PORT="${rpc_port}"
    CURRENT_P2P_PORT="${p2p_port}"
    CURRENT_WALLET_PORT="${wallet_port}"

    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --wallet-socket-port="${wallet_port}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        "$@" \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "stop" '[]' 2>/dev/null || true)"
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
    result="$(rpc_call "generatetoaddress" "[${blocks},\"${address}\"]")"
    rpc_has_error "${result}" && fail "generatetoaddress failed: ${result}"
    return 0
}

get_block_hash() {
    local height="$1"
    local result
    result="$(rpc_call "getblockhash" "[${height}]")"
    rpc_has_error "${result}" && fail "getblockhash(${height}) failed: ${result}"
    jq -r '.result // empty' <<<"${result}"
}

get_block_header() {
    local hash="$1"
    local result
    result="$(rpc_call "getblockheader" "[\"${hash}\"]")"
    rpc_has_error "${result}" && fail "getblockheader(${hash}) failed: ${result}"
    jq -c '.result' <<<"${result}"
}

sync_health() {
    local result
    result="$(rpc_call "blockchain.getsynchealth" '[]')"
    rpc_has_error "${result}" && fail "blockchain.getsynchealth failed: ${result}"
    jq -c '.result' <<<"${result}"
}

assert_height() {
    local expected="$1"
    local label="$2"
    local result
    result="$(rpc_call "getblockcount" '[]')"
    rpc_has_error "${result}" && fail "getblockcount failed: ${result}"
    local actual
    actual="$(jq -r '.result' <<<"${result}")"
    [[ "${actual}" == "${expected}" ]] || fail "${label}: expected height ${expected}, got ${actual}"
}

assert_best_hash() {
    local expected="$1"
    local label="$2"
    local result
    result="$(rpc_call "getbestblockhash" '[]')"
    rpc_has_error "${result}" && fail "getbestblockhash failed: ${result}"
    local actual
    actual="$(jq -r '.result // empty' <<<"${result}")"
    [[ "${actual}" == "${expected}" ]] || fail "${label}: expected best hash ${expected}, got ${actual}"
}

capture_invalidity_bundle() {
    local parent_hash="$1"
    local target_hash="$2"
    local child_hash="$3"
    local tip_hash="$4"
    local health parent_header target_header child_header tip_header

    health="$(sync_health)"
    parent_header="$(get_block_header "${parent_hash}")"
    target_header="$(get_block_header "${target_hash}")"
    child_header="$(get_block_header "${child_hash}")"
    tip_header="$(get_block_header "${tip_hash}")"

    jq -n \
        --argjson health "${health}" \
        --argjson parent "${parent_header}" \
        --argjson target "${target_header}" \
        --argjson child "${child_header}" \
        --argjson tip "${tip_header}" \
        '{
            sync_health: {
                active_height: $health.active_height,
                active_best_hash: $health.active_best_hash,
                chaindb_tip_height: $health.chaindb_tip_height,
                chaindb_tip_hash: $health.chaindb_tip_hash,
                canonical_state_aligned: $health.canonical_state_aligned
            },
            parent: {
                height: $parent.height,
                hash: $parent.hash,
                previousblockhash: $parent.previousblockhash,
                chainwork: $parent.chainwork,
                status_flags: $parent.status_flags,
                failed_valid: $parent.failed_valid,
                failed_child: $parent.failed_child
            },
            target: {
                height: $target.height,
                hash: $target.hash,
                previousblockhash: $target.previousblockhash,
                chainwork: $target.chainwork,
                status_flags: $target.status_flags,
                failed_valid: $target.failed_valid,
                failed_child: $target.failed_child
            },
            child: {
                height: $child.height,
                hash: $child.hash,
                previousblockhash: $child.previousblockhash,
                chainwork: $child.chainwork,
                status_flags: $child.status_flags,
                failed_valid: $child.failed_valid,
                failed_child: $child.failed_child
            },
            tip: {
                height: $tip.height,
                hash: $tip.hash,
                previousblockhash: $tip.previousblockhash,
                chainwork: $tip.chainwork,
                status_flags: $tip.status_flags,
                failed_valid: $tip.failed_valid,
                failed_child: $tip.failed_child
            }
        }'
}

assert_same_bundle() {
    local lhs_file="$1"
    local rhs_file="$2"
    if ! jq -s -e '.[0] == .[1]' "${lhs_file}" "${rhs_file}" >/dev/null; then
        printf -- '--- live bundle ---\n' >&2
        cat "${lhs_file}" >&2
        printf -- '\n--- imported bundle ---\n' >&2
        cat "${rhs_file}" >&2
        fail "invalidity import bundle mismatch"
    fi
}

require_tools

# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
LIVE_RPC_PORT=$(alloc_port_base)
LIVE_P2P_PORT=$((LIVE_RPC_PORT + 1))
LIVE_WALLET_PORT=$((LIVE_RPC_PORT + 2))
IMPORT_RPC_PORT=$((LIVE_RPC_PORT + 10))
IMPORT_P2P_PORT=$((IMPORT_RPC_PORT + 1))
IMPORT_WALLET_PORT=$((IMPORT_RPC_PORT + 2))

start_node "${LIVE_DIR}" "${LOG_LIVE}" "${LIVE_RPC_PORT}" "${LIVE_P2P_PORT}" "${LIVE_WALLET_PORT}"
wait_rpc || fail "live daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

info "Building invalidated branch on live node"
mine_blocks 24 "${MINER_ADDR}"

TARGET_HASH="$(get_block_hash 22)"
CHILD_HASH="$(get_block_hash 23)"
TIP_HASH="$(get_block_hash 24)"
PARENT_HASH="$(get_block_hash 21)"

[[ -n "${TARGET_HASH}" && -n "${CHILD_HASH}" && -n "${TIP_HASH}" && -n "${PARENT_HASH}" ]] || \
    fail "failed to capture branch hashes"

INVALIDATE_RESULT="$(rpc_call "blockchain.invalidateblock" "[\"${TARGET_HASH}\"]")"
rpc_has_error "${INVALIDATE_RESULT}" && fail "invalidateblock failed: ${INVALIDATE_RESULT}"

assert_height 21 "live post-invalidate height"
assert_best_hash "${PARENT_HASH}" "live post-invalidate best hash"
capture_invalidity_bundle "${PARENT_HASH}" "${TARGET_HASH}" "${CHILD_HASH}" "${TIP_HASH}" > "${STATE_LIVE_FILE}"
pass "Captured live invalidity bundle"

stop_node

info "Cloning persisted state into import datadir"
cp -a "${LIVE_DIR}" "${IMPORT_DIR}"
rm -f "${IMPORT_DIR}/.cookie" "${IMPORT_DIR}/regtest/.cookie"

start_node "${IMPORT_DIR}" "${LOG_IMPORT}" "${IMPORT_RPC_PORT}" "${IMPORT_P2P_PORT}" "${IMPORT_WALLET_PORT}"
wait_rpc || fail "import daemon did not reach RPC readiness"

capture_invalidity_bundle "${PARENT_HASH}" "${TARGET_HASH}" "${CHILD_HASH}" "${TIP_HASH}" > "${STATE_IMPORT_FILE}"
assert_same_bundle "${STATE_LIVE_FILE}" "${STATE_IMPORT_FILE}"
pass "Imported daemon rebuilt identical invalidity metadata from persisted state"

RECONSIDER_RESULT="$(rpc_call "blockchain.reconsiderblock" "[\"${TARGET_HASH}\"]")"
rpc_has_error "${RECONSIDER_RESULT}" && fail "reconsiderblock failed: ${RECONSIDER_RESULT}"

assert_height 24 "import post-reconsider active height"
assert_best_hash "${TIP_HASH}" "import post-reconsider best hash"
mine_blocks 1 "${MINER_ADDR}"
pass "Imported daemon advanced cleanly after invalidity equivalence check"

stop_node
