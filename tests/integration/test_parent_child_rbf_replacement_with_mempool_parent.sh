#!/usr/bin/env bash
#
# Regression: a child that spends an unconfirmed wallet-owned parent output
# must remain replaceable via RBF. Validation and fee calculation must recover
# the parent output even though the original child already marks it spent in
# the mempool overlay.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/parent_child_rbf_replacement_with_mempool_parent_$$"
PORT_RPC="${PORT_RPC:-23156}"
PORT_P2P="${PORT_P2P:-23157}"
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

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

wait_for_mempool_txids() {
    local expected_json="$1"
    local max_wait=20
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        local actual
        actual="$(rpc_result "getrawmempool" "[]" | jq -c 'sort')"
        if [ "$actual" = "$expected_json" ]; then
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

reset_datadir() {
    rm -rf "$DATADIR"
    mkdir -p "$DATADIR"
    : > "$DATADIR/daemon.log"
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
    if [ "$KEEP_DATADIR" = "1" ]; then
        info "KEEP_DATADIR=1 preserving $DATADIR"
    else
        rm -rf "$DATADIR"
    fi
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon with opt-in RBF enabled"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["parent_child_rbf_replacement_with_mempool_parent"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
PARENT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_ONE="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_TWO="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty miner address"
[ -n "$PARENT_ADDR" ] || fail "wallet.getnewaddress returned empty parent address"
[ -n "$RECIPIENT_ONE" ] || fail "wallet.getnewaddress returned empty recipient one address"
[ -n "$RECIPIENT_TWO" ] || fail "wallet.getnewaddress returned empty recipient two address"

info "Mining 101 blocks to create mature funds"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null

info "Broadcasting parent receive to a wallet-owned address"
PARENT_TXID="$(rpc_scalar "wallet.sendtoaddress" "[\"$PARENT_ADDR\",1.0]" '.txid // empty')"
[ -n "$PARENT_TXID" ] || fail "wallet.sendtoaddress did not return parent txid"

wait_for_mempool_size 1 || fail "mempool never reached size 1 after parent broadcast"
wait_for_mempool_txids "[\"$PARENT_TXID\"]" || fail "parent tx did not become the sole mempool entry"

PARENT_SCRIPT="$(rpc_result "wallet.listaddresses" "[]" | jq --arg addr "$PARENT_ADDR" -r '.[] | select(.address == $addr) | .scriptPubKey' | head -n 1)"
[ -n "$PARENT_SCRIPT" ] && [ "$PARENT_SCRIPT" != "null" ] || fail "failed to locate authoritative scriptPubKey for parent address"

PARENT_TX="$(rpc_result "wallet.getrawtransaction" "[\"$PARENT_TXID\",true]")"
PARENT_VOUT="$(echo "$PARENT_TX" | jq --arg script "$PARENT_SCRIPT" -r '.vout[] | select(.scriptPubKey.hex == $script) | .n' | head -n 1)"
PARENT_AMOUNT="$(echo "$PARENT_TX" | jq --arg script "$PARENT_SCRIPT" -r '.vout[] | select(.scriptPubKey.hex == $script) | .value' | head -n 1)"
[ -n "$PARENT_VOUT" ] && [ "$PARENT_VOUT" != "null" ] || fail "failed to locate wallet-owned parent output"
[ -n "$PARENT_AMOUNT" ] && [ "$PARENT_AMOUNT" != "null" ] || fail "failed to locate parent amount"

info "Creating and broadcasting the original child spend"
RAW_CHILD_ONE="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$PARENT_TXID\",\"vout\":$PARENT_VOUT,\"sequence\":4294967293}],{\"$RECIPIENT_ONE\":0.999}]" '.hex // empty')"
[ -n "$RAW_CHILD_ONE" ] || fail "wallet.createrawtransaction returned empty child hex"

SIGNED_CHILD_ONE="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_CHILD_ONE\",[{\"txid\":\"$PARENT_TXID\",\"vout\":$PARENT_VOUT,\"scriptPubKey\":\"$PARENT_SCRIPT\",\"amount\":$PARENT_AMOUNT}]]")"
[ "$(echo "$SIGNED_CHILD_ONE" | jq -r '.complete')" = "true" ] || fail "original child should sign completely"
CHILD_ONE_HEX="$(echo "$SIGNED_CHILD_ONE" | jq -r '.hex // empty')"
[ -n "$CHILD_ONE_HEX" ] || fail "wallet.signrawtransaction returned empty child hex"
CHILD_ONE_TXID="$(rpc_scalar "wallet.sendrawtransaction" "[\"$CHILD_ONE_HEX\"]" '.txid // .result // . // empty')"
[ -n "$CHILD_ONE_TXID" ] || fail "wallet.sendrawtransaction did not return original child txid"

wait_for_mempool_size 2 || fail "mempool never reached size 2 after original child broadcast"
wait_for_mempool_txids "$(jq -cn --arg a "$CHILD_ONE_TXID" --arg b "$PARENT_TXID" '[$a,$b] | sort')" || fail "expected parent + original child in mempool"

