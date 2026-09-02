#!/usr/bin/env bash
#
# Regression: mempool.testmempoolaccept must perform the canonical admission
# checks without inserting the transaction, consuming its input in the mempool
# coins view, changing counters, or preventing a later real submission.
#

set -euo pipefail

DATADIR="/tmp/mempool_test_accept_read_only_$$"
PORT_RPC="${PORT_RPC:-23166}"
PORT_P2P="${PORT_P2P:-23167}"
DAEMON_PID=""
ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DINEROD="${DINEROD:-$ROOT_DIR/build/dinerod}"

fail() {
    echo "FAIL: $*" >&2
    if [ -f "$DATADIR/daemon.log" ]; then
        tail -n 160 "$DATADIR/daemon.log" >&2 || true
    fi
    exit 1
}

cookie_password() {
    cut -d: -f2 "$DATADIR/.cookie" 2>/dev/null || true
}

rpc_raw() {
    local method="$1"
    local params="${2:-[]}"
    curl -sS -X POST "http://127.0.0.1:$PORT_RPC" \
        -u "__cookie__:$(cookie_password)" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}"
}

rpc_result() {
    local response
    response="$(rpc_raw "$1" "${2:-[]}")"
    echo "$response" | jq -e '.error == null' >/dev/null || \
        fail "RPC $1 failed: $(echo "$response" | jq -c '.error')"
    echo "$response" | jq '.result'
}

wait_for_daemon() {
    local attempt
    for attempt in $(seq 1 30); do
        if [ -s "$DATADIR/.cookie" ] && \
           rpc_raw "getblockcount" "[]" 2>/dev/null | jq -e '.error == null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

cleanup() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$DATADIR"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"

mkdir -p "$DATADIR"
"$DINEROD" --regtest --datadir="$DATADIR" --rpcport="$PORT_RPC" \
    --port="$PORT_P2P" --debug >>"$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
wait_for_daemon || fail "daemon failed to start"

rpc_result "wallet.createhd" '["mempool_test_accept_read_only"]' >/dev/null 2>&1 || true
MINER="$(rpc_result "wallet.getnewaddress" | jq -r '.address')"
RECIPIENT="$(rpc_result "wallet.getnewaddress" | jq -r '.address')"
CHANGE="$(rpc_result "wallet.getnewaddress" | jq -r '.address')"
rpc_result "generatetoaddress" "[101,\"$MINER\"]" >/dev/null

UTXO="$(rpc_result "wallet.listunspent" '[101,9999999]' | jq 'map(select(.spendable == true))[0]')"
TXID="$(echo "$UTXO" | jq -r '.txid')"
VOUT="$(echo "$UTXO" | jq -r '.vout')"
AMOUNT="$(echo "$UTXO" | jq -r '.amount')"
SCRIPT="$(echo "$UTXO" | jq -r '.scriptPubKey')"
[ "$TXID" != "null" ] || fail "no mature wallet UTXO"

CHANGE_AMOUNT="$(awk -v amount="$AMOUNT" 'BEGIN { printf "%.8f", amount - 1.00100000 }')"
RAW="$(rpc_result "wallet.createrawtransaction" "[[{\"txid\":\"$TXID\",\"vout\":$VOUT}],{\"$RECIPIENT\":1.0,\"$CHANGE\":$CHANGE_AMOUNT}]" | jq -r '.hex')"
SIGNED_RESULT="$(rpc_result "wallet.signrawtransaction" "[\"$RAW\",[{\"txid\":\"$TXID\",\"vout\":$VOUT,\"scriptPubKey\":\"$SCRIPT\",\"amount\":$AMOUNT}]]")"
[ "$(echo "$SIGNED_RESULT" | jq -r '.complete')" = "true" ] || fail "transaction signing incomplete"
SIGNED="$(echo "$SIGNED_RESULT" | jq -r '.hex')"

INITIAL_MEMPOOL="$(rpc_result "getrawmempool" | jq 'length')"
[ "$INITIAL_MEMPOOL" = "0" ] || fail "expected an empty initial mempool"

for attempt in 1 2; do
    ACCEPT="$(rpc_result "mempool.testmempoolaccept" "[\"$SIGNED\"]")"
    [ "$(echo "$ACCEPT" | jq -r '.[0].allowed')" = "true" ] || \
        fail "dry-run attempt $attempt rejected the valid transaction"
    [ "$(rpc_result "getrawmempool" | jq 'length')" = "0" ] || \
        fail "dry-run attempt $attempt mutated the mempool"
    [ "$(rpc_result "gettxout" "[\"$TXID\",$VOUT,true]" | jq -r 'type')" = "object" ] || \
        fail "dry-run attempt $attempt consumed the source UTXO"
done

BROADCAST_TXID="$(rpc_result "wallet.sendrawtransaction" "[\"$SIGNED\"]" | jq -r '.txid // .result // .')"
[ -n "$BROADCAST_TXID" ] && [ "$BROADCAST_TXID" != "null" ] || fail "real submission returned no txid"
[ "$(rpc_result "getrawmempool" | jq 'length')" = "1" ] || fail "real submission did not enter the mempool"

echo "PASS: testmempoolaccept stayed read-only and normal submission still succeeded"
