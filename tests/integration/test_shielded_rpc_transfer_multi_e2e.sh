#!/usr/bin/env bash
# Phase 3 wave 3e: end-to-end regtest for multi-spend wallet.transfer.
#
# Asserts:
#   - empty-balance amount rejection
#   - amount=0 rejection
#   - amount > available balance rejection
#   - happy path: shield 0.5 DIN + 0.5 DIN, transfer amount=0.7 DIN with
#     fee=10000 → 2-spend, 2-output (recipient + change), value_balance=-fee
#   - tree grows by exactly 2 (one recipient note + one change note)
#   - shielded balance drops by exactly the fee
#   - double-spend rejection (both notes now pending-spent)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"

DATA_DIR="/tmp/dinero_transfer_multi_e2e_$$"
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
    if [[ -f "${datadir}/.cookie" ]]; then printf '%s\n' "${datadir}/.cookie"; return 0; fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then printf '%s\n' "${datadir}/regtest/.cookie"; return 0; fi
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
        if [[ -n "${PID}" ]] && ! kill -0 "${PID}" 2>/dev/null; then return 1; fi
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}

start_node() {
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" \
        --regtest --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 \
        >"${LOG_FILE}" 2>&1 &
    PID=$!
}

require_tools
RPC_PORT=$((40000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","transfer-multi-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 blocks"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null

# ── Negative: empty balance + amount supplied ──────────────────────────
info "Negative: amount supplied with empty shielded balance"
EMPTY_RES="$(rpc_call "wallet.transfer" '[10000, 100000000]')"
echo "$EMPTY_RES" | jq -e '.result.error == "insufficient_balance" or .error != null' >/dev/null 2>&1 \
    || fail "empty-balance amount transfer should be rejected: ${EMPTY_RES}"
pass "no-notes amount transfer rejected"

# ── Negative: amount=0 ─────────────────────────────────────────────────
ZERO_RES="$(rpc_call "wallet.transfer" '[10000, 0]')"
echo "$ZERO_RES" | jq -e '.result.error == "invalid_params" or .error != null' >/dev/null 2>&1 \
    || fail "amount=0 should be rejected: ${ZERO_RES}"
pass "zero amount rejected"

# ── Shield 0.5 DIN twice ───────────────────────────────────────────────
info "Shielding 0.5 DIN (note A)"
rpc_result "wallet.shield" '[0.5, 10000]' >/dev/null
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
info "Shielding 0.5 DIN (note B)"
rpc_result "wallet.shield" '[0.5, 10000]' >/dev/null
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

BAL_BEFORE="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_BEFORE="$(jq -r '.result.tree_size' <<<"${BAL_BEFORE}")"
BAL_BEFORE_UNA="$(jq -r '.result.balance_una // 0' <<<"${BAL_BEFORE}")"
(( TREE_BEFORE >= 2 )) || fail "expected tree_size >= 2, got ${TREE_BEFORE}"
(( BAL_BEFORE_UNA >= 100000000 )) || fail "expected balance_una >= 1 DIN, got ${BAL_BEFORE_UNA}"
pass "two notes confirmed; tree_size=${TREE_BEFORE}, balance_una=${BAL_BEFORE_UNA}"

# ── Negative: amount > available balance ───────────────────────────────
TOO_MUCH=$((BAL_BEFORE_UNA + 1000000))
INSUF_RES="$(rpc_call "wallet.transfer" "[10000, ${TOO_MUCH}]")"
echo "$INSUF_RES" | jq -e '.result.error == "insufficient_balance" or .error != null' >/dev/null 2>&1 \
    || fail "amount > balance should be rejected: ${INSUF_RES}"
pass "amount > balance rejected"

# ── Happy path: amount=0.7 DIN, fee=20000 ──────────────────────────────
# Multi-spend bundles (2+ spends) are larger than single-spend; need fee
# >= 1 una/byte (BIP141 vsize). 2-spend / 2-output bundle is ~11kB → fee
# of 20000 una clears with buffer.
TRANSFER_FEE=20000
TRANSFER_AMOUNT=70000000   # 0.7 DIN in una
info "Calling wallet.transfer(fee=${TRANSFER_FEE}, amount_una=${TRANSFER_AMOUNT})"
TRANSFER_RES="$(rpc_result "wallet.transfer" "[${TRANSFER_FEE}, ${TRANSFER_AMOUNT}]")"
echo "${TRANSFER_RES}" | jq -e '.result.status == "transferred" and .result.wave == "3e"' >/dev/null \
    || fail "wallet.transfer (multi) did not return status=transferred wave=3e: ${TRANSFER_RES}"

TRANSFER_TXID="$(jq -r '.result.txid' <<<"${TRANSFER_RES}")"
SPEND_COUNT="$(jq -r '.result.spend_count' <<<"${TRANSFER_RES}")"
OUT_COUNT="$(jq -r '.result.out_count' <<<"${TRANSFER_RES}")"
AMOUNT_OUT="$(jq -r '.result.amount_una' <<<"${TRANSFER_RES}")"
CHANGE_OUT="$(jq -r '.result.change_una' <<<"${TRANSFER_RES}")"
FEE_OUT="$(jq -r '.result.fee_una' <<<"${TRANSFER_RES}")"
[[ -n "${TRANSFER_TXID}" && "${TRANSFER_TXID}" != "null" ]] || fail "missing txid: ${TRANSFER_RES}"
(( SPEND_COUNT >= 2 )) || fail "expected multi-spend (>=2 notes), got spend_count=${SPEND_COUNT}"
(( OUT_COUNT == 2 )) || fail "expected 2 outputs (recipient + change), got out_count=${OUT_COUNT}"
(( AMOUNT_OUT == TRANSFER_AMOUNT )) || fail "expected amount_una=${TRANSFER_AMOUNT}, got ${AMOUNT_OUT}"
(( CHANGE_OUT > 0 )) || fail "expected positive change, got ${CHANGE_OUT}"
(( FEE_OUT == TRANSFER_FEE )) || fail "expected fee=${TRANSFER_FEE}, got ${FEE_OUT}"
pass "transfer (multi) returned spend_count=${SPEND_COUNT} out_count=${OUT_COUNT} amount=${AMOUNT_OUT} change=${CHANGE_OUT} fee=${FEE_OUT}"

# ── In mempool ─────────────────────────────────────────────────────────
MEMPOOL="$(rpc_result "getrawmempool" '[]')"
jq -e --arg txid "${TRANSFER_TXID}" '.result | index($txid) != null' <<<"${MEMPOOL}" >/dev/null \
    || fail "transfer tx ${TRANSFER_TXID} not in mempool: ${MEMPOOL}"
pass "transfer tx in mempool"

# ── Double-spend: both notes pending-spent → insufficient ─────────────
DOUBLE_RES="$(rpc_call "wallet.transfer" "[${TRANSFER_FEE}, ${TRANSFER_AMOUNT}]")"
echo "$DOUBLE_RES" | jq -e '.result.error == "insufficient_balance" or .error != null' >/dev/null 2>&1 \
    || fail "second transfer should hit insufficient_balance: ${DOUBLE_RES}"
pass "double-spend rejected (notes pending-spent)"

# ── Mine, confirm tx, verify tree growth + balance delta ──────────────
TIP_BEFORE="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
TIP_AFTER="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
[[ "${TIP_AFTER}" == "$((TIP_BEFORE + 1))" ]] || fail "block height did not advance"

BLOCK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TIP_AFTER}]")")"
BLOCK_JSON="$(rpc_result "getblock" "[\"${BLOCK_HASH}\", 1]")"
jq -e --arg txid "${TRANSFER_TXID}" '.result.tx | index($txid) != null' <<<"${BLOCK_JSON}" >/dev/null \
    || fail "transfer tx not in block: ${BLOCK_JSON}"
pass "transfer mined into block ${BLOCK_HASH:0:16}… at height ${TIP_AFTER}"

FINAL_BAL="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_AFTER="$(jq -r '.result.tree_size' <<<"${FINAL_BAL}")"
BAL_AFTER_UNA="$(jq -r '.result.balance_una // 0' <<<"${FINAL_BAL}")"
(( TREE_AFTER == TREE_BEFORE + 2 )) \
    || fail "expected tree_size to grow by 2 (${TREE_BEFORE} → ${TREE_BEFORE}+2), got ${TREE_AFTER}"
(( BAL_AFTER_UNA == BAL_BEFORE_UNA - TRANSFER_FEE )) \
    || fail "expected balance to drop by exactly the fee: ${BAL_BEFORE_UNA} - ${TRANSFER_FEE} = $((BAL_BEFORE_UNA - TRANSFER_FEE)), got ${BAL_AFTER_UNA}"
pass "tree_size ${TREE_BEFORE} → ${TREE_AFTER}; balance ${BAL_BEFORE_UNA} → ${BAL_AFTER_UNA} (Δ=-${TRANSFER_FEE})"

echo "=== SUCCESS: wallet.transfer multi-spend end-to-end on regtest ==="
