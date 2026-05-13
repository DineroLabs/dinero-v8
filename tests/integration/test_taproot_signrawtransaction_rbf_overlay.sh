#!/usr/bin/env bash
#
# Regression: wallet.signrawtransaction must sign taproot spends correctly,
# and a higher-fee raw replacement must advance the mempool/address overlay
# without leaving stale recipient state behind.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/taproot_signrawtransaction_rbf_overlay_$$"
PORT_RPC="${PORT_RPC:-23142}"
PORT_P2P="${PORT_P2P:-23143}"
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

extract_sendraw_txid() {
    jq -r '.txid // .result // . // empty'
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon with opt-in RBF enabled"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["taproot_signrawtransaction_rbf_overlay"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
CHANGE_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_ONE="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_TWO="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty miner address"
[ -n "$CHANGE_ADDR" ] || fail "wallet.getnewaddress returned empty change address"
[ -n "$RECIPIENT_ONE" ] || fail "wallet.getnewaddress returned empty recipient one address"
[ -n "$RECIPIENT_TWO" ] || fail "wallet.getnewaddress returned empty recipient two address"

info "Mining 101 blocks to create a mature taproot UTXO"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null
CANONICAL_BEFORE_RBF="$(capture_canonical_state_json)"

UTXO="$(rpc_result "wallet.listunspent" "[101,9999999]" | jq 'map(select(.spendable == true))[0]')"
UTXO_TXID="$(echo "$UTXO" | jq -r '.txid')"
UTXO_VOUT="$(echo "$UTXO" | jq -r '.vout')"
UTXO_AMOUNT="$(echo "$UTXO" | jq -r '.amount')"
UTXO_SCRIPT="$(echo "$UTXO" | jq -r '.scriptPubKey')"
[ -n "$UTXO_TXID" ] && [ "$UTXO_TXID" != "null" ] || fail "expected a spendable UTXO"
[ "$UTXO_SCRIPT" != "null" ] || fail "expected scriptPubKey for spendable UTXO"

assert_zero_address_overlay "$RECIPIENT_ONE"
assert_zero_address_overlay "$RECIPIENT_TWO"

info "Creating and signing the first raw taproot spend"
RAW_ONE="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"sequence\":4294967293}],{\"$RECIPIENT_ONE\":1.0,\"$CHANGE_ADDR\":98.999}]" '.hex // empty')"
[ -n "$RAW_ONE" ] || fail "wallet.createrawtransaction returned empty hex for tx1"

SIGNED_ONE="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_ONE\",[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"scriptPubKey\":\"$UTXO_SCRIPT\",\"amount\":$UTXO_AMOUNT}]]")"
[ "$(echo "$SIGNED_ONE" | jq -r '.complete')" = "true" ] || fail "taproot tx1 should sign completely"
HEX_ONE="$(echo "$SIGNED_ONE" | jq -r '.hex // empty')"
[ -n "$HEX_ONE" ] || fail "wallet.signrawtransaction returned empty hex for tx1"

DECODED_ONE="$(rpc_result "wallet.decoderawtransaction" "[\"$HEX_ONE\"]")"
[ "$(echo "$DECODED_ONE" | jq -r '.vin[0].txinwitness | length')" = "1" ] || fail "taproot tx1 should carry a witness signature"

TX_ONE_RESP="$(rpc_result "wallet.sendrawtransaction" "[\"$HEX_ONE\"]")"
TX_ONE="$(echo "$TX_ONE_RESP" | extract_sendraw_txid)"
[ -n "$TX_ONE" ] || fail "wallet.sendrawtransaction returned empty txid for tx1"

wait_for_mempool_size 1 || fail "mempool never reached size 1 after tx1"
wait_for_address_mempool_count "$RECIPIENT_ONE" 1 || fail "recipient one overlay never observed tx1"
assert_same_canonical_state "$CANONICAL_BEFORE_RBF" "$(capture_canonical_state_json)" \
    "initial taproot mempool spend"

