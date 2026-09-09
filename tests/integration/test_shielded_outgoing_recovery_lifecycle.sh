#!/usr/bin/env bash
# Outgoing-view lifecycle rehearsal (dormant on production networks).
#
# Drives the paired regtest spend-authority/outgoing-view cutover and proves:
#   provisional sender recovery -> lock -> recipient discovery while locked ->
#   confirmation -> restart -> disconnect -> reconnect/rescan -> second restart
#   -> unlock -> recipient-authorized spend. The recipient is account 1 of the
# same wallet so incoming scanning and outgoing recovery are exercised through
# independent authorities while a single regtest process keeps the fixture
# deterministic.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dinero_outgoing_recovery.XXXXXX")"
LOG_FILE="${DATA_DIR}.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_FILE}" ]] && tail -160 "${LOG_FILE}" >&2 || true
    exit 1
}
cleanup() {
    local rc=$?
    trap - EXIT
    set +e
    # fail() may run in a command-substitution subshell, so its assignment to
    # KEEP_ON_FAIL cannot reach the parent. The exit status is authoritative.
    if (( rc != 0 )); then
        KEEP_ON_FAIL=1
        printf '[FAIL] retained daemon artifacts: %s and %s\n' "${DATA_DIR}" "${LOG_FILE}" >&2
    fi
    dinero_cleanup_single_daemon "${rc}" "${PID}" "${DATA_DIR}" \
        "${KEEP_ON_FAIL}" "outgoing-recovery daemon" "${DATA_DIR}" "${LOG_FILE}"
    exit $?
}
trap cleanup EXIT

cookie_file() {
    if [[ -f "${DATA_DIR}/.cookie" ]]; then printf '%s\n' "${DATA_DIR}/.cookie"; return; fi
    if [[ -f "${DATA_DIR}/regtest/.cookie" ]]; then printf '%s\n' "${DATA_DIR}/regtest/.cookie"; return; fi
    return 1
}
rpc_call() {
    local method="$1" params="$2" cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    # Spend-authority proofs are deliberately real. A local Debug build on
    # Apple silicon can spend several minutes inside one wallet RPC.
    curl -sS --max-time 600 --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}
rpc_result() {
    local response
    response="$(rpc_call "$1" "$2")" || fail "$1 transport failure"
    jq -e '.error == null or .error == false' <<<"${response}" >/dev/null \
        || fail "$1 failed: ${response}"
    printf '%s\n' "${response}"
}
wait_rpc() {
    for _ in $(seq 1 120); do
        [[ -z "${PID}" || -e "/proc/${PID}" || "$(uname)" == "Darwin" ]] || return 1
        if rpc_call getblockcount '[]' 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}
start_node() {
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" --regtest --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 \
        --consensus-shielded-epoch-reset-height=1 \
        --consensus-shielded-spend-auth-height=2 \
        >>"${LOG_FILE}" 2>&1 &
    PID=$!
    wait_rpc || fail "daemon did not reach RPC readiness"
}
stop_node() {
    rpc_call stop '[]' >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do
        kill -0 "${PID}" 2>/dev/null || { wait "${PID}" 2>/dev/null || true; PID=""; return; }
        sleep 1
    done
    fail "daemon did not stop cleanly"
}
outgoing() { rpc_result wallet.listshieldedoutgoing '[]'; }

command -v curl >/dev/null || fail "curl required"
command -v jq >/dev/null || fail "jq required"
[[ -x "${DINEROD}" ]] || fail "dinerod missing: ${DINEROD}"
read -r RPC_PORT P2P_PORT WALLET_PORT < <(dinero_allocate_port_triplet)
start_node

MINER="$(rpc_result wallet.getnewaddress '["taproot","outgoing-miner"]' | jq -r '.result.address // .result')"
rpc_result generatetoaddress "[101,\"${MINER}\"]" >/dev/null
RECIPIENT="$(rpc_result wallet.getshieldedaddress '{"account":1,"j":0}' | jq -r '.result.address')"
[[ "${RECIPIENT}" == rdins1* ]] || fail "bad recipient address"

info "shielding directly to account 1 with an outgoing-view envelope"
# A one-spend/two-output shielded transfer is intentionally above the current
# standard ancestor-size limit. That policy belongs to a separate rollout;
# direct transparent-to-shielded delivery exercises the same recipient and
# outgoing authorities without weakening mempool limits for this test.
SEND="$(rpc_result wallet.shield "{\"amount_una\":70000000,\"fee_una\":1000000,\"address\":\"${RECIPIENT}\",\"memo\":\"sender recovery lifecycle\"}")"
TXID="$(jq -r '.result.txid' <<<"${SEND}")"
jq -e '.result.status == "shielded_to_recipient" and (.result.txid | length) == 64' \
    <<<"${SEND}" >/dev/null || fail "addressed shield was not submitted: ${SEND}"
PROVISIONAL="$(outgoing)"
jq -e --arg txid "${TXID}" \
    --arg address "${RECIPIENT}" \
    '([.result.outputs[] | select(.txid == $txid)] | length) == 1 and any(.result.outputs[]; .txid == $txid and .recipient_address == $address and .confirmed == false and .value_una == 70000000 and (.memo_hex | startswith("73656e646572207265636f76657279206c6966656379636c65")))' \
    <<<"${PROVISIONAL}" >/dev/null || fail "provisional outgoing record missing: ${PROVISIONAL}"
pass "sender recovered provisional recipient/value/memo metadata"

