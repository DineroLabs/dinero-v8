#!/usr/bin/env bash
# M2: end-to-end regtest coverage for blockchain.shielded.outputs.
#
# Validates the public light-client shielded feed through the real daemon path:
#   - empty non-shielded blocks are omitted from result.blocks
#   - a shield tx block returns exact commitment + encrypted_note bytes
#   - a transfer block returns the public spend nullifier
#   - multi-block ranges preserve height order and monotonic leaf indexes
#   - alias/positional params and count clamping match the RPC contract
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

DATA_DIR="/tmp/dinero_shielded_outputs_feed_$$"
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
RPC_PORT=$((41000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","m2-feed-miner"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"

info "Mining 101 spendable regtest blocks"
rpc_result "generatetoaddress" "[101,\"${MINER_ADDR}\"]" >/dev/null
BASE_TIP="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"

EMPTY_FEED="$(rpc_result "blockchain.shielded.outputs" "{\"from_height\": ${BASE_TIP}, \"count\": 1}")"
jq -e ".result.from_height == ${BASE_TIP} and .result.count == 1 and (.result.tip_height >= ${BASE_TIP}) and (.result.blocks | length) == 0" \
    <<<"${EMPTY_FEED}" >/dev/null || fail "empty non-shielded block should be omitted: ${EMPTY_FEED}"
pass "empty non-shielded block omitted"

CLAMP_FEED="$(rpc_result "blockchain.shielded.outputs" '{"from_height": -7, "count": 999999}')"
jq -e '.result.from_height == 0 and .result.count == 2000 and (.result.blocks | type == "array")' \
    <<<"${CLAMP_FEED}" >/dev/null || fail "bad params were not clamped as expected: ${CLAMP_FEED}"
pass "negative from_height and oversized count clamp consistently"

info "Shielding 1 DIN to create first feed output"
SHIELD_RES="$(rpc_result "wallet.shield" '[1.0, 10000]')"
SHIELD_TXID="$(jq -r '.result.txid' <<<"${SHIELD_RES}")"
SHIELD_COMMITMENT="$(jq -r '.result.commitment_hex' <<<"${SHIELD_RES}")"
[[ -n "${SHIELD_TXID}" && "${SHIELD_TXID}" != "null" ]] || fail "shield missing txid: ${SHIELD_RES}"
[[ -n "${SHIELD_COMMITMENT}" && "${SHIELD_COMMITMENT}" != "null" ]] || fail "shield missing commitment: ${SHIELD_RES}"

rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
SHIELD_HEIGHT="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
SHIELD_BLOCK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${SHIELD_HEIGHT}]")")"

SHIELD_FEED="$(rpc_result "blockchain.shielded.outputs" "{\"from_height\": ${SHIELD_HEIGHT}, \"count\": 1}")"
jq -e --arg hash "${SHIELD_BLOCK_HASH}" --arg txid "${SHIELD_TXID}" --arg commitment "${SHIELD_COMMITMENT}" \
    '.result.blocks | length == 1 and
     .[0].height == '"${SHIELD_HEIGHT}"' and
     .[0].block_hash == $hash and
     .[0].shielded_spend_count == 0 and
     .[0].shielded_output_count == 1 and
     (.[0].spent_nullifiers | length) == 0 and
     (.[0].outputs | length) == 1 and
     .[0].outputs[0].txid == $txid and
     .[0].outputs[0].output_index == 0 and
     .[0].outputs[0].leaf_index == 0 and
     .[0].outputs[0].commitment == $commitment and
     (.[0].outputs[0].encrypted_note | length) == 192' \
    <<<"${SHIELD_FEED}" >/dev/null || fail "shield output feed mismatch: ${SHIELD_FEED}"
pass "shield block returned exact commitment and legacy 96-byte encrypted note"

TRANSFER_FEE=50000
info "Creating a shielded self-transfer to expose a public nullifier"
TRANSFER_RES="$(rpc_result "wallet.transfer" "[${TRANSFER_FEE}]")"
echo "${TRANSFER_RES}" | jq -e '.result.status == "transferred"' >/dev/null \
    || fail "wallet.transfer did not return transferred: ${TRANSFER_RES}"
TRANSFER_TXID="$(jq -r '.result.txid' <<<"${TRANSFER_RES}")"
SPEND_NULLIFIER="$(jq -r '.result.spend_nullifier_hex' <<<"${TRANSFER_RES}")"
TRANSFER_COMMITMENT="$(jq -r '.result.out_commitment_hex' <<<"${TRANSFER_RES}")"
[[ -n "${TRANSFER_TXID}" && "${TRANSFER_TXID}" != "null" ]] || fail "transfer missing txid: ${TRANSFER_RES}"
[[ -n "${SPEND_NULLIFIER}" && "${SPEND_NULLIFIER}" != "null" ]] || fail "transfer missing nullifier: ${TRANSFER_RES}"
[[ -n "${TRANSFER_COMMITMENT}" && "${TRANSFER_COMMITMENT}" != "null" ]] || fail "transfer missing output commitment: ${TRANSFER_RES}"

rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
TRANSFER_HEIGHT="$(jq -r '.result' <<<"$(rpc_result "getblockcount" '[]')")"
TRANSFER_BLOCK_HASH="$(jq -r '.result' <<<"$(rpc_result "getblockhash" "[${TRANSFER_HEIGHT}]")")"

TRANSFER_FEED="$(rpc_result "shieldedoutputs" "[${TRANSFER_HEIGHT}, 1]")"
jq -e --arg hash "${TRANSFER_BLOCK_HASH}" --arg txid "${TRANSFER_TXID}" \
      --arg nullifier "${SPEND_NULLIFIER}" --arg commitment "${TRANSFER_COMMITMENT}" \
    '.result.blocks | length == 1 and
     .[0].height == '"${TRANSFER_HEIGHT}"' and
     .[0].block_hash == $hash and
     .[0].shielded_spend_count == 1 and
     .[0].shielded_output_count == 1 and
     (.[0].spent_nullifiers | length) == 1 and
     .[0].spent_nullifiers[0].txid == $txid and
     .[0].spent_nullifiers[0].spend_index == 0 and
     .[0].spent_nullifiers[0].nullifier == $nullifier and
     (.[0].outputs | length) == 1 and
     .[0].outputs[0].txid == $txid and
     .[0].outputs[0].leaf_index == 1 and
     .[0].outputs[0].commitment == $commitment and
     (.[0].outputs[0].encrypted_note | length) == 192' \
    <<<"${TRANSFER_FEED}" >/dev/null || fail "transfer output/nullifier feed mismatch: ${TRANSFER_FEED}"
pass "transfer block returned public nullifier and next leaf"

RANGE_FEED="$(rpc_result "blockchain.shielded.outputs" "{\"from_height\": ${SHIELD_HEIGHT}, \"count\": 2}")"
jq -e --arg shield_txid "${SHIELD_TXID}" --arg transfer_txid "${TRANSFER_TXID}" \
    '.result.blocks | length == 2 and
     .[0].height < .[1].height and
     .[0].outputs[0].txid == $shield_txid and
     .[0].outputs[0].leaf_index == 0 and
     (.[1].spent_nullifiers | length) == 1 and
     .[1].outputs[0].txid == $transfer_txid and
     .[1].outputs[0].leaf_index == 1' \
    <<<"${RANGE_FEED}" >/dev/null || fail "multi-block range ordering mismatch: ${RANGE_FEED}"
pass "multi-block range preserves height order and leaf indexes"

echo "=== SUCCESS: blockchain.shielded.outputs light-client feed ==="
