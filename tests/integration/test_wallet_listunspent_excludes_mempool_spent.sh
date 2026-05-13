#!/usr/bin/env bash
#
# Regression: wallet.listunspent must stop returning a confirmed coin
# immediately after an unconfirmed mempool spend takes it. This mirrors the
# server-wallet raw-tx seam used by DineroDPI live artifact setup.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/wallet_listunspent_excludes_mempool_spent_$$"
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

calc_change_amount() {
    python3 - "$1" <<'PY'
from decimal import Decimal, ROUND_DOWN
import sys
amount = Decimal(sys.argv[1])
change = (amount - Decimal("1.00100000")).quantize(Decimal("0.00000001"), rounding=ROUND_DOWN)
if change <= 0:
    raise SystemExit("non-positive change amount")
print(format(change, "f"))
PY
}

create_sign_send_exact() {
    local input_txid="$1"
    local input_vout="$2"
    local input_amount="$3"
    local input_script="$4"
    local recipient="$5"
    local change_address="$6"
    local change_amount="$7"

    local raw signed complete txid
    raw="$(rpc_scalar "wallet.createrawtransaction" "[[{\"txid\":\"$input_txid\",\"vout\":$input_vout}],{\"$recipient\":1.0,\"$change_address\":$change_amount}]" '.hex // empty')"
    [ -n "$raw" ] || fail "wallet.createrawtransaction returned empty hex"

    signed="$(rpc_result "wallet.signrawtransaction" "[\"$raw\",[{\"txid\":\"$input_txid\",\"vout\":$input_vout,\"scriptPubKey\":\"$input_script\",\"amount\":$input_amount}]]")"
    complete="$(echo "$signed" | jq -r '.complete // false')"
    [ "$complete" = "true" ] || fail "wallet.signrawtransaction did not complete"

    txid="$(rpc_result "wallet.sendrawtransaction" "[\"$(echo "$signed" | jq -r '.hex')\"]" | jq -r '.txid // .result // . // empty')"
    [ -n "$txid" ] || fail "wallet.sendrawtransaction returned empty txid"
    echo "$txid"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["wallet_listunspent_excludes_mempool_spent"]' >/dev/null 2>&1 || true

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_ONE="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
RECIPIENT_TWO="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
CHANGE_ONE="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
CHANGE_TWO="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"

[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty miner address"
[ -n "$RECIPIENT_ONE" ] || fail "wallet.getnewaddress returned empty recipient one"
[ -n "$RECIPIENT_TWO" ] || fail "wallet.getnewaddress returned empty recipient two"
[ -n "$CHANGE_ONE" ] || fail "wallet.getnewaddress returned empty change one"
[ -n "$CHANGE_TWO" ] || fail "wallet.getnewaddress returned empty change two"

info "Mining 102 blocks to create at least two mature confirmed wallet UTXOs"
rpc_result "generatetoaddress" "[102,\"$MINER_ADDR\"]" >/dev/null

LIST_ONE="$(rpc_result "wallet.listunspent" "[101,9999999]")"
UTXO_ONE="$(echo "$LIST_ONE" | jq 'map(select(.spendable == true))[0]')"
UTXO_ONE_TXID="$(echo "$UTXO_ONE" | jq -r '.txid')"
UTXO_ONE_VOUT="$(echo "$UTXO_ONE" | jq -r '.vout')"
UTXO_ONE_AMOUNT="$(echo "$UTXO_ONE" | jq -r '.amount')"
UTXO_ONE_SCRIPT="$(echo "$UTXO_ONE" | jq -r '.scriptPubKey')"
[ -n "$UTXO_ONE_TXID" ] && [ "$UTXO_ONE_TXID" != "null" ] || fail "expected first spendable wallet UTXO"

CHANGE_ONE_AMOUNT="$(calc_change_amount "$UTXO_ONE_AMOUNT")"
info "Broadcasting first exact-input raw spend from $UTXO_ONE_TXID:$UTXO_ONE_VOUT"
TX1="$(create_sign_send_exact "$UTXO_ONE_TXID" "$UTXO_ONE_VOUT" "$UTXO_ONE_AMOUNT" "$UTXO_ONE_SCRIPT" "$RECIPIENT_ONE" "$CHANGE_ONE" "$CHANGE_ONE_AMOUNT")"
[ -n "$TX1" ] || fail "first raw spend did not return txid"
wait_for_mempool_size 1 || fail "mempool never reached size 1 after first raw spend"

LIST_TWO="$(rpc_result "wallet.listunspent" "[101,9999999]")"
MATCH_COUNT="$(echo "$LIST_TWO" | jq --arg txid "$UTXO_ONE_TXID" --argjson vout "$UTXO_ONE_VOUT" 'map(select(.txid == $txid and .vout == $vout)) | length')"
[ "$MATCH_COUNT" = "0" ] || fail "wallet.listunspent still returned mempool-spent outpoint $UTXO_ONE_TXID:$UTXO_ONE_VOUT"

UTXO_TWO="$(echo "$LIST_TWO" | jq 'map(select(.spendable == true))[0]')"
UTXO_TWO_TXID="$(echo "$UTXO_TWO" | jq -r '.txid')"
UTXO_TWO_VOUT="$(echo "$UTXO_TWO" | jq -r '.vout')"
UTXO_TWO_AMOUNT="$(echo "$UTXO_TWO" | jq -r '.amount')"
UTXO_TWO_SCRIPT="$(echo "$UTXO_TWO" | jq -r '.scriptPubKey')"
[ -n "$UTXO_TWO_TXID" ] && [ "$UTXO_TWO_TXID" != "null" ] || fail "expected a second spendable wallet UTXO"
[ "$UTXO_TWO_TXID:$UTXO_TWO_VOUT" != "$UTXO_ONE_TXID:$UTXO_ONE_VOUT" ] || fail "wallet.listunspent reused the same outpoint after a mempool spend"

CHANGE_TWO_AMOUNT="$(calc_change_amount "$UTXO_TWO_AMOUNT")"
info "Broadcasting second exact-input raw spend from $UTXO_TWO_TXID:$UTXO_TWO_VOUT"
TX2="$(create_sign_send_exact "$UTXO_TWO_TXID" "$UTXO_TWO_VOUT" "$UTXO_TWO_AMOUNT" "$UTXO_TWO_SCRIPT" "$RECIPIENT_TWO" "$CHANGE_TWO" "$CHANGE_TWO_AMOUNT")"
[ -n "$TX2" ] || fail "second raw spend did not return txid"
wait_for_mempool_size 2 || fail "mempool never reached size 2 after second raw spend"

pass "wallet.listunspent excluded the mempool-spent outpoint and the second exact-input spend succeeded"
