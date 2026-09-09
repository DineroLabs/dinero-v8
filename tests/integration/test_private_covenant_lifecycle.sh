#!/usr/bin/env bash
# Isolated two-node private covenant wallet lifecycle. Never connects to production.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dinero_private_covenant.XXXXXX")"
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
        --consensus-shielded-spend-auth-height=2 --consensus-private-covenant-height=3 \
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
        --consensus-shielded-epoch-reset-height=1 --consensus-shielded-spend-auth-height=2 --consensus-private-covenant-height=3 \
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

OWNER="$(rpc_result wallet.getshieldedaddress '{"account":0,"j":0}' | jq -r '.result.address')"
HEIGHT="$(rpc_result getblockcount '[]' | jq -r '.result')"
MINIMUM=$((HEIGHT + 3))
PARAMS="$(jq -nc --arg owner "$OWNER" --arg recipient "$RECIPIENT" --argjson height "$MINIMUM" '{owner_address:$owner,minimum_height:$height,spend_fee_una:1000000,fee_una:1000000,outputs:[{address:$recipient,value_una:40000000},{address:$recipient,value_una:20000000}]}')"
BAD="$(rpc_call wallet.covenant.privatefund "$(jq '.owner_address=""' <<<"$PARAMS")")"
jq -e '.result.error == "invalid_private_covenant" or .error != null' <<<"$BAD" >/dev/null || fail "empty owner accepted: $BAD"
FUND="$(rpc_result wallet.covenant.privatefund "$PARAMS")"
TXID="$(jq -r '.result.txid' <<<"$FUND")"
[[ ${#TXID} == 64 ]] || fail "private funding failed: $FUND"
peer_mine_tx "$TXID"
NOTES="$(rpc_result wallet.listshielded '[]')"
LEAF="$(jq -r '.result.notes[] | select(.private_covenant and .confirmed and (.spent|not)) | .leaf_index' <<<"$NOTES")"
CM="$(jq -r '.result.notes[] | select(.private_covenant and .confirmed and (.spent|not)) | .commitment_hex' <<<"$NOTES")"
[[ "$LEAF" =~ ^[0-9]+$ ]] || fail "private funding not recovered: $NOTES"
EARLY="$(rpc_call wallet.covenant.privatespend "{\"leaf_index\":$LEAF,\"commitment_hex\":\"$CM\"}")"
jq -e '.result.error != null or .error != null' <<<"$EARLY" >/dev/null || fail "immature spend accepted: $EARLY"
ORDINARY="$(rpc_call wallet.unshield '{"amount_una":50000000,"fee_una":1000000}')"
jq -e '.result.error != null or .error != null' <<<"$ORDINARY" >/dev/null || fail "ordinary selection spent covenant: $ORDINARY"
WRONG="$(rpc_call wallet.covenant.privatespend "{\"leaf_index\":$LEAF,\"commitment_hex\":\"$(printf '%064d' 0)\"}")"
jq -e '(.result.error // .error.message) == "private_covenant_commitment_changed"' <<<"$WRONG" >/dev/null || fail "wrong commitment accepted: $WRONG"
pass "private note recovered; maturity and ordinary spending fail closed"
BLOCK="$(rpc_result getbestblockhash '[]' | jq -r '.result')"
rpc_result wallet.encrypt '["private-covenant-pass"]' >/dev/null
stop_node
start_node
NOTES="$(rpc_result wallet.listshielded '[]')"
jq -e --argjson leaf "$LEAF" 'any(.result.notes[]; .leaf_index==$leaf and .private_covenant and .outputs[0].value_una==40000000)' <<<"$NOTES" >/dev/null || fail "restart lost descriptor: $NOTES"
rpc_result wallet.unlock '["private-covenant-pass",600]' >/dev/null
rpc_result wallet.lock '[]' >/dev/null
rpc_result blockchain.invalidateblock "[\"$BLOCK\"]" >/dev/null
rpc_result blockchain.reconsiderblock "[\"$BLOCK\"]" >/dev/null
wait_same_tip
NOTES="$(rpc_result wallet.listshielded '[]')"
jq -e --argjson leaf "$LEAF" 'any(.result.notes[]; .leaf_index==$leaf and .private_covenant and .confirmed)' <<<"$NOTES" >/dev/null || fail "reorg lost private descriptor: $NOTES"
pass "descriptor survived encrypted restart and locked reorg recovery"
rpc_result wallet.unlock '["private-covenant-pass",600]' >/dev/null
rpc_result generatetoaddress "[2,\"$MINER\"]" >/dev/null
wait_same_tip
# A canonical ingress rejection must undo the pending-spent reservation.
stop_node
DINERO_TEST_REJECT_SHIELDED_WALLET_SUBMIT=1 start_node
rpc_result wallet.unlock '["private-covenant-pass",600]' >/dev/null
REJECTED="$(rpc_call wallet.covenant.privatespend "{\"leaf_index\":$LEAF,\"commitment_hex\":\"$CM\"}")"
jq -e '.error.data.wallet_rollback == "complete"' <<<"$REJECTED" >/dev/null || fail "rejected private spend did not roll back: $REJECTED"
NOTES="$(rpc_result wallet.listshielded '[]')"
jq -e --arg cm "$CM" 'any(.result.notes[]; .commitment_hex==$cm and (.spent|not))' <<<"$NOTES" >/dev/null || fail "rejected note remained reserved: $NOTES"
stop_node
start_node
rpc_result wallet.unlock '["private-covenant-pass",600]' >/dev/null
pass "rejected private spend rolled back and survived restart"
SPEND="$(rpc_result wallet.covenant.privatespend "{\"leaf_index\":$LEAF,\"commitment_hex\":\"$CM\"}")"
TXID="$(jq -r '.result.txid' <<<"$SPEND")"
[[ ${#TXID} == 64 ]] || fail "private spend failed: $SPEND"
peer_mine_tx "$TXID"
NOTES="$(rpc_result wallet.listshielded '[]')"
jq -e 'any(.result.notes[]; .value_una==40000000 and .confirmed and (.private_covenant|not)) and any(.result.notes[]; .value_una==20000000 and .confirmed)' <<<"$NOTES" >/dev/null || fail "private payees not recovered: $NOTES"
pass "private covenant paid both committed shielded outputs"
HEIGHT="$(rpc_result getblockcount '[]' | jq -r '.result')"
PARAMS="$(jq -nc --arg owner "$OWNER" --arg recipient "$RECIPIENT" --argjson height "$HEIGHT" '{source:"private",owner_address:$owner,minimum_height:$height,spend_fee_una:1000000,fee_una:1000000,outputs:[{address:$recipient,value_una:10000000}]}')"
FUND="$(rpc_result wallet.covenant.privatefund "$PARAMS")"
TXID="$(jq -r '.result.txid' <<<"$FUND")"
[[ ${#TXID} == 64 ]] || fail "shielded-source covenant failed: $FUND"
peer_mine_tx "$TXID"
NOTES="$(rpc_result wallet.listshielded '[]')"
LEAF="$(jq -r '.result.notes[] | select(.private_covenant and .confirmed and (.spent|not)) | .leaf_index' <<<"$NOTES")"
CM="$(jq -r '.result.notes[] | select(.private_covenant and .confirmed and (.spent|not)) | .commitment_hex' <<<"$NOTES")"
[[ "$LEAF" =~ ^[0-9]+$ ]] || fail "shielded-source contract not recovered: $NOTES"
SPEND="$(rpc_result wallet.covenant.privatespend "{\"leaf_index\":$LEAF,\"commitment_hex\":\"$CM\"}")"
TXID="$(jq -r '.result.txid' <<<"$SPEND")"
[[ ${#TXID} == 64 ]] || fail "second private spend failed: $SPEND"
peer_mine_tx "$TXID"
FINAL="$(rpc_result wallet.unshield '{"amount_una":9000000,"fee_una":1000000}')"
TXID="$(jq -r '.result.txid' <<<"$FINAL")"
[[ ${#TXID} == 64 ]] || fail "recipient final spend failed: $FINAL"
peer_mine_tx "$TXID"
pass "public and private funding, recovery and recipient spending complete"
