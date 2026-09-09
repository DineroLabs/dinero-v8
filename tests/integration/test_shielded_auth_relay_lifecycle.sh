#!/usr/bin/env bash
# Outgoing-view lifecycle rehearsal (dormant on production networks).
#
# Drives the paired regtest spend-authority/outgoing-view cutover and proves:
#   provisional sender recovery -> lock -> recipient discovery while locked ->
#   confirmation -> restart -> disconnect -> reconnect/rescan -> second restart
#   -> unlock -> recipient-authorized spend. The recipient is account 1 of the
# same wallet so incoming scanning and outgoing recovery are exercised through
# independent authorities while a two regtest processes exercise actual relay and peer mining while keeping the fixture
# deterministic.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dinero_outgoing_recovery.XXXXXX")"
LOG_FILE="${DATA_DIR}.log"
PID=""
PEER_PID=""
PEER_DIR="${DATA_DIR}.peer"
PEER_LOG="${PEER_DIR}.log"
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
    dinero_cleanup_single_daemon "${rc}" "${PEER_PID}" "${PEER_DIR}" \
        "${KEEP_ON_FAIL}" "shielded relay peer" "${PEER_DIR}" "${PEER_LOG}"
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
        --listen=1 --utreexo=1 --connect="127.0.0.1:${PEER_P2P}" \
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
peer_result() { ( DATA_DIR="${PEER_DIR}"; RPC_PORT="${PEER_RPC}"; rpc_result "$@"; ); }
start_peer() {
    # Both nodes connect only to each other, never the regtest default seed.
    # Other agents may be running regtest daemons on this same machine.
    mkdir -p "${PEER_DIR}"
    "${DINEROD}" --regtest --datadir="${PEER_DIR}" \
        --rpcport="${PEER_RPC}" --port="${PEER_P2P}" --wallet-socket-port="${PEER_WALLET}" \
        --listen=1 --utreexo=1 --connect="127.0.0.1:${P2P_PORT}" \
        --consensus-shielded-epoch-reset-height=1 --consensus-shielded-spend-auth-height=2 \
        >>"${PEER_LOG}" 2>&1 &
    PEER_PID=$!
    ( DATA_DIR="${PEER_DIR}"; RPC_PORT="${PEER_RPC}"; PID="${PEER_PID}"; wait_rpc; ) \
        || fail "relay peer did not start"
}
wait_same_tip() {
    for _ in $(seq 1 180); do
        local a b
        a="$(rpc_result getbestblockhash '[]' | jq -r '.result')"
        b="$(peer_result getbestblockhash '[]' | jq -r '.result')"
        [[ "${a}" == "${b}" ]] && return 0
        sleep 1
    done
    fail "nodes did not converge on the same tip"
}
peer_mine_tx() {
    local txid="$1" found=0
    for _ in $(seq 1 180); do
        if peer_result getrawmempool '[]' | jq -e --arg txid "${txid}" '.result | index($txid) != null' >/dev/null; then found=1; break; fi
        sleep 1
    done
    [[ "${found}" == 1 ]] || fail "peer did not admit relayed tx ${txid}; see ${PEER_LOG}"
    pass "independent peer received and admitted ${txid}"
    peer_result generatetoaddress "[1,\"${MINER}\"]" >/dev/null
    wait_same_tip
    local hash block
    hash="$(peer_result getbestblockhash '[]' | jq -r '.result')"
    block="$(peer_result getblock "[\"${hash}\",1]")"
    jq -e --arg txid "${txid}" '.result.tx | map(if type == "object" then .txid else . end) | index($txid) != null' \
        <<<"${block}" >/dev/null || fail "peer mined a block without ${txid}: ${block}"
    pass "peer mined the relayed transaction and source accepted its block"
}


command -v curl >/dev/null || fail "curl required"
command -v jq >/dev/null || fail "jq required"
[[ -x "${DINEROD}" ]] || fail "dinerod missing: ${DINEROD}"
read -r RPC_PORT P2P_PORT WALLET_PORT < <(dinero_allocate_port_triplet)
read -r PEER_RPC PEER_P2P PEER_WALLET < <(dinero_allocate_port_triplet)
start_peer
start_node

MINER="$(rpc_result wallet.getnewaddress '["taproot","outgoing-miner"]' | jq -r '.result.address // .result')"
rpc_result generatetoaddress "[101,\"${MINER}\"]" >/dev/null
wait_same_tip
RECIPIENT="$(rpc_result wallet.getshieldedaddress '{"account":1,"j":0}' | jq -r '.result.address')"
[[ "${RECIPIENT}" == rdins1* ]] || fail "bad recipient address"

info "shielding an initial 100,000,000-una note"
SHIELD="$(rpc_result wallet.shield '{"amount_una":100000000,"fee_una":1000000}')"
SHIELD_TXID="$(jq -r '.result.txid' <<<"${SHIELD}")"
[[ ${#SHIELD_TXID} == 64 ]] || fail "shield submission failed: ${SHIELD}"
peer_mine_tx "${SHIELD_TXID}"
INITIAL_NOTES="$(rpc_result wallet.listshielded '[]')"
jq -e 'any(.result.notes[]; .value_una == 100000000 and .confirmed == true)' \
    <<<"${INITIAL_NOTES}" >/dev/null || fail "initial shield did not confirm: ${INITIAL_NOTES}"
info "transferring to account 1 with recipient and change outputs"
SEND="$(rpc_result wallet.transfer "{\"address\":\"${RECIPIENT}\",\"amount_una\":70000000,\"fee_una\":1000000,\"memo\":\"sender recovery lifecycle\"}")"
TXID="$(jq -r '.result.txid' <<<"${SEND}")"
[[ ${#TXID} == 64 ]] || fail "transfer submission failed: ${SEND}"
PROVISIONAL="$(outgoing)"
jq -e --arg txid "${TXID}" \
    --arg address "${RECIPIENT}" \
    '([.result.outputs[] | select(.txid == $txid)] | length) == 2 and any(.result.outputs[]; .txid == $txid and .recipient_address == $address and .confirmed == false and .value_una == 70000000 and (.memo_hex | startswith("73656e646572207265636f76657279206c6966656379636c65")))' \
    <<<"${PROVISIONAL}" >/dev/null || fail "provisional outgoing record missing: ${PROVISIONAL}"
pass "sender recovered provisional recipient/value/memo metadata"

# Cache full-viewing authority, then remove spend authority before the output
# reaches the chain. Recognition below must therefore use (ak,nk,nvk,ovk), not
# a master seed or a spend scalar left in memory.
rpc_result wallet.encrypt '["outgoing-lifecycle-pass"]' >/dev/null
LOCKED_STATUS="$(rpc_result wallet.status '[]')"
jq -e '.result.locked == true' <<<"${LOCKED_STATUS}" >/dev/null \
    || fail "wallet did not lock after encryption: ${LOCKED_STATUS}"

peer_mine_tx "${TXID}"
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
peer_mine_tx "${SPEND_TXID}"
POST_SPEND="$(rpc_result wallet.listshielded '[]')"
jq -e 'any(.result.notes[]; .value_una == 70000000 and .spent == true)' \
    <<<"${POST_SPEND}" >/dev/null \
    || fail "recipient note was not marked spent after unlock/hydration: ${POST_SPEND}"
pass "unlock hydrated recipient-only spend authority and spent the locked-discovered note"

echo "=== SUCCESS: two-node Auth transfer/recovery lifecycle ==="
