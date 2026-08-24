#!/usr/bin/env bash
#
# D.3-full regression: missing undo for an active tip that contains a normal
# user transaction must regenerate from txindex + parent block bodies, not wedge.
#
# Shape:
#   1. Mine enough blocks for spendable wallet funds.
#   2. Create a wallet spend and mine it as the active tip.
#   3. Stop the daemon and remove the regtest rev*.dat undo bytes.
#   4. Restart and invalidate the user-tx tip.
#   5. DisconnectTip must take the full prevout-lookup regeneration path.
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
fi
RUN_ID=$$
DATA_DIR="/tmp/dinero_d3_full_user_tx_missing_undo_${RUN_ID}"
LOG_NODE="${DATA_DIR}.node.log"
RPC_PORT=$((35000 + RUN_ID % 1000))
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

mine_blocks() {
    local n="$1"
    local address="$2"
    rpc_result "generatetoaddress" "[${n},\"${address}\"]" >/dev/null
}

corrupt_undo_flatfiles() {
    local rev_files=()
    while IFS= read -r path; do
        rev_files+=("${path}")
    done < <(find "${DATA_DIR}" -path "*/blocks/rev*.dat" -type f | sort)

    [[ "${#rev_files[@]}" -gt 0 ]] || fail "no rev*.dat files found to corrupt"

    for rev in "${rev_files[@]}"; do
        : >"${rev}"
    done
    info "Truncated ${#rev_files[@]} undo flatfile(s) to force ReadStoredUndo failure"
}

require_tools
: >"${LOG_NODE}"

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" '["taproot","d3-full-miner"]' '.result.address // .result // empty')"
[[ -n "${MINER_ADDR}" && "${MINER_ADDR}" != "null" ]] || fail "empty miner address"
RECIPIENT_ADDR="$(rpc_scalar "wallet.getnewaddress" '["taproot","d3-full-recipient"]' '.result.address // .result // empty')"
[[ -n "${RECIPIENT_ADDR}" && "${RECIPIENT_ADDR}" != "null" ]] || fail "empty recipient address"

info "Mining mature wallet balance"
mine_blocks 110 "${MINER_ADDR}"

SEND_TXID="$(rpc_scalar "wallet.sendtoaddress" "[\"${RECIPIENT_ADDR}\",1.0]" '.result.txid // .result // empty')"
[[ -n "${SEND_TXID}" && "${SEND_TXID}" != "null" ]] || fail "wallet.sendtoaddress returned empty txid"
info "Created wallet spend ${SEND_TXID:0:16}..."

HEIGHT_BEFORE_TIP="$(rpc_scalar "getblockcount" '[]' '.result')"
mine_blocks 1 "${MINER_ADDR}"
TIP_HEIGHT="$(rpc_scalar "getblockcount" '[]' '.result')"
[[ "${TIP_HEIGHT}" == "$((HEIGHT_BEFORE_TIP + 1))" ]] || \
    fail "mined user-tx tip height mismatch: before=${HEIGHT_BEFORE_TIP} after=${TIP_HEIGHT}"
TIP_HASH="$(rpc_scalar "getblockhash" "[${TIP_HEIGHT}]" '.result')"
info "User-tx tip is height=${TIP_HEIGHT} hash=${TIP_HASH:0:16}..."

stop_node
corrupt_undo_flatfiles

start_node
wait_rpc || fail "daemon did not reach RPC readiness after undo corruption"

RESTART_HEIGHT="$(rpc_scalar "getblockcount" '[]' '.result')"
[[ "${RESTART_HEIGHT}" == "${TIP_HEIGHT}" ]] || \
    fail "restart did not preserve active tip before invalidation: expected=${TIP_HEIGHT} got=${RESTART_HEIGHT}"

info "Invalidating user-tx tip to force D.3-full regeneration"
INVALIDATE_RESULT="$(rpc_call "blockchain.invalidateblock" "[\"${TIP_HASH}\"]")"
rpc_has_error "${INVALIDATE_RESULT}" && fail "invalidateblock failed instead of regenerating undo: ${INVALIDATE_RESULT}"

POST_HEIGHT="$(rpc_scalar "getblockcount" '[]' '.result')"
[[ "${POST_HEIGHT}" == "${HEIGHT_BEFORE_TIP}" ]] || \
    fail "invalidate did not disconnect exactly one block: expected=${HEIGHT_BEFORE_TIP} got=${POST_HEIGHT}"

grep -q "ReadStoredUndo failed but block undo regenerated via prevout txindex lookup" "${LOG_NODE}" || \
    fail "D.3-full prevout regeneration log line not observed"

pass "D.3-full regenerated missing undo for a user-tx-bearing active tip"
stop_node
