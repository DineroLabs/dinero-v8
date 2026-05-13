#!/usr/bin/env bash
#
# Regression: address-index balance must keep confirmed chain truth separate
# from the mempool overlay.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/address_balance_mempool_overlay_$$"
PORT_RPC=23140
PORT_P2P=23139
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
        tail -n 120 "$DATADIR/daemon.log" >&2 || true
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

wait_for_mempool_address() {
    local address="$1"
    local max_wait=20
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        local count
        count="$(rpc_result "getaddressmempool" "[\"$address\"]" | jq -r '.transactions | length')"
        if [ "$count" -ge 1 ]; then
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

assert_zero_balance() {
    local address="$1"
    local response
    response="$(rpc_result "getaddressbalance" "[\"$address\"]")"

    [ "$(echo "$response" | jq -r '.confirmed')" = "0" ] || fail "expected 0 confirmed before send"
    [ "$(echo "$response" | jq -r '.unconfirmed')" = "0" ] || fail "expected 0 unconfirmed before send"
    [ "$(echo "$response" | jq -r '.balance')" = "0" ] || fail "expected 0 balance before send"
    [ "$(echo "$response" | jq -r '.estimated_balance')" = "0" ] || fail "expected 0 estimated balance before send"
    [ "$(echo "$response" | jq -r '.spendable')" = "0" ] || fail "expected 0 spendable before send"
}

assert_source_pending_spend_overlay() {
    local address="$1"
    local confirmed_before="$2"
    local response
    response="$(rpc_result "getaddressbalance" "[\"$address\"]")"

    [ "$(echo "$response" | jq -r '.confirmed')" = "$confirmed_before" ] || fail "pending spend must not change confirmed source balance"
    [ "$(echo "$response" | jq -r '.balance')" = "$confirmed_before" ] || fail "source balance must stay confirmed-only while spend is pending"
    [ "$(echo "$response" | jq -r '.spendable')" = "$confirmed_before" ] || fail "source spendable must stay confirmed-only while spend is pending"
    [ "$(echo "$response" | jq -r '.unconfirmed < 0')" = "true" ] || fail "expected negative unconfirmed delta for pending source spend"
    [ "$(echo "$response" | jq -r '.estimated_balance < .balance')" = "true" ] || fail "expected estimated balance to drop below confirmed source balance"
    [ "$(echo "$response" | jq -r '.unconfirmed_din | startswith("-")')" = "true" ] || fail "negative unconfirmed delta must stay signed in DIN formatting"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["address_balance_overlay_wallet"]' >/dev/null 2>&1 || true
MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty mining address"

info "Mining 101 blocks to create mature spendable funds"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null
TIP_HEIGHT="$(rpc_scalar "getblockcount" "[]" '.')"
[ "$TIP_HEIGHT" -ge 101 ] || fail "expected height >= 101, got $TIP_HEIGHT"

RECIPIENT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$RECIPIENT_ADDR" ] || fail "wallet.getnewaddress returned empty recipient address"

assert_zero_balance "$RECIPIENT_ADDR"
pass "recipient starts at zero confirmed and zero estimated balance"

SOURCE_BEFORE="$(rpc_result "getaddressbalance" "[\"$MINER_ADDR\"]")"
SOURCE_CONFIRMED_BEFORE="$(echo "$SOURCE_BEFORE" | jq -r '.confirmed')"
[ "$SOURCE_CONFIRMED_BEFORE" -gt 0 ] || fail "expected mining address to have confirmed balance before spend"

info "Sending 1.25 DIN to recipient without mining a confirmation block"
SEND_RESULT="$(rpc_result "wallet.sendtoaddress" "[\"$RECIPIENT_ADDR\",1.25]")"
TXID="$(echo "$SEND_RESULT" | jq -r '.txid // empty')"
[ -n "$TXID" ] || fail "wallet.sendtoaddress did not return txid: $(echo "$SEND_RESULT" | jq -c '.')"

wait_for_mempool_address "$RECIPIENT_ADDR" || fail "recipient transaction never appeared in getaddressmempool"

MEMPOOL_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_ADDR\"]")"
[ "$(echo "$MEMPOOL_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "mempool receive must not change confirmed balance"
[ "$(echo "$MEMPOOL_BALANCE" | jq -r '.balance')" = "0" ] || fail "balance must remain confirmed-only while tx is pending"
[ "$(echo "$MEMPOOL_BALANCE" | jq -r '.spendable')" = "0" ] || fail "spendable must remain zero while tx is pending"
[ "$(echo "$MEMPOOL_BALANCE" | jq -r '.unconfirmed')" = "125000000" ] || fail "expected +125000000 unconfirmed delta"
[ "$(echo "$MEMPOOL_BALANCE" | jq -r '.estimated_balance')" = "125000000" ] || fail "expected estimated balance to include mempool delta"
[ "$(echo "$MEMPOOL_BALANCE" | jq -r '.unconfirmed_din')" = "1.25000000" ] || fail "expected signed DIN string for positive unconfirmed delta"

MEMPOOL_TXS="$(rpc_result "getaddressmempool" "[\"$RECIPIENT_ADDR\"]")"
[ "$(echo "$MEMPOOL_TXS" | jq -r '.transactions | length')" = "1" ] || fail "expected one pending tx for recipient"
[ "$(echo "$MEMPOOL_TXS" | jq -r '.transactions[0].txid // empty')" = "$TXID" ] || fail "mempool txid mismatch"
[ "$(echo "$MEMPOOL_TXS" | jq -r '.transactions[0].type // empty')" = "receive" ] || fail "recipient mempool entry should be receive"
pass "pending receive is exposed only via explicit mempool fields"

assert_source_pending_spend_overlay "$MINER_ADDR" "$SOURCE_CONFIRMED_BEFORE"
SOURCE_MEMPOOL_TXS="$(rpc_result "getaddressmempool" "[\"$MINER_ADDR\"]")"
SOURCE_PENDING_DELTA="$(rpc_result "getaddressbalance" "[\"$MINER_ADDR\"]" | jq -r 'if .unconfirmed < 0 then -(.unconfirmed) else .unconfirmed end')"
[ "$(echo "$SOURCE_MEMPOOL_TXS" | jq -r '.transactions | length')" = "1" ] || fail "expected one pending tx for source address"
[ "$(echo "$SOURCE_MEMPOOL_TXS" | jq -r '.transactions[0].txid // empty')" = "$TXID" ] || fail "source mempool txid mismatch"
[ "$(echo "$SOURCE_MEMPOOL_TXS" | jq -r '.transactions[0].type // empty')" = "send" ] || fail "source mempool entry should be send"
[ "$(echo "$SOURCE_MEMPOOL_TXS" | jq -r '.transactions[0].amount')" = "$SOURCE_PENDING_DELTA" ] || fail "source mempool entry should match the source-address mempool delta"
pass "pending spend keeps source confirmed balance separate from the mempool overlay"

info "Mining a confirmation block"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null

CONFIRMED_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_ADDR\"]")"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.confirmed')" = "125000000" ] || fail "confirmed balance should reflect mined receive"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "unconfirmed delta should clear after confirmation"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.balance')" = "125000000" ] || fail "balance should equal confirmed after mining"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.estimated_balance')" = "125000000" ] || fail "estimated balance should match confirmed after mining"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.spendable')" = "125000000" ] || fail "spendable should equal confirmed after mining"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.balance_din')" = "1.25000000" ] || fail "expected confirmed DIN string after mining"

POST_MEMPOOL="$(rpc_result "getaddressmempool" "[\"$RECIPIENT_ADDR\"]")"
[ "$(echo "$POST_MEMPOOL" | jq -r '.transactions | length')" = "0" ] || fail "recipient mempool overlay should clear after confirmation"
pass "confirmed address balance converges cleanly after mining"

echo -e "${GREEN}SUCCESS:${NC} address-index mempool overlay checks passed"
