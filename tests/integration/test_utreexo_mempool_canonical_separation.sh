#!/usr/bin/env bash
#
# Regression: mempool activity must never mutate the canonical Utreexo state.
# The confirmed accumulator may change only when a block connects.
#

set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ -n "${DINEROD:-}" && -x "${DINEROD}" ]]; then
    DINEROD="${DINEROD}"
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    echo -e "${RED}FAIL:${NC} missing dinerod binary" >&2
    exit 1
fi

DATADIR="${DATADIR:-$(mktemp -d -t utreexo_mempool_canonical_XXXXXX)}"
RPC_PORT="${RPC_PORT:-$((23250 + RANDOM % 500))}"
P2P_PORT="${P2P_PORT:-$((RPC_PORT + 1))}"
KEEP_DATADIR="${KEEP_DATADIR:-0}"
DAEMON_PID=""

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }

fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [[ -f "${DATADIR}/daemon.log" ]]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 160 "${DATADIR}/daemon.log" >&2 || true
    fi
    exit 1
}

node_pattern() {
    echo "dinerod.*${DATADIR}"
}

is_node_running() {
    pgrep -f "$(node_pattern)" >/dev/null 2>&1
}

wait_for_node_state() {
    local desired=$1
    local timeout=$2
    local waited=0
    while [[ "${waited}" -lt "${timeout}" ]]; do
        if [[ "${desired}" == "running" ]] && is_node_running; then
            return 0
        fi
        if [[ "${desired}" == "stopped" ]] && ! is_node_running; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

get_cookie() {
    cut -d: -f2 "${DATADIR}/.cookie" 2>/dev/null || true
}

rpc_raw() {
    local method=$1
    local params=${2:-[]}
    local cookie
    cookie="$(get_cookie)"
    if [[ -z "${cookie}" ]]; then
        echo '{"jsonrpc":"2.0","error":{"code":-1,"message":"missing rpc cookie"},"result":null}'
        return 1
    fi

    curl -sS -X POST "http://127.0.0.1:${RPC_PORT}" \
        -u "__cookie__:${cookie}" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}" \
        2>/dev/null
}

rpc_result() {
    local method=$1
    local params=${2:-[]}
    local response
    response="$(rpc_raw "${method}" "${params}")"
    if echo "${response}" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "${response}" | jq -r '.error.message // (.error | tostring)')"
        fail "RPC error [${method}]: ${emsg}"
    fi
    echo "${response}" | jq '.result'
}

rpc_scalar() {
    local method=$1
    local params=$2
    local jq_expr=$3
    rpc_result "${method}" "${params}" | jq -r "${jq_expr}"
}

