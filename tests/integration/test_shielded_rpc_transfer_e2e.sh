#!/usr/bin/env bash
# Phase 3 wave 3d: end-to-end regtest for the wallet.transfer RPC.
#
# Asserts:
#   - shield 1 DIN → mine → transfer (self) → mempool → mine → block
#   - new self-output note replaces spent note in shielded balance
#   - double-spend rejection (second transfer finds no available note)
#   - empty-balance rejection (no notes available)
#   - zero-fee rejection
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

DATA_DIR="/tmp/dinero_transfer_rpc_e2e_$$"
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

# ── Mine spendable funds ───────────────────────────────────────────────
MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","transfer-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 blocks"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null

# ── Negative test: transfer with no shielded notes ─────────────────────
info "Negative case: transfer with empty shielded balance"
EMPTY_RES="$(rpc_call "wallet.transfer" '[10000]')"
echo "$EMPTY_RES" | jq -e '.result.error == "insufficient_single_note" or .error != null' >/dev/null 2>&1 \
    || fail "empty-balance transfer should be rejected: ${EMPTY_RES}"
pass "no-notes transfer rejected"

# ── Negative test: zero fee ────────────────────────────────────────────
ZERO_RES="$(rpc_call "wallet.transfer" '[0]')"
echo "$ZERO_RES" | jq -e '.result.error == "invalid_params" or .error != null' >/dev/null 2>&1 \
    || fail "zero-fee transfer should be rejected: ${ZERO_RES}"
pass "zero-fee transfer rejected"

# ── Shield 1 DIN, mine, confirm note ───────────────────────────────────
# No explicit fee: exercises issue #273 size-aware fee auto-sizing.
info "Shielding 1 DIN (auto-sized fee)"
SHIELD_RES="$(rpc_result "wallet.shield" '[1.0]')"
SHIELD_TXID="$(jq -r '.result.txid' <<<"${SHIELD_RES}")"
[[ -n "${SHIELD_TXID}" && "${SHIELD_TXID}" != "null" ]] || fail "shield missing txid: ${SHIELD_RES}"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

BAL_AFTER_SHIELD="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_SIZE_BEFORE="$(jq -r '.result.tree_size' <<<"${BAL_AFTER_SHIELD}")"
BAL_BEFORE_UNA="$(jq -r '.result.balance_una // 0' <<<"${BAL_AFTER_SHIELD}")"
(( TREE_SIZE_BEFORE >= 1 )) || fail "expected tree_size >= 1: ${BAL_AFTER_SHIELD}"
pass "shield confirmed, tree_size=${TREE_SIZE_BEFORE}, balance_una=${BAL_BEFORE_UNA}"

# ── Happy path: transfer (self) with auto-sized fee (issue #273) ───────
# Object form with no fee_una → the handler measures the built tx and
# raises the fee to the mempool's vsize-based floor.
info "Calling wallet.transfer (fee auto-sized)"
TRANSFER_RES="$(rpc_result "wallet.transfer" '{}')"
echo "${TRANSFER_RES}" | jq -e '.result.status == "transferred"' >/dev/null \
    || fail "wallet.transfer did not return status=transferred: ${TRANSFER_RES}"

