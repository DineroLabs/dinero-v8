#!/usr/bin/env bash
#
# Regression: wallet.createrawtransaction must accept explicit scriptPubKey
# outputs so higher layers can fund wallet-owned scripts without re-encoding the
# address on the daemon side.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/wallet_createrawtransaction_scriptpubkey_outputs_$$"
PORT_RPC=23152
PORT_P2P=23153
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

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["createrawtransaction_scriptpubkey_outputs"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
CHANGE_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty miner address"
[ -n "$CHANGE_ADDR" ] || fail "wallet.getnewaddress returned empty change address"
[ -n "$RECIPIENT_ADDR" ] || fail "wallet.getnewaddress returned empty recipient address"

RECIPIENT_SCRIPT="$(rpc_result "wallet.listaddresses" "[]" | jq -r --arg addr "$RECIPIENT_ADDR" '.[] | select(.address == $addr) | .scriptPubKey // empty' | head -n 1)"
[ -n "$RECIPIENT_SCRIPT" ] || fail "wallet.listaddresses did not expose recipient scriptPubKey"

info "Mining 101 blocks to create a mature spendable UTXO"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null

UTXO="$(rpc_result "wallet.listunspent" "[101,9999999]" | jq 'map(select(.spendable == true))[0]')"
UTXO_TXID="$(echo "$UTXO" | jq -r '.txid // empty')"
UTXO_VOUT="$(echo "$UTXO" | jq -r '.vout // empty')"
UTXO_AMOUNT="$(echo "$UTXO" | jq -r '.amount // empty')"
UTXO_SCRIPT="$(echo "$UTXO" | jq -r '.scriptPubKey // empty')"

[ -n "$UTXO_TXID" ] && [ "$UTXO_TXID" != "null" ] || fail "expected a spendable UTXO"
[ -n "$UTXO_SCRIPT" ] && [ "$UTXO_SCRIPT" != "null" ] || fail "expected scriptPubKey for spendable UTXO"

info "Creating raw transaction with explicit scriptPubKey output"
RAW_TX="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT}], [{\"scriptPubKey\":\"$RECIPIENT_SCRIPT\",\"amount\":1.0}, {\"$CHANGE_ADDR\":98.999}]]" '.hex // empty')"
[ -n "$RAW_TX" ] || fail "wallet.createrawtransaction returned empty hex"

DECODED_TX="$(rpc_result "wallet.decoderawtransaction" "[\"$RAW_TX\"]")"
[ "$(echo "$DECODED_TX" | jq -r '.vout | length')" = "2" ] || fail "expected two outputs in decoded raw transaction"
[ "$(echo "$DECODED_TX" | jq -r --arg spk "$RECIPIENT_SCRIPT" 'any(.vout[]; .scriptPubKey.hex == $spk and (.value > 0.99999999 and .value < 1.00000001))')" = "true" ] || fail "decoded raw transaction missing explicit scriptPubKey output"
pass "raw transaction preserves explicit scriptPubKey output before signing"

SIGNED_TX="$(rpc_result "wallet.signrawtransaction" "[\"$RAW_TX\",[{\"txid\":\"$UTXO_TXID\",\"vout\":$UTXO_VOUT,\"scriptPubKey\":\"$UTXO_SCRIPT\",\"amount\":$UTXO_AMOUNT}]]")"
[ "$(echo "$SIGNED_TX" | jq -r '.complete')" = "true" ] || fail "wallet.signrawtransaction should complete"

SIGNED_HEX="$(echo "$SIGNED_TX" | jq -r '.hex // empty')"
[ -n "$SIGNED_HEX" ] || fail "wallet.signrawtransaction returned empty hex"

TXID="$(rpc_scalar "wallet.sendrawtransaction" "[\"$SIGNED_HEX\"]" '.txid // .result // . // empty')"
[ -n "$TXID" ] || fail "wallet.sendrawtransaction did not return txid"

info "Mining confirmation block"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null

RECIPIENT_BALANCE="$(rpc_result "getaddressbalance" "[\"$RECIPIENT_ADDR\"]")"
[ "$(echo "$RECIPIENT_BALANCE" | jq -r '.confirmed')" = "100000000" ] || fail "recipient confirmed balance mismatch after mining"
[ "$(echo "$RECIPIENT_BALANCE" | jq -r '.spendable')" = "100000000" ] || fail "recipient spendable mismatch after mining"

RECIPIENT_HISTORY="$(rpc_result "getaddresshistory" "[\"$RECIPIENT_ADDR\"]")"
[ "$(echo "$RECIPIENT_HISTORY" | jq -r '.transactions | length')" = "1" ] || fail "expected one confirmed history entry for scriptPubKey-funded recipient"
[ "$(echo "$RECIPIENT_HISTORY" | jq -r '.transactions[0].txid // empty')" = "$TXID" ] || fail "confirmed history txid mismatch"

pass "scriptPubKey-funded output signs, broadcasts, and confirms correctly"