info "Creating and broadcasting the higher-fee replacement child"
RAW_CHILD_TWO="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$PARENT_TXID\",\"vout\":$PARENT_VOUT,\"sequence\":4294967293}],{\"$RECIPIENT_TWO\":0.9985}]" '.hex // empty')"
[ -n "$RAW_CHILD_TWO" ] || fail "wallet.createrawtransaction returned empty replacement hex"

SIGNED_CHILD_TWO="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_CHILD_TWO\",[{\"txid\":\"$PARENT_TXID\",\"vout\":$PARENT_VOUT,\"scriptPubKey\":\"$PARENT_SCRIPT\",\"amount\":$PARENT_AMOUNT}]]")"
[ "$(echo "$SIGNED_CHILD_TWO" | jq -r '.complete')" = "true" ] || fail "replacement child should sign completely"
CHILD_TWO_HEX="$(echo "$SIGNED_CHILD_TWO" | jq -r '.hex // empty')"
[ -n "$CHILD_TWO_HEX" ] || fail "wallet.signrawtransaction returned empty replacement hex"
CHILD_TWO_TXID="$(rpc_scalar "wallet.sendrawtransaction" "[\"$CHILD_TWO_HEX\"]" '.txid // .result // . // empty')"
[ -n "$CHILD_TWO_TXID" ] || fail "wallet.sendrawtransaction did not return replacement child txid"
[ "$CHILD_TWO_TXID" != "$CHILD_ONE_TXID" ] || fail "replacement child txid must differ from original child txid"

wait_for_mempool_size 2 || fail "mempool never converged back to size 2 after replacement"
wait_for_mempool_txids "$(jq -cn --arg a "$CHILD_TWO_TXID" --arg b "$PARENT_TXID" '[$a,$b] | sort')" || fail "expected parent + replacement child in mempool"

RECIPIENT_ONE_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_ONE\"]")"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "recipient one unconfirmed overlay should clear after replacement"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.estimated_balance')" = "0" ] || fail "recipient one estimated balance should clear after replacement"

RECIPIENT_TWO_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_TWO\"]")"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "recipient two confirmed must stay zero while replacement is pending"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.balance')" = "0" ] || fail "recipient two balance must stay confirmed-only while replacement is pending"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.spendable')" = "0" ] || fail "recipient two spendable must stay zero while replacement is pending"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.unconfirmed')" = "99850000" ] || fail "recipient two unconfirmed delta mismatch after replacement"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.estimated_balance')" = "99850000" ] || fail "recipient two estimated balance mismatch after replacement"

PARENT_BALANCE="$(rpc_result "getaddressbalance" "[\"$PARENT_ADDR\"]")"
[ "$(echo "$PARENT_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "parent address confirmed must stay zero while package is pending"
[ "$(echo "$PARENT_BALANCE" | jq -r '.balance')" = "0" ] || fail "parent address balance must stay confirmed-only while package is pending"
[ "$(echo "$PARENT_BALANCE" | jq -r '.spendable')" = "0" ] || fail "parent address spendable must stay zero while package is pending"
[ "$(echo "$PARENT_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "parent address unconfirmed overlay must net to zero after replacement child"
[ "$(echo "$PARENT_BALANCE" | jq -r '.estimated_balance')" = "0" ] || fail "parent address estimated balance must net to zero after replacement child"

PARENT_MEMPOOL="$(rpc_result "getaddressmempool" "[\"$PARENT_ADDR\"]")"
[ "$(echo "$PARENT_MEMPOOL" | jq -r '.transactions | length')" = "2" ] || fail "expected parent address to expose parent + replacement child"
[ "$(echo "$PARENT_MEMPOOL" | jq --arg txid "$PARENT_TXID" -r '.transactions[] | select(.txid == $txid) | .type')" = "receive" ] || fail "parent tx should be classified as receive"
[ "$(echo "$PARENT_MEMPOOL" | jq --arg txid "$CHILD_TWO_TXID" -r '.transactions[] | select(.txid == $txid) | .type')" = "send" ] || fail "replacement child should be classified as send for parent address"

info "Mining one block to confirm parent + replacement child"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null
wait_for_mempool_size 0 || fail "mempool never emptied after confirmation"

RECIPIENT_TWO_CONFIRMED="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_TWO\"]")"
[ "$(echo "$RECIPIENT_TWO_CONFIRMED" | jq -r '.confirmed')" = "99850000" ] || fail "recipient two confirmed balance mismatch after mining replacement"
[ "$(echo "$RECIPIENT_TWO_CONFIRMED" | jq -r '.spendable')" = "99850000" ] || fail "recipient two spendable mismatch after mining replacement"
[ "$(echo "$RECIPIENT_TWO_CONFIRMED" | jq -r '.unconfirmed')" = "0" ] || fail "recipient two unconfirmed must clear after mining replacement"

pass "child spending a mempool parent remained replaceable and confirmed cleanly"
