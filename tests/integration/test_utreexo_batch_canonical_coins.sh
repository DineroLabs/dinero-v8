#!/usr/bin/env bash
# Batch proof verification must work for canonical coins absent from the local
# wallet index, and reject stale proofs after a real spend. Two isolated nodes
# use the same candidate binary; no wallet synchronization retry masks failure.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
PEER_DINEROD="${DINEROD}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dinero_batch_canonical.XXXXXX")"
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
        "${KEEP_ON_FAIL}" "canonical-coin daemon" "${DATA_DIR}" "${LOG_FILE}"
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
        --consensus-state-commitment-height=3 \
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
peer_result() { ( DATA_DIR="${PEER_DIR}"; RPC_PORT="${PEER_RPC}"; rpc_result "$@"; ); }
start_peer() {
    # Both nodes connect only to each other, never the regtest default seed.
    # Other agents may be running regtest daemons on this same machine.
    mkdir -p "${PEER_DIR}"
    "${PEER_DINEROD}" --regtest --datadir="${PEER_DIR}" \
        --rpcport="${PEER_RPC}" --port="${PEER_P2P}" --wallet-socket-port="${PEER_WALLET}" \
        --listen=1 --utreexo=1 --connect="127.0.0.1:${P2P_PORT}" \
        --consensus-shielded-epoch-reset-height=1 --consensus-shielded-spend-auth-height=2 \
        --consensus-state-commitment-height=3 \
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
wait_relay_ready() {
    local source_peers peer_peers
    # A restarted node can serve its persisted tip and proofs before reconnecting.
    # These two nodes have only each other as a connection target. Require a live,
    # handshaken peer on both ends before the one-shot transaction announcement.
    for _ in $(seq 1 60); do
        source_peers="$(rpc_result getpeerinfo '[]')"
        peer_peers="$(peer_result getpeerinfo '[]')"
        if jq -e '.result | any(.connected == true and .version > 0)' <<<"${source_peers}" >/dev/null \
            && jq -e '.result | any(.connected == true and .version > 0)' <<<"${peer_peers}" >/dev/null; then
            return 0
        fi
        sleep 1
    done
    fail "relay not ready after restart: source=${source_peers} peer=${peer_peers}"
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
command -v python3 >/dev/null || fail "python3 required"
[[ -x "${DINEROD}" ]] || fail "dinerod missing: ${DINEROD}"
[[ -x "${PEER_DINEROD}" ]] || fail "peer dinerod missing: ${PEER_DINEROD}"
read -r RPC_PORT P2P_PORT WALLET_PORT < <(dinero_allocate_port_triplet)
read -r PEER_RPC PEER_P2P PEER_WALLET < <(dinero_allocate_port_triplet)
start_peer
start_node

MINER="$(rpc_result wallet.getnewaddress '["taproot","batch-canonical-miner"]' | jq -r '.result.address // .result')"
rpc_result generatetoaddress "[110,\"${MINER}\"]" >/dev/null
wait_same_tip
# Wallet availability is a prerequisite only for constructing/signing the child.
# It is never a prerequisite for the non-owning peer to verify a Utreexo proof.
for _ in $(seq 1 60); do
    COIN="$(rpc_result wallet.listunspent '[105,9999999]' | jq -c '.result | map(select(.spendable == true))[0]')"
    [[ "${COIN}" != null ]] && break
    sleep 1
done
[[ "${COIN}" != null ]] || fail "no mature coin available for signed-child fixture"
TXID="$(jq -r '.txid' <<<"${COIN}")"
VOUT="$(jq -r '.vout' <<<"${COIN}")"
peer_result wallet.listunspent '[0,9999999]' | jq -e --arg txid "${TXID}" \
    'all(.result[]; .txid != $txid)' >/dev/null || fail "peer unexpectedly owns fixture coin"
peer_result gettxout "[\"${TXID}\",${VOUT},false]" | jq -e '.result != null' >/dev/null \
    || fail "peer is missing the canonical coin"

proof_on() {
    local rpc="$1" proofs verify
    proofs="$("${rpc}" blockchain.getutxoproofs_batch "[[{\"txid\":\"${TXID}\",\"vout\":${VOUT}}]]")"
    jq -e '.result.successful == 1 and .result.failed == 0' <<<"${proofs}" >/dev/null \
        || fail "canonical coin has no proof on ${rpc}: ${proofs}"
    verify="$("${rpc}" blockchain.verifyutxoproofs_batch "$(jq -c '[.result.proofs]' <<<"${proofs}")")"
    jq -e '.result.valid == 1 and .result.invalid == 0' <<<"${verify}" >/dev/null \
        || fail "canonical coin proof rejected on ${rpc}: ${verify}"
    jq -c '.result.proofs' <<<"${proofs}"
}
PROOFS="$(proof_on peer_result)"
proof_on rpc_result >/dev/null
pass "both nodes verify the same canonical coin, including the non-owning peer"

# Prove the root check still matters. A live outpoint alone is insufficient.
jq -e '.[0].proof.siblings | length > 0' <<<"${PROOFS}" >/dev/null \
    || fail "fixture has no sibling to corrupt"
BAD="$(jq -c '.[0].proof.siblings[0] = ("0" * 64)' <<<"${PROOFS}")"
REJECT="$(peer_result blockchain.verifyutxoproofs_batch "[${BAD}]")"
jq -e '.result.valid == 0 and .result.invalid == 1 and .result.results[0].error_code == "proof-invalid"' \
    <<<"${REJECT}" >/dev/null || fail "altered sibling accepted: ${REJECT}"
BAD="$(jq -c '.[0].txid = ("0" * 64)' <<<"${PROOFS}")"
REJECT="$(peer_result blockchain.verifyutxoproofs_batch "[${BAD}]")"
jq -e '.result.valid == 0 and .result.results[0].error_code == "utxo-not-found"' \
    <<<"${REJECT}" >/dev/null || fail "nonexistent outpoint accepted: ${REJECT}"

BEFORE="$(peer_result blockchain.getutreexocommitment '[]' | jq -r '.result.commitment')"
dinero_stop_process "${PEER_PID}" "canonical-coin peer restart" || fail "peer did not stop"
PEER_PID=""
start_peer
wait_same_tip
[[ "$(peer_result blockchain.getutreexocommitment '[]' | jq -r '.result.commitment')" == "${BEFORE}" ]] \
    || fail "restart changed commitment"
PROOFS="$(proof_on peer_result)"
pass "proof verifies after non-owning peer restart"

AMOUNT="$(jq -r '.amount' <<<"${COIN}")"
SCRIPT="$(jq -r '.scriptPubKey' <<<"${COIN}")"
CHILD_AMOUNT="$(jq -r '.amount - 0.001' <<<"${COIN}")"
RAW="$(rpc_result wallet.createrawtransaction "[[{\"txid\":\"${TXID}\",\"vout\":${VOUT}}],{\"${MINER}\":${CHILD_AMOUNT}}]" | jq -r '.result.hex')"
SIGNED="$(rpc_result wallet.signrawtransaction "[\"${RAW}\",[{\"txid\":\"${TXID}\",\"vout\":${VOUT},\"scriptPubKey\":\"${SCRIPT}\",\"amount\":${AMOUNT}}]]")"
jq -e '.result.complete == true' <<<"${SIGNED}" >/dev/null || fail "child signature incomplete"
wait_relay_ready
CHILD="$(rpc_result sendrawtransaction "[\"$(jq -r '.result.hex' <<<"${SIGNED}")\"]" | jq -r '.result | if type == "string" then . else .txid // .result end')"
peer_mine_tx "${CHILD}"
SPENT_BLOCK="$(peer_result getbestblockhash '[]' | jq -r '.result')"
for rpc in rpc_result peer_result; do
    REJECT="$("${rpc}" blockchain.verifyutxoproofs_batch "[${PROOFS}]")"
    jq -e '.result.valid == 0 and .result.invalid == 1 and .result.results[0].error_code == "utxo-not-found"' \
        <<<"${REJECT}" >/dev/null || fail "spent canonical coin accepted on ${rpc}: ${REJECT}"
done
pass "signed child consumes the exact coin; old proofs rejected on both nodes"

rpc_result blockchain.invalidateblock "[\"${SPENT_BLOCK}\"]" >/dev/null
peer_result blockchain.invalidateblock "[\"${SPENT_BLOCK}\"]" >/dev/null
wait_same_tip
proof_on peer_result >/dev/null
proof_on rpc_result >/dev/null
pass "disconnect restores canonical coin and proof on both nodes"
peer_result blockchain.reconsiderblock "[\"${SPENT_BLOCK}\"]" >/dev/null
rpc_result blockchain.reconsiderblock "[\"${SPENT_BLOCK}\"]" >/dev/null
wait_same_tip
for rpc in rpc_result peer_result; do
    REJECT="$("${rpc}" blockchain.verifyutxoproofs_batch "[${PROOFS}]")"
    jq -e '.result.valid == 0 and .result.results[0].error_code == "utxo-not-found"' \
        <<<"${REJECT}" >/dev/null || fail "reconnected spend left coin provable on ${rpc}"
done
pass "reconnect restores spentness without consulting wallet history"
echo '=== SUCCESS: batch proof verification uses canonical coins ==='
