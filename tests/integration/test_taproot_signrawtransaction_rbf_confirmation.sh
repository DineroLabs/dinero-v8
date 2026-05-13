#!/usr/bin/env bash
#
# Regression: when a raw taproot transaction is replaced by fee, only the
# replacement must survive into confirmed state.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/taproot_signrawtransaction_rbf_confirmation_$$"
PORT_RPC="${PORT_RPC:-23146}"
PORT_P2P="${PORT_P2P:-23147}"
DAEMON_PID=""

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }

fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [ -f "$DATADIR/daemon.log" ]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 160 "$DATADIR/daemon.log" >&2 || true
    fi
    exit 1
}

get_cookie() {
    cut -d: -f2 "$DATADIR/.cookie" 2>/dev/null || true
}

rpc_raw() {
    local method="$1"
    local params="${2:-[]}"
    local cookie
    cookie="$(get_cookie)"
    if [ -z "$cookie" ]; then
        echo '{"jsonrpc":"2.0","error":{"code":-1,"message":"missing rpc cookie"},"result":null}'
        return 1
    fi

    curl -sS -X POST "http://127.0.0.1:$PORT_RPC" \
        -u "__cookie__:$cookie" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}"
}

rpc_result() {
    local method="$1"
    local params="${2:-[]}"
    local response
    response="$(rpc_raw "$method" "$params")"
    if echo "$response" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "$response" | jq -r '.error.message // (.error | tostring)')"
        fail "RPC error [$method]: $emsg"
    fi
    echo "$response" | jq '.result'
}

rpc_scalar() {
    local method="$1"
    local params="$2"
    local jq_expr="$3"
    rpc_result "$method" "$params" | jq -r "$jq_expr"
}

wait_for_daemon() {
    local max_wait=30
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        if rpc_raw "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

wait_for_mempool_size() {
    local expected="$1"
    local max_wait=20
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        local count
        count="$(rpc_result "getrawmempool" "[]" | jq -r 'length')"
        if [ "$count" = "$expected" ]; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

start_daemon() {
    mkdir -p "$DATADIR"
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --mempool.enable_rbf=1 \
        --debug \
        >> "$DATADIR/daemon.log" 2>&1 &
    DAEMON_PID=$!
    wait_for_daemon || fail "daemon failed to start"
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
}

cleanup() {
    stop_daemon
    rm -rf "$DATADIR"
}

extract_sendraw_txid() {
    jq -r '.txid // .result // . // empty'
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon with opt-in RBF enabled"
start_daemon

rpc_result "wallet.createhd" '["taproot_signrawtransaction_rbf_confirmation"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
CHANGE_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_ONE="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_TWO="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

info "Mining 101 blocks to create a mature taproot UTXO"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null

UTXO="$(rpc_result "wallet.listunspent" "[101,9999999]" | jq 'map(select(.spendable == true))[0]')"
UTXO_TXID="$(echo "$UTXO" | jq -r '.txid')"
UTXO_VOUT="$(echo "$UTXO" | jq -r '.vout')"
UTXO_AMOUNT="$(echo "$UTXO" | jq -r '.amount')"
UTXO_SCRIPT="$(echo "$UTXO" | jq -r '.scriptPubKey')"
[ -n "$UTXO_TXID" ] && [ "$UTXO_TXID" != "null" ] || fail "expected a spendable UTXO"

RAW_ONE="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"sequence\":4294967293}],{\"$RECIPIENT_ONE\":1.0,\"$CHANGE_ADDR\":98.999}]" '.hex // empty')"
HEX_ONE="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_ONE\",[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"scriptPubKey\":\"$UTXO_SCRIPT\",\"amount\":$UTXO_AMOUNT}]]" | jq -r '.hex // empty')"
[ -n "$HEX_ONE" ] || fail "failed to sign tx1"
TX_ONE="$(rpc_result "wallet.sendrawtransaction" "[\"$HEX_ONE\"]" | extract_sendraw_txid)"
[ -n "$TX_ONE" ] || fail "failed to broadcast tx1"

RAW_TWO="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"sequence\":4294967293}],{\"$RECIPIENT_TWO\":0.999,\"$CHANGE_ADDR\":98.9999}]" '.hex // empty')"
HEX_TWO="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_TWO\",[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"scriptPubKey\":\"$UTXO_SCRIPT\",\"amount\":$UTXO_AMOUNT}]]" | jq -r '.hex // empty')"
[ -n "$HEX_TWO" ] || fail "failed to sign tx2"
TX_TWO="$(rpc_result "wallet.sendrawtransaction" "[\"$HEX_TWO\"]" | extract_sendraw_txid)"
[ -n "$TX_TWO" ] || fail "failed to broadcast tx2"
[ "$TX_TWO" != "$TX_ONE" ] || fail "replacement txid must differ from original txid"

wait_for_mempool_size 1 || fail "mempool never converged to the replacement"
[ "$(rpc_result "getrawmempool" "[]" | jq -r '.[0]')" = "$TX_TWO" ] || fail "replacement should be the only mempool transaction"

info "Mining a block to confirm the replacement"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null
wait_for_mempool_size 0 || fail "mempool did not clear after mining the replacement"

RECIPIENT_ONE_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_ONE\"]")"
RECIPIENT_TWO_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_TWO\"]")"

[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "replaced recipient must not receive confirmed funds"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "replaced recipient unconfirmed delta must clear"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.estimated_balance')" = "0" ] || fail "replaced recipient estimated balance must stay zero"

[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.confirmed')" = "99900000" ] || fail "replacement recipient confirmed balance mismatch"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "replacement recipient unconfirmed delta must clear"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.balance')" = "99900000" ] || fail "replacement recipient balance mismatch"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.estimated_balance')" = "99900000" ] || fail "replacement recipient estimated balance mismatch"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.spendable')" = "99900000" ] || fail "replacement recipient spendable balance mismatch"

[ "$(rpc_result "getaddressmempool" "[\"$RECIPIENT_ONE\"]" | jq -r '.transactions | length')" = "0" ] || fail "replaced recipient mempool overlay should be empty"
[ "$(rpc_result "getaddressmempool" "[\"$RECIPIENT_TWO\"]" | jq -r '.transactions | length')" = "0" ] || fail "replacement recipient mempool overlay should be empty"

pass "only the replacement raw taproot transaction survives into confirmed state"
