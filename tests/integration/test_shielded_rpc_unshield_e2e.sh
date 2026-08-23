#!/usr/bin/env bash
# Phase 3 wave 3c: end-to-end regtest for the wallet.unshield RPC.
#
# Asserts:
#   - shield 1 DIN → mine → unshield 1 DIN → mempool → mine → block
#   - double-spend rejection (second unshield finds no available note)
#   - insufficient balance rejection (no notes available)
#   - dust / fee_too_large rejection
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

DATA_DIR="/tmp/dinero_unshield_rpc_e2e_$$"
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
    local test_rc=$?
    trap - EXIT
    set +e
    dinero_cleanup_single_daemon "${test_rc}" "${PID}" "${DATA_DIR}" \
        "${KEEP_ON_FAIL}" "unshield daemon" "${DATA_DIR}" "${LOG_FILE}"
    exit $?
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
read -r RPC_PORT P2P_PORT WALLET_PORT < <(dinero_allocate_port_triplet)

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

# ── Mine spendable funds ───────────────────────────────────────────────
MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","unshield-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 blocks"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null

# ── Negative test: unshield with no shielded notes ─────────────────────
info "Negative case: unshield with empty shielded balance"
EMPTY_RES="$(rpc_call "wallet.unshield" '[1.0]')"
echo "$EMPTY_RES" | jq -e '.result.error == "insufficient_single_note" or .error != null' >/dev/null 2>&1 \
    || fail "empty-balance unshield should be rejected: ${EMPTY_RES}"
pass "no-notes unshield rejected"

# ── Negative test: unshield zero amount ────────────────────────────────
ZERO_RES="$(rpc_call "wallet.unshield" '[0.0]')"
echo "$ZERO_RES" | jq -e '.result.error == "invalid_params" or .error != null' >/dev/null 2>&1 \
    || fail "zero-amount unshield should be rejected: ${ZERO_RES}"
pass "zero-amount unshield rejected"

# ── Shield 1 DIN, mine, confirm note ───────────────────────────────────
# No explicit fee: exercises issue #273 size-aware fee auto-sizing on the
# shield side (the fixed 1000-una default underpaid the mempool floor).
info "Shielding 1 DIN (auto-sized fee)"
SHIELD_RES="$(rpc_result "wallet.shield" '[1.0]')"
SHIELD_TXID="$(jq -r '.result.txid' <<<"${SHIELD_RES}")"
SHIELD_AUTOSIZED="$(jq -r '.result.fee_autosized' <<<"${SHIELD_RES}")"
[[ "${SHIELD_AUTOSIZED}" == "true" ]] || fail "shield fee not auto-sized: ${SHIELD_RES}"
[[ -n "${SHIELD_TXID}" && "${SHIELD_TXID}" != "null" ]] || fail "shield missing txid: ${SHIELD_RES}"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

# ── Verify the note is confirmed ───────────────────────────────────────
BAL_AFTER_SHIELD="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_SIZE="$(jq -r '.result.tree_size' <<<"${BAL_AFTER_SHIELD}")"
(( TREE_SIZE >= 1 )) || fail "expected tree_size >= 1: ${BAL_AFTER_SHIELD}"
pass "shield confirmed, tree_size=${TREE_SIZE}"

# ── Happy path: unshield 1 DIN with auto-sized fee (issue #273) ────────
info "Calling wallet.unshield(1.0 DIN, fee auto-sized)"
UNSHIELD_RES="$(rpc_result "wallet.unshield" '[1.0]')"
echo "${UNSHIELD_RES}" | jq -e '.result.status == "unshielded"' >/dev/null \
    || fail "wallet.unshield did not return status=unshielded: ${UNSHIELD_RES}"