TRANSFER_TXID="$(jq -r '.result.txid' <<<"${TRANSFER_RES}")"
SPEND_NULL="$(jq -r '.result.spend_nullifier_hex' <<<"${TRANSFER_RES}")"
SPEND_VALUE="$(jq -r '.result.spend_value_una' <<<"${TRANSFER_RES}")"
OUT_VALUE="$(jq -r '.result.out_value_una' <<<"${TRANSFER_RES}")"
OUT_COMMIT="$(jq -r '.result.out_commitment_hex' <<<"${TRANSFER_RES}")"
TRANSFER_FEE="$(jq -r '.result.fee_una' <<<"${TRANSFER_RES}")"
AUTOSIZED="$(jq -r '.result.fee_autosized' <<<"${TRANSFER_RES}")"
VSIZE_OUT="$(jq -r '.result.vsize' <<<"${TRANSFER_RES}")"
[[ -n "${TRANSFER_TXID}" && "${TRANSFER_TXID}" != "null" ]] || fail "missing txid: ${TRANSFER_RES}"
[[ -n "${SPEND_NULL}" && "${SPEND_NULL}" != "null" ]] || fail "missing nullifier: ${TRANSFER_RES}"
[[ -n "${OUT_COMMIT}" && "${OUT_COMMIT}" != "null" ]] || fail "missing out commitment: ${TRANSFER_RES}"
[[ "${AUTOSIZED}" == "true" ]] || fail "expected fee_autosized=true: ${TRANSFER_RES}"
(( TRANSFER_FEE >= VSIZE_OUT )) || fail "auto-sized fee ${TRANSFER_FEE} < vsize ${VSIZE_OUT}"
(( OUT_VALUE == SPEND_VALUE - TRANSFER_FEE )) \
    || fail "expected out_value_una = spend_value_una - fee (${SPEND_VALUE} - ${TRANSFER_FEE} = $((SPEND_VALUE - TRANSFER_FEE))), got ${OUT_VALUE}"
pass "transfer returned status=transferred, txid=${TRANSFER_TXID:0:16}…, in=${SPEND_VALUE} out=${OUT_VALUE} fee=${TRANSFER_FEE} (vsize=${VSIZE_OUT})"

# ── Verify tx is in mempool ────────────────────────────────────────────
MEMPOOL="$(rpc_result "getrawmempool" '[]')"
jq -e --arg txid "${TRANSFER_TXID}" '.result | index($txid) != null' <<<"${MEMPOOL}" >/dev/null \
    || fail "transfer tx ${TRANSFER_TXID} not in mempool: ${MEMPOOL}"
pass "transfer tx in mempool"

# ── Negative test: double-spend (note now pending-spent) ───────────────
info "Negative case: second transfer against the same note"
DOUBLE_RES="$(rpc_call "wallet.transfer" '{}')"
echo "$DOUBLE_RES" | jq -e '.result.error == "insufficient_single_note" or .error != null' >/dev/null 2>&1 \
    || fail "second transfer should hit insufficient_single_note: ${DOUBLE_RES}"
pass "double-spend rejected (note marked pending-spent)"

# ── Mine, confirm tx in block ──────────────────────────────────────────
TIP_BEFORE="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
TIP_AFTER="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
[[ "${TIP_AFTER}" == "$((TIP_BEFORE + 1))" ]] || fail "block height did not advance"

BLOCK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TIP_AFTER}]")")"
BLOCK_JSON="$(rpc_result "getblock" "[\"${BLOCK_HASH}\", 1]")"
jq -e --arg txid "${TRANSFER_TXID}" '.result.tx | index($txid) != null' <<<"${BLOCK_JSON}" >/dev/null \
    || fail "transfer tx not in block: ${BLOCK_JSON}"
pass "transfer tx mined into block ${BLOCK_HASH:0:16}… at height ${TIP_AFTER}"

# ── Verify shielded tree grew (one new commitment) and balance reflects fee ──
FINAL_BAL="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_SIZE_AFTER="$(jq -r '.result.tree_size' <<<"${FINAL_BAL}")"
BAL_AFTER_UNA="$(jq -r '.result.balance_una // 0' <<<"${FINAL_BAL}")"
(( TREE_SIZE_AFTER == TREE_SIZE_BEFORE + 1 )) \
    || fail "expected tree_size to grow by 1 (${TREE_SIZE_BEFORE} → ${TREE_SIZE_BEFORE}+1), got ${TREE_SIZE_AFTER}"
(( BAL_AFTER_UNA == BAL_BEFORE_UNA - TRANSFER_FEE )) \
    || fail "expected balance to drop by exactly the fee: ${BAL_BEFORE_UNA} - ${TRANSFER_FEE} = $((BAL_BEFORE_UNA - TRANSFER_FEE)), got ${BAL_AFTER_UNA}"
pass "tree_size ${TREE_SIZE_BEFORE} → ${TREE_SIZE_AFTER}; balance ${BAL_BEFORE_UNA} → ${BAL_AFTER_UNA} (Δ=-${TRANSFER_FEE})"

echo "=== SUCCESS: wallet.transfer end-to-end on regtest ==="
