#!/usr/bin/env bash
# Phase 5 Wave 3d: addressed wallet.transfer regtest e2e.
#
# Asserts the sender side of any-recipient transfer:
#   - shield 1 DIN to self → mine
#   - call wallet.transfer({fee, amount_una, address}) where `address`
#     is a recipient shielded address from a different account index
#   - verify status="transferred", wave="3d-addressed"
#   - tx accepted into mempool, mined into a block
#   - sender's shielded balance reflects (sum_inputs - amount - fee) for
#     change; recipient balance shows nothing yet (Wave 3e adds the
#     receive-side scanner).
#   - reject: address with invalid HRP
#   - reject: address without amount
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

DATA_DIR="/tmp/dinero_xfer_addr_$$"
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

MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","xfer-addr-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 blocks"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null

# Get a recipient shielded address (account 1, j=0) from the same wallet —
# stands in for a "different recipient" since we have one wallet here.
RECIPIENT_RES="$(rpc_result "wallet.getshieldedaddress" '{"account": 1, "j": 0}')"
RECIPIENT_ADDR="$(jq -r '.result.address' <<<"${RECIPIENT_RES}")"
[[ "${RECIPIENT_ADDR}" =~ ^rdins1 ]] || fail "expected rdins1… recipient, got ${RECIPIENT_ADDR}"
pass "recipient address ${RECIPIENT_ADDR:0:24}…"

# Shield 1 DIN to self (auto-sized fee — issue #273).
info "Shielding 1 DIN"
rpc_result "wallet.shield" '[1.0]' >/dev/null
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

BAL_BEFORE="$(rpc_result "wallet.shieldedbalance" '[]')"
TREE_BEFORE="$(jq -r '.result.tree_size' <<<"${BAL_BEFORE}")"
BAL_BEFORE_UNA="$(jq -r '.result.balance_una // 0' <<<"${BAL_BEFORE}")"
(( TREE_BEFORE >= 1 )) || fail "expected tree_size >= 1, got ${TREE_BEFORE}"
pass "shield confirmed; tree_size=${TREE_BEFORE}, balance_una=${BAL_BEFORE_UNA}"

# Negative: address without amount → invalid_params.
info "Negative: address without amount"
NA_RES="$(rpc_call "wallet.transfer" "{\"fee_una\": 20000, \"address\": \"${RECIPIENT_ADDR}\"}")"
# #458 envelope migration: accept both nested and top-level error shapes.
echo "${NA_RES}" | jq -e '(.error.message // .result.error) == "invalid_params"' >/dev/null \
    || fail "address-without-amount should be invalid_params: ${NA_RES}"
pass "address-without-amount rejected"

# Negative: bad HRP (taproot din1 mishosed in shielded slot).
info "Negative: non-shielded HRP rejected"
BAD_HRP_RES="$(rpc_call "wallet.transfer" "{\"fee_una\": 20000, \"amount_una\": 10000, \"address\": \"din1pqyqsywdkqz4dz9hfff9zfwryjs4khufprr2elx2qedlcz4cyk67r3qq2yzg2v\"}")"
echo "${BAD_HRP_RES}" | jq -e '(.error.message // .result.error) == "attach_transfer_failed"' >/dev/null \
    || fail "din1p HRP should be rejected by addressed transfer: ${BAD_HRP_RES}"
pass "non-shielded HRP rejected by addressed transfer"

# Happy path: addressed transfer with change, fee auto-sized (issue #273).
info "Calling wallet.transfer to recipient address (fee auto-sized)"
TRANSFER_AMOUNT=70000000   # 0.7 DIN
T_RES="$(rpc_result "wallet.transfer" "{\"amount_una\": ${TRANSFER_AMOUNT}, \"address\": \"${RECIPIENT_ADDR}\"}")"
echo "${T_RES}" | jq -e '.result.status == "transferred" and .result.wave == "3d-addressed"' >/dev/null \
    || fail "expected transferred/3d-addressed: ${T_RES}"

T_TXID="$(jq -r '.result.txid' <<<"${T_RES}")"
HAD_CHANGE="$(jq -r '.result.had_change' <<<"${T_RES}")"
CHANGE_UNA="$(jq -r '.result.change_una' <<<"${T_RES}")"
SPEND_COUNT="$(jq -r '.result.spend_count' <<<"${T_RES}")"
TRANSFER_FEE="$(jq -r '.result.fee_una' <<<"${T_RES}")"
AUTOSIZED="$(jq -r '.result.fee_autosized' <<<"${T_RES}")"
VSIZE_OUT="$(jq -r '.result.vsize' <<<"${T_RES}")"
[[ -n "${T_TXID}" && "${T_TXID}" != "null" ]] || fail "missing txid: ${T_RES}"
(( SPEND_COUNT >= 1 )) || fail "expected >= 1 spend, got ${SPEND_COUNT}"
[[ "${AUTOSIZED}" == "true" ]] || fail "expected fee_autosized=true: ${T_RES}"
(( TRANSFER_FEE >= VSIZE_OUT )) || fail "auto-sized fee ${TRANSFER_FEE} < vsize ${VSIZE_OUT}"
pass "addressed transfer txid=${T_TXID:0:16}… spends=${SPEND_COUNT} change=${CHANGE_UNA} fee=${TRANSFER_FEE} (vsize=${VSIZE_OUT})"

# Verify in mempool.
MP="$(rpc_result "getrawmempool" '[]')"
jq -e --arg txid "${T_TXID}" '.result | index($txid) != null' <<<"${MP}" >/dev/null \
    || fail "addressed transfer not in mempool: ${MP}"
pass "addressed transfer in mempool"

# Mine, confirm in block.
TIP_BEFORE="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
TIP_AFTER="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
[[ "${TIP_AFTER}" == "$((TIP_BEFORE + 1))" ]] || fail "block height did not advance"
BLK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TIP_AFTER}]")")"
BLK_JSON="$(rpc_result "getblock" "[\"${BLK_HASH}\", 1]")"
jq -e --arg txid "${T_TXID}" '.result.tx | index($txid) != null' <<<"${BLK_JSON}" >/dev/null \
    || fail "addressed transfer tx not in block ${BLK_HASH}: ${BLK_JSON}"
pass "addressed transfer mined into block ${BLK_HASH:0:16}… at height ${TIP_AFTER}"

# Sender's shielded balance: BOTH the change (legacy AddPendingNote
# path) AND the recipient note are owned by the same wallet — recipient
# is account 1 j=0, and ProcessConfirmedBlock now scans accounts 0..3
# (multi-account scan, post-Wave-3e). So balance drops by exactly the
# fee (recipient credited, change credited, sum = sum_inputs - fee).
FINAL_BAL="$(rpc_result "wallet.shieldedbalance" '[]')"
FINAL_BAL_UNA="$(jq -r '.result.balance_una // 0' <<<"${FINAL_BAL}")"
EXPECTED=$((BAL_BEFORE_UNA - TRANSFER_FEE))
(( FINAL_BAL_UNA == EXPECTED )) \
    || fail "expected sender shielded balance ${EXPECTED} (= ${BAL_BEFORE_UNA} - ${TRANSFER_FEE}); got ${FINAL_BAL_UNA} — multi-account scan should detect account 1"
pass "sender shielded balance ${BAL_BEFORE_UNA} → ${FINAL_BAL_UNA} (Δ = -fee; account 1 recipient detected via multi-account scan)"

echo "=== SUCCESS: wallet.transfer addressed end-to-end on regtest ==="