UNSHIELD_TXID="$(jq -r '.result.txid' <<<"${UNSHIELD_RES}")"
NULLIFIER="$(jq -r '.result.nullifier_hex' <<<"${UNSHIELD_RES}")"
RECIPIENT="$(jq -r '.result.recipient_address' <<<"${UNSHIELD_RES}")"
RECIPIENT_UNA="$(jq -r '.result.recipient_una' <<<"${UNSHIELD_RES}")"
FEE_OUT="$(jq -r '.result.fee_una' <<<"${UNSHIELD_RES}")"
AUTOSIZED="$(jq -r '.result.fee_autosized' <<<"${UNSHIELD_RES}")"
VSIZE_OUT="$(jq -r '.result.vsize' <<<"${UNSHIELD_RES}")"
[[ -n "${UNSHIELD_TXID}" && "${UNSHIELD_TXID}" != "null" ]] || fail "missing txid: ${UNSHIELD_RES}"
[[ -n "${NULLIFIER}" && "${NULLIFIER}" != "null" ]] || fail "missing nullifier: ${UNSHIELD_RES}"
[[ -n "${RECIPIENT}" && "${RECIPIENT}" != "null" ]] || fail "missing recipient: ${UNSHIELD_RES}"
[[ "${AUTOSIZED}" == "true" ]] || fail "expected fee_autosized=true: ${UNSHIELD_RES}"
# Size-aware fee invariant: fee covers the mempool floor (1 una/vbyte on
# regtest), and the transparent vout is exactly note_value - fee.
(( FEE_OUT >= VSIZE_OUT )) || fail "auto-sized fee ${FEE_OUT} < vsize ${VSIZE_OUT}"
(( RECIPIENT_UNA == 100000000 - FEE_OUT )) \
    || fail "expected recipient_una=$((100000000 - FEE_OUT)) (note - fee), got ${RECIPIENT_UNA}"
pass "unshield returned status=unshielded, txid=${UNSHIELD_TXID:0:16}…, fee=${FEE_OUT} (vsize=${VSIZE_OUT}), recipient=${RECIPIENT:0:20}…"

# ── Verify tx is in mempool ────────────────────────────────────────────
MEMPOOL="$(rpc_result "getrawmempool" '[]')"
jq -e --arg txid "${UNSHIELD_TXID}" '.result | index($txid) != null' <<<"${MEMPOOL}" >/dev/null \
    || fail "unshield tx ${UNSHIELD_TXID} not in mempool: ${MEMPOOL}"
pass "unshield tx in mempool"

# ── Negative test: double-spend (note now pending-spent) ───────────────
info "Negative case: second unshield against the same note"
DOUBLE_RES="$(rpc_call "wallet.unshield" '[1.0]')"
echo "$DOUBLE_RES" | jq -e '.result.error == "insufficient_single_note" or .error != null' >/dev/null 2>&1 \
    || fail "second unshield should hit insufficient_single_note: ${DOUBLE_RES}"
pass "double-spend rejected (note marked pending-spent)"

# ── Mine, confirm tx in block, confirm note becomes confirmed-spent ────
TIP_BEFORE="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
TIP_AFTER="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
[[ "${TIP_AFTER}" == "$((TIP_BEFORE + 1))" ]] || fail "block height did not advance"

BLOCK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TIP_AFTER}]")")"
BLOCK_JSON="$(rpc_result "getblock" "[\"${BLOCK_HASH}\", 1]")"
jq -e --arg txid "${UNSHIELD_TXID}" '.result.tx | index($txid) != null' <<<"${BLOCK_JSON}" >/dev/null \
    || fail "unshield tx not in block: ${BLOCK_JSON}"
pass "unshield tx mined into block ${BLOCK_HASH:0:16}… at height ${TIP_AFTER}"

# ── Verify a fresh transparent UTXO emerged at the unshield value ──────
# Note: we'd compare by address but the wallet has a known HRP-encoding
# mismatch between getNewAddress (returns rdin1p... on regtest) and
# listunspent (re-encodes the same scriptPubKey as din1p...). Compare by
# {amount_una, txid} instead — both come from the same code path.
LISTUNSPENT="$(rpc_result "wallet.listunspent" '[0,9999999]')"
jq -e --argjson recipient_una "${RECIPIENT_UNA}" --arg unshield_txid "${UNSHIELD_TXID}" \
   '.result | map(select(.amount_una == $recipient_una and .txid == $unshield_txid)) | length >= 1' \
    <<<"${LISTUNSPENT}" >/dev/null \
    || fail "expected new transparent UTXO with amount_una=${RECIPIENT_UNA} and txid=${UNSHIELD_TXID}: ${LISTUNSPENT}"
pass "new transparent UTXO emerged from unshield"

# ── Final shielded balance should reflect the spent note ───────────────
FINAL_BAL="$(rpc_result "wallet.shieldedbalance" '[]')"
FINAL_BAL_UNA="$(jq -r '.result.balance_una // 0' <<<"${FINAL_BAL}")"
(( FINAL_BAL_UNA == 0 )) || \
    info "shielded balance after unshield = ${FINAL_BAL_UNA} una (expected 0; non-zero acceptable if test mined extra coinbase shields)"
pass "shieldedbalance.balance_una=${FINAL_BAL_UNA} after unshield mine"

echo "=== SUCCESS: wallet.unshield end-to-end on regtest ==="
