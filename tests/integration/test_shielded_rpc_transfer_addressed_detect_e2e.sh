#!/usr/bin/env bash
# Phase 5 Wave 3e: addressed transfer with receive-side detection.
#
# Asserts the WHOLE round trip when sender and recipient share a wallet:
#   - shield 1 DIN to self → mine
#   - transfer 0.7 DIN with address = account 0 j=1 (same wallet, same ivk)
#   - mine the transfer block
#   - wallet's shielded balance reflects only the fee being paid (the
#     recipient note is detected via the new receive-side scan in
#     ProcessConfirmedBlock)
#   - shielded tree grew by exactly 2 (recipient + change outputs)
#
# Distinct from ShieldedRpcTransferAddressedEndToEnd which targets
# account 1 (different ivk → undetectable, so balance drops by amount +
# fee).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

DATA_DIR="/tmp/dinero_xfer_addr_detect_$$"
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
        "${KEEP_ON_FAIL}" "address-detection daemon" "${DATA_DIR}" "${LOG_FILE}"
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

MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","detect-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 blocks"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null

# Target: same account 0 (same ivk) but j=1 → different (d, pk_d).
# Wave 3e ProcessConfirmedBlock scans account 0 ivk; should detect.
RECIPIENT_RES="$(rpc_result "wallet.getshieldedaddress" '{"account": 0, "j": 1}')"
RECIPIENT_ADDR="$(jq -r '.result.address' <<<"${RECIPIENT_RES}")"
[[ "${RECIPIENT_ADDR}" =~ ^rdins1 ]] || fail "expected rdins1… recipient, got ${RECIPIENT_ADDR}"
pass "recipient address (account 0 j=1) ${RECIPIENT_ADDR:0:24}…"

# Auto-sized fee — issue #273.
info "Shielding 1 DIN"
rpc_result "wallet.shield" '[1.0]' >/dev/null
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

BAL_BEFORE="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_BEFORE="$(jq -r '.result.tree_size' <<<"${BAL_BEFORE}")"
BAL_BEFORE_UNA="$(jq -r '.result.balance_una // 0' <<<"${BAL_BEFORE}")"
(( TREE_BEFORE >= 1 )) || fail "expected tree_size >= 1, got ${TREE_BEFORE}"
pass "shield confirmed; tree=${TREE_BEFORE}, balance=${BAL_BEFORE_UNA}"

TRANSFER_AMOUNT=70000000
info "Calling wallet.transfer to self via account 0 j=1 (fee auto-sized — issue #273)"
T_RES="$(rpc_result "wallet.transfer" "{\"amount_una\": ${TRANSFER_AMOUNT}, \"address\": \"${RECIPIENT_ADDR}\"}")"
echo "${T_RES}" | jq -e '.result.status == "transferred" and .result.wave == "3d-addressed"' >/dev/null \
    || fail "expected transferred/3d-addressed: ${T_RES}"
T_TXID="$(jq -r '.result.txid' <<<"${T_RES}")"
HAD_CHANGE="$(jq -r '.result.had_change' <<<"${T_RES}")"
TRANSFER_FEE="$(jq -r '.result.fee_una' <<<"${T_RES}")"
AUTOSIZED="$(jq -r '.result.fee_autosized' <<<"${T_RES}")"
VSIZE_OUT="$(jq -r '.result.vsize' <<<"${T_RES}")"
[[ "${AUTOSIZED}" == "true" ]] || fail "expected fee_autosized=true: ${T_RES}"
(( TRANSFER_FEE >= VSIZE_OUT )) || fail "auto-sized fee ${TRANSFER_FEE} < vsize ${VSIZE_OUT}"
pass "addressed transfer txid=${T_TXID:0:16}… had_change=${HAD_CHANGE} fee=${TRANSFER_FEE} (vsize=${VSIZE_OUT})"

# Mine the transfer block — receive-side scan runs here.
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

# Verify tx in block.
TIP="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
BLK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TIP}]")")"
BLK_JSON="$(rpc_result "getblock" "[\"${BLK_HASH}\", 1]")"
jq -e --arg txid "${T_TXID}" '.result.tx | index($txid) != null' <<<"${BLK_JSON}" >/dev/null \
    || fail "addressed transfer not in block: ${BLK_JSON}"
pass "addressed transfer mined into block ${BLK_HASH:0:16}…"

# Receive-side detection check: wallet should now see the recipient
# note. Total shielded balance after = (balance_before - fee) because
# both recipient (detected via Wave 3e scan) and change (legacy
# AddPendingNote path) are owned by the same wallet.
FINAL_BAL="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_AFTER="$(jq -r '.result.tree_size' <<<"${FINAL_BAL}")"
BAL_AFTER_UNA="$(jq -r '.result.balance_una // 0' <<<"${FINAL_BAL}")"
EXPECTED=$((BAL_BEFORE_UNA - TRANSFER_FEE))
(( BAL_AFTER_UNA == EXPECTED )) \
    || fail "expected balance ${EXPECTED} (= ${BAL_BEFORE_UNA} - ${TRANSFER_FEE}); got ${BAL_AFTER_UNA} — recipient note NOT detected by Wave 3e scan"
pass "RECEIVED recipient note via Wave 3e scan: balance ${BAL_BEFORE_UNA} → ${BAL_AFTER_UNA} (Δ=-fee only)"
(( TREE_AFTER == TREE_BEFORE + 2 )) \
    || fail "expected tree_size to grow by 2 (recipient + change), got ${TREE_AFTER} from ${TREE_BEFORE}"
pass "tree_size ${TREE_BEFORE} → ${TREE_AFTER} (+2: recipient + change)"

echo "=== SUCCESS: Wave 3e receive-side detection ==="