RECIPIENT_ONE_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_ONE\"]")"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "recipient one confirmed must stay zero while pending"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.balance')" = "0" ] || fail "recipient one balance must stay confirmed-only while pending"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.spendable')" = "0" ] || fail "recipient one spendable must stay zero while pending"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.unconfirmed')" = "100000000" ] || fail "recipient one unconfirmed delta mismatch"
[ "$(echo "$RECIPIENT_ONE_BALANCE" | jq -r '.estimated_balance')" = "100000000" ] || fail "recipient one estimated balance mismatch"

info "Creating and signing a higher-fee raw taproot replacement"
RAW_TWO="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"sequence\":4294967293}],{\"$RECIPIENT_TWO\":0.999,\"$CHANGE_ADDR\":98.9999}]" '.hex // empty')"
[ -n "$RAW_TWO" ] || fail "wallet.createrawtransaction returned empty hex for tx2"

SIGNED_TWO="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_TWO\",[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"scriptPubKey\":\"$UTXO_SCRIPT\",\"amount\":$UTXO_AMOUNT}]]")"
[ "$(echo "$SIGNED_TWO" | jq -r '.complete')" = "true" ] || fail "taproot tx2 should sign completely"
HEX_TWO="$(echo "$SIGNED_TWO" | jq -r '.hex // empty')"
[ -n "$HEX_TWO" ] || fail "wallet.signrawtransaction returned empty hex for tx2"

DECODED_TWO="$(rpc_result "wallet.decoderawtransaction" "[\"$HEX_TWO\"]")"
[ "$(echo "$DECODED_TWO" | jq -r '.vin[0].txinwitness | length')" = "1" ] || fail "taproot tx2 should carry a witness signature"

TX_TWO_RESP="$(rpc_result "wallet.sendrawtransaction" "[\"$HEX_TWO\"]")"
TX_TWO="$(echo "$TX_TWO_RESP" | extract_sendraw_txid)"
[ -n "$TX_TWO" ] || fail "wallet.sendrawtransaction returned empty txid for tx2"
[ "$TX_TWO" != "$TX_ONE" ] || fail "replacement txid must differ from original txid"

wait_for_mempool_size 1 || fail "mempool never converged back to size 1 after replacement"
wait_for_address_mempool_count "$RECIPIENT_ONE" 0 || fail "recipient one overlay did not clear after replacement"
wait_for_address_mempool_count "$RECIPIENT_TWO" 1 || fail "recipient two overlay never observed replacement"
assert_same_canonical_state "$CANONICAL_BEFORE_RBF" "$(capture_canonical_state_json)" \
    "taproot RBF replacement"

MEMPOOL_TXS="$(rpc_result "getrawmempool" "[]")"
[ "$(echo "$MEMPOOL_TXS" | jq -r 'length')" = "1" ] || fail "expected a single mempool tx after replacement"
[ "$(echo "$MEMPOOL_TXS" | jq -r '.[0]')" = "$TX_TWO" ] || fail "replacement tx should be the only mempool entry"

assert_zero_address_overlay "$RECIPIENT_ONE"

RECIPIENT_TWO_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_TWO\"]")"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "recipient two confirmed must stay zero while pending"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.balance')" = "0" ] || fail "recipient two balance must stay confirmed-only while pending"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.spendable')" = "0" ] || fail "recipient two spendable must stay zero while pending"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.unconfirmed')" = "99900000" ] || fail "recipient two unconfirmed delta mismatch"
[ "$(echo "$RECIPIENT_TWO_BALANCE" | jq -r '.estimated_balance')" = "99900000" ] || fail "recipient two estimated balance mismatch"

info "Clearing mempool to prove overlay rollback"
rpc_result "mempool.clear" "[]" >/dev/null
wait_for_mempool_size 0 || fail "mempool.clear did not empty the mempool"
wait_for_address_mempool_count "$RECIPIENT_TWO" 0 || fail "recipient two overlay did not clear after mempool.clear"
assert_same_canonical_state "$CANONICAL_BEFORE_RBF" "$(capture_canonical_state_json)" \
    "taproot mempool clear"

assert_zero_address_overlay "$RECIPIENT_ONE"
assert_zero_address_overlay "$RECIPIENT_TWO"

pass "wallet.signrawtransaction signs taproot spends and RBF replacement updates the overlay cleanly"
