#!/usr/bin/env bash
#
# Historical undo metadata re-stamp regression.
#
# Proves the operator path:
#   1. A healthy regtest node mines blocks with readable undo bytes.
#   2. A regtest-only debug hook clears BLOCK_HAVE_UNDO for the active tip
#      while preserving undo_file/undo_pos/undo_size.
#   3. Restart surfaces the startup audit marker.
#   4. blockchain.auditundometadata dry-run reports the block as restampable.
#   5. apply=true re-stamps the flag.
#   6. After removing the stale marker and restarting, startup audit does not
#      recreate it.
#
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
RUN_ID=$$
DATA_DIR="/tmp/dinero_undo_metadata_restamp_${RUN_ID}"
LOG_NODE="${DATA_DIR}.node.log"
RPC_PORT=$((36000 + RUN_ID % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_NODE}" ]] && { printf -- '--- node log tail ---\n' >&2; tail -180 "${LOG_NODE}" >&2 || true; }
    exit 1
}

cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_NODE}"
    else
        printf '[INFO] Keeping artifacts for inspection: %s\n' "${DATA_DIR}" >&2
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    if [[ -f "${DATA_DIR}/.cookie" ]]; then
        printf '%s\n' "${DATA_DIR}/.cookie"
        return 0
    fi
    if [[ -f "${DATA_DIR}/regtest/.cookie" ]]; then
        printf '%s\n' "${DATA_DIR}/regtest/.cookie"
        return 0
    fi
    return 1
}

rpc_call() {
    local method="$1"
    local params_json="$2"
    local cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
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

rpc_result() {
    local method="$1"
    local params_json="$2"
    local response
    response="$(rpc_call "${method}" "${params_json}")"
    rpc_has_error "${response}" && fail "${method} failed: ${response}"
    printf '%s\n' "${response}"
}

rpc_scalar() {
    local method="$1"
    local params_json="$2"
    local jq_filter="$3"
    rpc_result "${method}" "${params_json}" | jq -r "${jq_filter}"
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
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        >>"${LOG_NODE}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    kill -TERM "${PID}" 2>/dev/null || true
    wait_dead "${PID}" || fail "daemon did not stop after SIGTERM"
    wait "${PID}" 2>/dev/null || true
    PID=""
}

require_tools
: >"${LOG_NODE}"

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" '["taproot","restamp-miner"]' '.result.address // .result // empty')"
[[ -n "${MINER_ADDR}" && "${MINER_ADDR}" != "null" ]] || fail "empty miner address"

info "Mining baseline blocks"
rpc_result "generatetoaddress" "[3,\"${MINER_ADDR}\"]" >/dev/null
TIP_HEIGHT="$(rpc_scalar "getblockcount" '[]' '.result')"
TIP_HASH="$(rpc_scalar "getbestblockhash" '[]' '.result')"
[[ "${TIP_HEIGHT}" == "3" ]] || fail "expected height 3, got ${TIP_HEIGHT}"

info "Clearing BLOCK_HAVE_UNDO for active tip ${TIP_HASH:0:16}..."
rpc_result "blockchain.debugclearundoflag" "[\"${TIP_HASH}\"]" >/dev/null
stop_node

start_node
wait_rpc || fail "daemon did not restart after metadata corruption"
[[ -f "${DATA_DIR}/chainstate_recovery.marker" ]] || \
    fail "startup audit did not write recovery marker for missing BLOCK_HAVE_UNDO"

DRY_RUN="$(rpc_result "blockchain.auditundometadata" '[{"max_blocks":8,"apply":false}]')"
jq -e '.result.apply == false and .result.restampable == 1 and .result.repaired == 0' <<<"${DRY_RUN}" >/dev/null || \
    fail "dry-run did not report exactly one restampable block: ${DRY_RUN}"
jq -e --arg hash "${TIP_HASH}" '.result.entries[] | select(.hash == $hash and .restampable == true and .has_undo_flag == false)' <<<"${DRY_RUN}" >/dev/null || \
    fail "dry-run entry did not identify corrupted tip: ${DRY_RUN}"
pass "Dry-run identified stale undo metadata without mutating"

APPLY="$(rpc_result "blockchain.auditundometadata" '[{"max_blocks":8,"apply":true}]')"
jq -e '.result.apply == true and .result.repaired == 1 and .result.failed == 0' <<<"${APPLY}" >/dev/null || \
    fail "apply did not re-stamp exactly one block: ${APPLY}"
jq -e --arg hash "${TIP_HASH}" '.result.entries[] | select(.hash == $hash and .repaired == true and .has_undo_flag == true)' <<<"${APPLY}" >/dev/null || \
    fail "apply entry did not show repaired tip: ${APPLY}"
pass "Apply re-stamped BLOCK_HAVE_UNDO"

rm -f "${DATA_DIR}/chainstate_recovery.marker"
stop_node

start_node
wait_rpc || fail "daemon did not restart after re-stamp"
[[ ! -f "${DATA_DIR}/chainstate_recovery.marker" ]] || \
    fail "startup audit recreated recovery marker after re-stamp"

VERIFY="$(rpc_result "blockchain.auditundometadata" '[{"max_blocks":8,"apply":false}]')"
jq -e '.result.restampable == 0 and .result.failed == 0' <<<"${VERIFY}" >/dev/null || \
    fail "post-restart audit still reports restamp work: ${VERIFY}"

pass "Undo metadata re-stamp survived restart and startup audit stayed quiet"
stop_node