# Cache full-viewing authority, then remove spend authority before the output
# reaches the chain. Recognition below must therefore use (ak,nk,nvk,ovk), not
# a master seed or a spend scalar left in memory.
rpc_result wallet.encrypt '["outgoing-lifecycle-pass"]' >/dev/null
LOCKED_STATUS="$(rpc_result wallet.status '[]')"
jq -e '.result.locked == true' <<<"${LOCKED_STATUS}" >/dev/null \
    || fail "wallet did not lock after encryption: ${LOCKED_STATUS}"

rpc_result generatetoaddress "[1,\"${MINER}\"]" >/dev/null
TIP="$(rpc_result getblockcount '[]' | jq -r '.result')"
TRANSFER_BLOCK="$(rpc_result getblockhash "[${TIP}]" | jq -r '.result')"
CONFIRMED="$(outgoing)"
jq -e --arg txid "${TXID}" --argjson height "${TIP}" \
    '.result.count >= 1 and any(.result.outputs[]; .txid == $txid and .confirmed == true and .confirmed_height == $height)' \
    <<<"${CONFIRMED}" >/dev/null || fail "confirmation did not promote outgoing record: ${CONFIRMED}"
pass "confirmed outgoing record promoted at height ${TIP}"

LOCKED_INCOMING="$(rpc_result wallet.listshielded '[]')"
jq -e --argjson height "${TIP}" \
    'any(.result.notes[]; .confirmed == true and .confirmed_height == $height and .value_una == 70000000)' \
    <<<"${LOCKED_INCOMING}" >/dev/null \
    || fail "full-viewing authority did not recognize recipient note while locked: ${LOCKED_INCOMING}"
pass "locked wallet recognized recipient note without retaining spend authority"

LOCKED_SPEND="$(rpc_call wallet.unshield '{"amount_una":60000000,"fee_una":1000000}')"
[[ "$(jq -r '.error.message // .error // .result.error // empty' <<<"${LOCKED_SPEND}")" == "wallet_locked" ]] \
    || fail "locked recipient spend did not fail closed: ${LOCKED_SPEND}"
pass "locked full-viewing wallet cannot spend the recognized note"

stop_node; start_node
AFTER_RESTART="$(outgoing)"
jq -e --arg txid "${TXID}" '.result.count >= 1 and any(.result.outputs[]; .txid == $txid and .confirmed == true)' \
    <<<"${AFTER_RESTART}" >/dev/null || fail "outgoing record lost across restart"
pass "outgoing recovery survived restart"

# Viewing secrets are deliberately not persisted in plaintext. An encrypted
# wallet therefore needs one unlock after process restart to repopulate its
# in-memory viewing cache; re-lock immediately and prove reorg scanning still
# works without retaining spend authority.
rpc_result wallet.unlock '["outgoing-lifecycle-pass",600]' >/dev/null
rpc_result wallet.lock '[]' >/dev/null
pass "one post-restart unlock repopulated viewing authority, then re-locked"

rpc_result blockchain.invalidateblock "[\"${TRANSFER_BLOCK}\"]" >/dev/null
DISCONNECTED="$(outgoing)"
jq -e --arg txid "${TXID}" \
    'all(.result.outputs[]; .txid != $txid)' <<<"${DISCONNECTED}" >/dev/null \
    || fail "disconnected output remained confirmed: ${DISCONNECTED}"
pass "disconnect removed stale confirmed sender history"

rpc_result blockchain.reconsiderblock "[\"${TRANSFER_BLOCK}\"]" >/dev/null
for _ in $(seq 1 60); do
    HEIGHT_NOW="$(rpc_result getblockcount '[]' | jq -r '.result')"
    [[ "${HEIGHT_NOW}" == "${TIP}" ]] && break
    sleep 1
done
[[ "${HEIGHT_NOW}" == "${TIP}" ]] || fail "reconsider did not restore tip"
RECONNECTED="$(outgoing)"
jq -e --arg txid "${TXID}" '.result.count >= 1 and any(.result.outputs[]; .txid == $txid and .confirmed == true)' \
    <<<"${RECONNECTED}" >/dev/null || fail "reconnect did not recompute outgoing note"
pass "reconnect/rescan recomputed sender history"

stop_node; start_node
FINAL="$(outgoing)"
jq -e --arg txid "${TXID}" '.result.count >= 1 and any(.result.outputs[]; .txid == $txid and .confirmed == true)' \
    <<<"${FINAL}" >/dev/null || fail "second restart lost recomputed history"
pass "second same-datadir restart preserved recomputed history"

# The note was discovered and persisted while locked, so its database row has
# no spend scalar. Unlocking must hydrate `s` from ask + Poseidon(ak,d), verify
# the independently stored nfk/composite ownership, and build a real proof.
rpc_result wallet.unlock '["outgoing-lifecycle-pass",600]' >/dev/null
SPEND="$(rpc_result wallet.unshield '{"amount_una":60000000,"fee_una":1000000}')"
SPEND_TXID="$(jq -r '.result.txid' <<<"${SPEND}")"
[[ -n "${SPEND_TXID}" && "${SPEND_TXID}" != null ]] \
    || fail "unlocked recipient-authorized spend returned no txid: ${SPEND}"
rpc_result generatetoaddress "[1,\"${MINER}\"]" >/dev/null
POST_SPEND="$(rpc_result wallet.listshielded '[]')"
jq -e 'any(.result.notes[]; .value_una == 70000000 and .spent == true)' \
    <<<"${POST_SPEND}" >/dev/null \
    || fail "recipient note was not marked spent after unlock/hydration: ${POST_SPEND}"
pass "unlock hydrated recipient-only spend authority and spent the locked-discovered note"

echo "=== SUCCESS: outgoing-view wallet lifecycle ==="
