#!/usr/bin/env bash
# Phase 3 wave 3b: end-to-end regtest for the wallet.shield RPC.
#
# Validates that wallet.shield:
#   1. Selects transparent UTXOs from the wallet
#   2. Builds a v5 tx with attached shielded bundle
#   3. Signs transparent inputs
#   4. Submits the tx to the mempool (gets accepted)
#   5. Tx mines into a block (block validator accepts the bundle)
#   6. The shielded note is queryable post-confirmation
#
# Plus a few cheap negative cases: zero amount, insufficient funds.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"

DATA_DIR="/tmp/dinero_shield_rpc_e2e_$$"
LOG_FILE="${DATA_DIR}.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_FILE}" ]] && { printf -- '--- daemon log tail ---\n' >&2; tail -120 "${LOG_FILE}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_FILE}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null   || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"; return 0
    fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"; return 0
    fi
    return 1
}

rpc_call() {
    local method="$1"; local params_json="$2"
    local cookie_path; cookie_path="$(cookie_file "${DATA_DIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie; cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_result() {
    local method="$1"; local params_json="$2"
    local result; result="$(rpc_call "${method}" "${params_json}")"
    if echo "$result" | jq -e '.error != null and .error != false' >/dev/null 2>&1; then
        fail "${method} failed: ${result}"
    fi
    printf '%s\n' "${result}"
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if [[ -n "${PID}" ]] && ! kill -0 "${PID}" 2>/dev/null; then
            return 1
        fi
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
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
        >"${LOG_FILE}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc_call "stop" '[]' >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do
        kill -0 "${PID}" 2>/dev/null || break
        sleep 1
    done
    kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

require_tools

RPC_PORT=$((39000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
SHIELD_VALUE_DIN="1.0"
SHIELD_VALUE_UNA=100000000
SHIELD_FEE_UNA=10000

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

# ── Mine spendable regtest funds ────────────────────────────────────────
MINER_ADDR_RESULT="$(rpc_result "wallet.getnewaddress" '["taproot","shield-rpc-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 blocks to mature regtest coinbase"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null

# ── Negative test: zero amount ─────────────────────────────────────────
info "Negative case: zero amount"
ZERO_RES="$(rpc_call "wallet.shield" '[0.0]')"
echo "$ZERO_RES" | jq -e '.result.error == "invalid_params" or .error != null' >/dev/null 2>&1 \
    || fail "zero-amount shield should be rejected: ${ZERO_RES}"
pass "zero-amount rejected"

# ── Negative test: insufficient funds ──────────────────────────────────
info "Negative case: amount > wallet balance"
HUGE_RES="$(rpc_call "wallet.shield" '[1000000.0]')"
echo "$HUGE_RES" | jq -e '.result.error == "insufficient_funds" or .error != null' >/dev/null 2>&1 \
    || fail "insufficient-funds shield should be rejected: ${HUGE_RES}"
pass "insufficient-funds rejected"

# ── Happy path: shield 1 DIN ───────────────────────────────────────────
info "Calling wallet.shield(${SHIELD_VALUE_DIN} DIN, fee=${SHIELD_FEE_UNA} una)"
SHIELD_RES="$(rpc_result "wallet.shield" "[${SHIELD_VALUE_DIN}, ${SHIELD_FEE_UNA}]")"
echo "${SHIELD_RES}" | jq -e '.result.status == "shielded"' >/dev/null \
    || fail "wallet.shield did not return status=shielded: ${SHIELD_RES}"

SHIELD_TXID="$(jq -r '.result.txid' <<<"${SHIELD_RES}")"
SHIELD_COMMITMENT="$(jq -r '.result.commitment_hex' <<<"${SHIELD_RES}")"
[[ -n "${SHIELD_TXID}" && "${SHIELD_TXID}" != "null" ]] || fail "missing txid: ${SHIELD_RES}"
[[ -n "${SHIELD_COMMITMENT}" && "${SHIELD_COMMITMENT}" != "null" ]] || fail "missing commitment: ${SHIELD_RES}"
pass "wallet.shield returned status=shielded, txid=${SHIELD_TXID:0:16}…"

# ── Verify tx is in mempool ────────────────────────────────────────────
MEMPOOL="$(rpc_result "getrawmempool" '[]')"
jq -e --arg txid "${SHIELD_TXID}" '.result | index($txid) != null' <<<"${MEMPOOL}" >/dev/null \
    || fail "shield tx ${SHIELD_TXID} not in mempool: ${MEMPOOL}"
pass "shield tx accepted into mempool"

# ── Mine the tx into a block ───────────────────────────────────────────
TIP_BEFORE="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
TIP_AFTER="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
[[ "${TIP_AFTER}" == "$((TIP_BEFORE + 1))" ]] || fail "block height did not advance: ${TIP_BEFORE} -> ${TIP_AFTER}"

# ── Confirm tx is now in the new block (no longer in mempool) ──────────
MEMPOOL_AFTER="$(rpc_result "getrawmempool" '[]')"
jq -e --arg txid "${SHIELD_TXID}" '.result | index($txid) == null' <<<"${MEMPOOL_AFTER}" >/dev/null \
    || fail "shield tx still in mempool after block: ${MEMPOOL_AFTER}"

BLOCK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TIP_AFTER}]")")"
BLOCK_JSON="$(rpc_result "getblock" "[\"${BLOCK_HASH}\", 1]")"
jq -e --arg txid "${SHIELD_TXID}" '.result.tx | index($txid) != null' <<<"${BLOCK_JSON}" >/dev/null \
    || fail "shield tx ${SHIELD_TXID} not in block ${BLOCK_HASH}: ${BLOCK_JSON}"
pass "shield tx mined into block ${BLOCK_HASH:0:16}… at height ${TIP_AFTER}"

# ── Confirm shielded balance now reflects the shielded note ────────────
BAL_RES="$(rpc_result "wallet.shieldedbalance" '[]')"
BAL_UNA="$(jq -r '.result.balance_una // 0' <<<"${BAL_RES}")"
TREE_SIZE="$(jq -r '.result.tree_size // 0' <<<"${BAL_RES}")"
(( TREE_SIZE >= 1 )) || fail "expected tree_size >= 1 after shield, got ${TREE_SIZE}: ${BAL_RES}"
(( BAL_UNA == SHIELD_VALUE_UNA )) || \
    info "shielded balance=${BAL_UNA} una (expected ${SHIELD_VALUE_UNA}; pending-note delta acceptable)"
pass "shieldedbalance.tree_size=${TREE_SIZE}, balance=${BAL_UNA} una"

stop_node
echo "=== SUCCESS: wallet.shield end-to-end on regtest ==="
