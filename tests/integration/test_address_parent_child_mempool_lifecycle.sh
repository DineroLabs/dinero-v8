#!/usr/bin/env bash
#
# Regression: address-index RPCs must treat an address-owned mempool parent
# plus descendant child spend as a net-zero advisory balance until the package
# confirms, while still exposing both mempool entries explicitly.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/address_parent_child_mempool_lifecycle_$$"
PORT_RPC="${PORT_RPC:-23148}"
PORT_P2P="${PORT_P2P:-23149}"
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

capture_canonical_state_json() {
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

assert_same_canonical_state() {
    local lhs_json="$1"
    local rhs_json="$2"
    local label="$3"
    local equal
    equal="$(jq -n \
        --argjson lhs "${lhs_json}" \
        --argjson rhs "${rhs_json}" \
        '($lhs.height == $rhs.height) and
         ($lhs.tip == $rhs.tip) and
         ($lhs.commitment == $rhs.commitment) and
         ($lhs.num_leaves == $rhs.num_leaves) and
         ($lhs.num_roots == $rhs.num_roots) and
         ($lhs.roots == $rhs.roots)')"
    [ "${equal}" = "true" ] || fail "${label} changed canonical Utreexo state unexpectedly"
}

assert_canonical_state_advanced() {
    local before_json="$1"
    local after_json="$2"
    local label="$3"
    local advanced
    advanced="$(jq -n \
        --argjson before "${before_json}" \
        --argjson after "${after_json}" \
        '($after.height > $before.height) and
         ($after.tip != $before.tip) and
         ($after.commitment != $before.commitment) and
         (($after.roots != $before.roots) or
          ($after.num_leaves != $before.num_leaves) or
          ($after.num_roots != $before.num_roots))')"
    [ "${advanced}" = "true" ] || fail "${label} failed to advance canonical Utreexo state"
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

wait_for_address_mempool_count() {
    local address="$1"
    local expected="$2"
    local max_wait=20
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        local count
        count="$(rpc_result "getaddressmempool" "[\"$address\"]" | jq -r '.transactions | length')"
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

assert_zero_address_overlay() {
    local address="$1"
    local balance
    local mempool
    balance="$(rpc_result "getaddressbalance" "[\"$address\"]")"
    mempool="$(rpc_result "getaddressmempool" "[\"$address\"]")"

    [ "$(echo "$balance" | jq -r '.confirmed')" = "0" ] || fail "expected 0 confirmed for $address"
    [ "$(echo "$balance" | jq -r '.unconfirmed')" = "0" ] || fail "expected 0 unconfirmed for $address"
    [ "$(echo "$balance" | jq -r '.balance')" = "0" ] || fail "expected 0 balance for $address"
    [ "$(echo "$balance" | jq -r '.estimated_balance')" = "0" ] || fail "expected 0 estimated balance for $address"
    [ "$(echo "$balance" | jq -r '.spendable')" = "0" ] || fail "expected 0 spendable for $address"
    [ "$(echo "$mempool" | jq -r '.transactions | length')" = "0" ] || fail "expected empty mempool overlay for $address"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["address_parent_child_mempool_lifecycle"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
PARENT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
FINAL_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty miner address"
[ -n "$PARENT_ADDR" ] || fail "wallet.getnewaddress returned empty parent address"
[ -n "$FINAL_ADDR" ] || fail "wallet.getnewaddress returned empty final address"

info "Mining 101 blocks to create mature spendable funds"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null
CANONICAL_BEFORE_PACKAGE="$(capture_canonical_state_json)"

assert_zero_address_overlay "$PARENT_ADDR"
assert_zero_address_overlay "$FINAL_ADDR"

info "Broadcasting parent receive to wallet-owned address"
PARENT_TXID="$(rpc_scalar "wallet.sendtoaddress" "[\"$PARENT_ADDR\",1.0]" '.txid // empty')"
[ -n "$PARENT_TXID" ] || fail "wallet.sendtoaddress did not return txid"

wait_for_mempool_size 1 || fail "mempool never reached size 1 after parent broadcast"
wait_for_address_mempool_count "$PARENT_ADDR" 1 || fail "parent address never observed parent receive"
assert_same_canonical_state "$CANONICAL_BEFORE_PACKAGE" "$(capture_canonical_state_json)" \
    "wallet parent receive"

PARENT_PENDING_BALANCE="$(rpc_result "getaddressbalance" "[\"$PARENT_ADDR\"]")"
[ "$(echo "$PARENT_PENDING_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "parent address confirmed must stay zero while parent is pending"
[ "$(echo "$PARENT_PENDING_BALANCE" | jq -r '.balance')" = "0" ] || fail "parent address balance must stay confirmed-only while parent is pending"
[ "$(echo "$PARENT_PENDING_BALANCE" | jq -r '.spendable')" = "0" ] || fail "parent address spendable must stay zero while parent is pending"
[ "$(echo "$PARENT_PENDING_BALANCE" | jq -r '.unconfirmed')" = "100000000" ] || fail "parent address unconfirmed delta mismatch after parent receive"
[ "$(echo "$PARENT_PENDING_BALANCE" | jq -r '.estimated_balance')" = "100000000" ] || fail "parent address estimated balance mismatch after parent receive"

PARENT_SCRIPT="$(rpc_result "wallet.listaddresses" "[]" | jq --arg addr "$PARENT_ADDR" -r '.[] | select(.address == $addr) | .scriptPubKey' | head -n 1)"
[ -n "$PARENT_SCRIPT" ] && [ "$PARENT_SCRIPT" != "null" ] || fail "failed to locate authoritative scriptPubKey for parent address"

PARENT_TX="$(rpc_result "wallet.getrawtransaction" "[\"$PARENT_TXID\",true]")"
PARENT_VOUT="$(echo "$PARENT_TX" | jq --arg script "$PARENT_SCRIPT" -r '.vout[] | select(.scriptPubKey.hex == $script) | .n' | head -n 1)"
PARENT_AMOUNT="$(echo "$PARENT_TX" | jq --arg script "$PARENT_SCRIPT" -r '.vout[] | select(.scriptPubKey.hex == $script) | .value' | head -n 1)"
[ -n "$PARENT_VOUT" ] && [ "$PARENT_VOUT" != "null" ] || fail "failed to locate wallet-owned parent output"
[ -n "$PARENT_AMOUNT" ] && [ "$PARENT_AMOUNT" != "null" ] || fail "failed to locate parent amount"

info "Creating child spend of the unconfirmed parent output"
RAW_CHILD="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$PARENT_TXID\",\"vout\":$PARENT_VOUT}],{\"$FINAL_ADDR\":0.999}]" '.hex // empty')"
[ -n "$RAW_CHILD" ] || fail "wallet.createrawtransaction returned empty child hex"

SIGNED_CHILD="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_CHILD\",[{\"txid\":\"$PARENT_TXID\",\"vout\":$PARENT_VOUT,\"scriptPubKey\":\"$PARENT_SCRIPT\",\"amount\":$PARENT_AMOUNT}]]")"
[ "$(echo "$SIGNED_CHILD" | jq -r '.complete')" = "true" ] || fail "child transaction should sign completely"
CHILD_HEX="$(echo "$SIGNED_CHILD" | jq -r '.hex // empty')"
[ -n "$CHILD_HEX" ] || fail "wallet.signrawtransaction returned empty child hex"

CHILD_TXID="$(rpc_scalar "wallet.sendrawtransaction" "[\"$CHILD_HEX\"]" '.txid // .result // . // empty')"
[ -n "$CHILD_TXID" ] || fail "wallet.sendrawtransaction did not return child txid"
[ "$CHILD_TXID" != "$PARENT_TXID" ] || fail "child txid must differ from parent txid"

wait_for_mempool_size 2 || fail "mempool never reached size 2 after child broadcast"
wait_for_address_mempool_count "$PARENT_ADDR" 2 || fail "parent address never exposed parent+child package"
wait_for_address_mempool_count "$FINAL_ADDR" 1 || fail "final address never observed child receive"
assert_same_canonical_state "$CANONICAL_BEFORE_PACKAGE" "$(capture_canonical_state_json)" \
    "wallet parent/child mempool package"

PARENT_NET_ZERO_BALANCE="$(rpc_result "getaddressbalance" "[\"$PARENT_ADDR\"]")"
[ "$(echo "$PARENT_NET_ZERO_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "parent address confirmed must stay zero while package is pending"
[ "$(echo "$PARENT_NET_ZERO_BALANCE" | jq -r '.balance')" = "0" ] || fail "parent address balance must stay confirmed-only while package is pending"
[ "$(echo "$PARENT_NET_ZERO_BALANCE" | jq -r '.spendable')" = "0" ] || fail "parent address spendable must stay zero while package is pending"
[ "$(echo "$PARENT_NET_ZERO_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "parent address unconfirmed must net to zero after child spend"
[ "$(echo "$PARENT_NET_ZERO_BALANCE" | jq -r '.estimated_balance')" = "0" ] || fail "parent address estimated balance must net to zero after child spend"

PARENT_MEMPOOL="$(rpc_result "getaddressmempool" "[\"$PARENT_ADDR\"]")"
[ "$(echo "$PARENT_MEMPOOL" | jq -r '.transactions | length')" = "2" ] || fail "expected parent address to expose both parent and child mempool entries"
[ "$(echo "$PARENT_MEMPOOL" | jq --arg txid "$PARENT_TXID" -r '.transactions[] | select(.txid == $txid) | .type')" = "receive" ] || fail "parent tx should be classified as receive"
[ "$(echo "$PARENT_MEMPOOL" | jq --arg txid "$PARENT_TXID" -r '.transactions[] | select(.txid == $txid) | .amount')" = "100000000" ] || fail "parent receive amount mismatch"
[ "$(echo "$PARENT_MEMPOOL" | jq --arg txid "$CHILD_TXID" -r '.transactions[] | select(.txid == $txid) | .type')" = "send" ] || fail "child tx should be classified as send for parent address"
[ "$(echo "$PARENT_MEMPOOL" | jq --arg txid "$CHILD_TXID" -r '.transactions[] | select(.txid == $txid) | .amount')" = "100000000" ] || fail "child spend amount mismatch for parent address"

FINAL_PENDING_BALANCE="$(rpc_result "getaddressbalance" "[\"$FINAL_ADDR\"]")"
[ "$(echo "$FINAL_PENDING_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "final address confirmed must stay zero while child is pending"
[ "$(echo "$FINAL_PENDING_BALANCE" | jq -r '.balance')" = "0" ] || fail "final address balance must stay confirmed-only while child is pending"
[ "$(echo "$FINAL_PENDING_BALANCE" | jq -r '.spendable')" = "0" ] || fail "final address spendable must stay zero while child is pending"
[ "$(echo "$FINAL_PENDING_BALANCE" | jq -r '.unconfirmed')" = "99900000" ] || fail "final address unconfirmed delta mismatch"
[ "$(echo "$FINAL_PENDING_BALANCE" | jq -r '.estimated_balance')" = "99900000" ] || fail "final address estimated balance mismatch"

FINAL_MEMPOOL="$(rpc_result "getaddressmempool" "[\"$FINAL_ADDR\"]")"
[ "$(echo "$FINAL_MEMPOOL" | jq -r '.transactions | length')" = "1" ] || fail "expected one child receive entry for final address"
[ "$(echo "$FINAL_MEMPOOL" | jq -r '.transactions[0].txid')" = "$CHILD_TXID" ] || fail "final address mempool txid mismatch"
[ "$(echo "$FINAL_MEMPOOL" | jq -r '.transactions[0].type')" = "receive" ] || fail "final address mempool entry must be receive"
[ "$(echo "$FINAL_MEMPOOL" | jq -r '.transactions[0].amount')" = "99900000" ] || fail "final address mempool amount mismatch"

info "Mining a block to confirm the parent+child package"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null
wait_for_mempool_size 0 || fail "mempool did not clear after package confirmation"
assert_canonical_state_advanced "$CANONICAL_BEFORE_PACKAGE" "$(capture_canonical_state_json)" \
    "wallet parent/child confirmation"

assert_zero_address_overlay "$PARENT_ADDR"

FINAL_CONFIRMED_BALANCE="$(rpc_result "getaddressbalance" "[\"$FINAL_ADDR\"]")"
[ "$(echo "$FINAL_CONFIRMED_BALANCE" | jq -r '.confirmed')" = "99900000" ] || fail "final address confirmed balance mismatch after mining"
[ "$(echo "$FINAL_CONFIRMED_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "final address unconfirmed delta must clear after mining"
[ "$(echo "$FINAL_CONFIRMED_BALANCE" | jq -r '.balance')" = "99900000" ] || fail "final address balance mismatch after mining"
[ "$(echo "$FINAL_CONFIRMED_BALANCE" | jq -r '.estimated_balance')" = "99900000" ] || fail "final address estimated balance mismatch after mining"
[ "$(echo "$FINAL_CONFIRMED_BALANCE" | jq -r '.spendable')" = "99900000" ] || fail "final address spendable balance mismatch after mining"
[ "$(rpc_result "getaddressmempool" "[\"$FINAL_ADDR\"]" | jq -r '.transactions | length')" = "0" ] || fail "final address mempool overlay should be empty after mining"

pass "parent/child address-index overlay nets to zero until confirmation and then collapses into confirmed state"