wait_for_rpc_ready() {
    local timeout=$1
    local waited=0
    while [[ "${waited}" -lt "${timeout}" ]]; do
        if rpc_raw "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

wait_for_mempool_size() {
    local expected=$1
    local timeout=${2:-30}
    local waited=0
    while [[ "${waited}" -lt "${timeout}" ]]; do
        local count
        count="$(rpc_result "getrawmempool" "[]" | jq -r 'length')"
        if [[ "${count}" == "${expected}" ]]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

start_daemon() {
    mkdir -p "${DATADIR}"
    : > "${DATADIR}/daemon.log"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATADIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --debug \
        >> "${DATADIR}/daemon.log" 2>&1 &
    DAEMON_PID=$!
    wait_for_node_state running 30 || true
    wait_for_rpc_ready 30 || fail "daemon RPC failed to start"
}

stop_daemon_clean() {
    if ! is_node_running; then
        return 0
    fi

    rpc_raw "stop" "[]" >/dev/null 2>&1 || true
    wait_for_node_state stopped 60 || fail "daemon failed to stop cleanly"
    wait "${DAEMON_PID}" 2>/dev/null || true
    DAEMON_PID=""
}

cleanup() {
    stop_daemon_clean || true
    if [[ "${KEEP_DATADIR}" == "1" ]]; then
        info "KEEP_DATADIR=1 preserving ${DATADIR}"
    else
        rm -rf "${DATADIR}"
    fi
}

capture_state_json() {
    local height tip commitment roots
    height="$(rpc_scalar "getblockcount" "[]" '.')"
    tip="$(rpc_scalar "getbestblockhash" "[]" '.')"
    commitment="$(rpc_raw "blockchain.getutreexocommitment" "[]")"
    roots="$(rpc_raw "blockchain.getutreexoroots" "[]")"

    jq -n \
        --argjson height "${height}" \
        --arg tip "${tip}" \
        --arg commitment "$(echo "${commitment}" | jq -r '.result.commitment // empty')" \
        --argjson num_leaves "$(echo "${commitment}" | jq -r '.result.num_leaves // 0')" \
        --argjson num_roots "$(echo "${commitment}" | jq -r '.result.num_roots // 0')" \
        --argjson roots "$(echo "${roots}" | jq -c '.result.roots // []')" \
        '{
            height: $height,
            tip: $tip,
            commitment: $commitment,
            num_leaves: $num_leaves,
            num_roots: $num_roots,
            roots: $roots
        }'
}

assert_same_state() {
    local lhs_json=$1
    local rhs_json=$2
    local label=$3
    local mismatch
    mismatch="$(jq -n \
        --argjson lhs "${lhs_json}" \
        --argjson rhs "${rhs_json}" \
        '{
            height: ($lhs.height == $rhs.height),
            tip: ($lhs.tip == $rhs.tip),
            commitment: ($lhs.commitment == $rhs.commitment),
            num_leaves: ($lhs.num_leaves == $rhs.num_leaves),
            num_roots: ($lhs.num_roots == $rhs.num_roots),
            roots: ($lhs.roots == $rhs.roots)
        }')"
    if [[ "$(echo "${mismatch}" | jq -r 'all(.[]; . == true)')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "lhs=$(echo "${lhs_json}" | jq -c '.')"
        echo "rhs=$(echo "${rhs_json}" | jq -c '.')"
        echo "eq =$(echo "${mismatch}" | jq -c '.')"
        fail "${label} state mismatch"
    fi
}

assert_state_advanced() {
    local before_json=$1
    local after_json=$2
    local label=$3
    local advanced
    advanced="$(jq -n \
        --argjson before "${before_json}" \
        --argjson after "${after_json}" \
        '{
            height_advanced: ($after.height > $before.height),
            commitment_changed: ($after.commitment != $before.commitment),
            roots_changed: ($after.roots != $before.roots or $after.num_leaves != $before.num_leaves or $after.num_roots != $before.num_roots)
        }')"
    if [[ "$(echo "${advanced}" | jq -r '.height_advanced and .commitment_changed and .roots_changed')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "before=$(echo "${before_json}" | jq -c '.')"
        echo "after =$(echo "${after_json}" | jq -c '.')"
        echo "eval =$(echo "${advanced}" | jq -c '.')"
        fail "${label} did not advance canonical Utreexo state as expected"
    fi
}

trap cleanup EXIT

command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting regtest daemon"
start_daemon

rpc_result "wallet.createhd" '["utreexo_mempool_canonical_separation"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
PARENT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
FINAL_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"
[[ -n "${PARENT_ADDR}" ]] || fail "wallet.getnewaddress returned empty parent address"
[[ -n "${FINAL_ADDR}" ]] || fail "wallet.getnewaddress returned empty final address"

info "Mining 110 blocks for mature funds"
rpc_result "generatetoaddress" "[110,\"${MINER_ADDR}\"]" >/dev/null
wait_for_mempool_size 0 10 || fail "mempool should be empty after bootstrap mining"

BASE_STATE="$(capture_state_json)"
pass "captured canonical baseline state"

info "Submitting pending parent receive"
PARENT_TXID="$(rpc_scalar "wallet.sendtoaddress" "[\"${PARENT_ADDR}\",1.0]" '.txid // empty')"
[[ -n "${PARENT_TXID}" ]] || fail "wallet.sendtoaddress did not return txid"
wait_for_mempool_size 1 30 || fail "mempool never reached size 1 after parent send"

PARENT_PENDING_STATE="$(capture_state_json)"
assert_same_state "${BASE_STATE}" "${PARENT_PENDING_STATE}" "pending parent receive"
pass "canonical Utreexo state stayed frozen after parent mempool admission"

PARENT_SCRIPT="$(rpc_result "wallet.listaddresses" "[]" | jq --arg addr "${PARENT_ADDR}" -r '.[] | select(.address == $addr) | .scriptPubKey' | head -n 1)"
[[ -n "${PARENT_SCRIPT}" && "${PARENT_SCRIPT}" != "null" ]] || fail "failed to resolve parent scriptPubKey"

PARENT_TX="$(rpc_result "wallet.getrawtransaction" "[\"${PARENT_TXID}\",true]")"
PARENT_VOUT="$(echo "${PARENT_TX}" | jq --arg script "${PARENT_SCRIPT}" -r '.vout[] | select(.scriptPubKey.hex == $script) | .n' | head -n 1)"
PARENT_AMOUNT="$(echo "${PARENT_TX}" | jq --arg script "${PARENT_SCRIPT}" -r '.vout[] | select(.scriptPubKey.hex == $script) | .value' | head -n 1)"
[[ -n "${PARENT_VOUT}" && "${PARENT_VOUT}" != "null" ]] || fail "failed to locate parent output index"
[[ -n "${PARENT_AMOUNT}" && "${PARENT_AMOUNT}" != "null" ]] || fail "failed to locate parent output amount"

info "Submitting child spend of the unconfirmed parent output"
RAW_CHILD="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"${PARENT_TXID}\",\"vout\":${PARENT_VOUT}}],{\"${FINAL_ADDR}\":0.999}]" '.hex // empty')"
[[ -n "${RAW_CHILD}" ]] || fail "wallet.createrawtransaction returned empty child tx"

SIGNED_CHILD="$(rpc_result "wallet.signrawtransaction" "[\"${RAW_CHILD}\",[{\"txid\":\"${PARENT_TXID}\",\"vout\":${PARENT_VOUT},\"scriptPubKey\":\"${PARENT_SCRIPT}\",\"amount\":${PARENT_AMOUNT}}]]")"
[[ "$(echo "${SIGNED_CHILD}" | jq -r '.complete')" == "true" ]] || fail "child transaction did not sign completely"
CHILD_HEX="$(echo "${SIGNED_CHILD}" | jq -r '.hex // empty')"
[[ -n "${CHILD_HEX}" ]] || fail "wallet.signrawtransaction returned empty signed child tx"

CHILD_TXID="$(rpc_scalar "wallet.sendrawtransaction" "[\"${CHILD_HEX}\"]" '.txid // .result // . // empty')"
[[ -n "${CHILD_TXID}" ]] || fail "wallet.sendrawtransaction did not return child txid"
wait_for_mempool_size 2 30 || fail "mempool never reached size 2 after child send"

CHAIN_PENDING_STATE="$(capture_state_json)"
assert_same_state "${BASE_STATE}" "${CHAIN_PENDING_STATE}" "pending parent-child chain"
pass "canonical Utreexo state stayed frozen through parent-child mempool chain"

info "Restarting daemon with mempool still live"
stop_daemon_clean
[[ -s "${DATADIR}/mempool.dat" ]] || fail "mempool.dat missing after clean stop"
start_daemon
wait_for_mempool_size 2 30 || fail "mempool entries did not reload after restart"

RESTART_STATE="$(capture_state_json)"
assert_same_state "${BASE_STATE}" "${RESTART_STATE}" "post-restart pending chain"
pass "canonical Utreexo state survived mempool-preserving restart unchanged"

info "Mining one confirmation block"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null
wait_for_mempool_size 0 30 || fail "mempool did not clear after confirmation block"

CONFIRMED_STATE="$(capture_state_json)"
assert_state_advanced "${BASE_STATE}" "${CONFIRMED_STATE}" "confirmed block connect"
pass "canonical Utreexo state advanced only when a block connected"

echo -e "${GREEN}SUCCESS:${NC} mempool activity stayed separate from canonical Utreexo state"
