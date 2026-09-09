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
export DINERO_PRIVATE_GUI_LIFECYCLE=1
export DINERO_RPC_URL="http://127.0.0.1:${RPC_PORT}/"
export DINERO_PRIVATE_GUI_DATADIR="$DATA_DIR"
export DINERO_PRIVATE_GUI_OWNER="$OWNER"
export DINERO_PRIVATE_GUI_RECIPIENT="$RECIPIENT"
export DINERO_PRIVATE_GUI_MINER="$MINER"
export QT_QPA_PLATFORM=offscreen
"${DINERO_QT_PRIVATE_TEST:-$ROOT_DIR/build-qt/bin/test_private_covenant_widget}" realDaemonFundingAndPayment
