#!/usr/bin/env bash
#
# Smoke: P2MR wallet addresses must work with address-index RPCs.
#
# This proves the user-facing din1r... path after BuildScriptPubKey accepts
# witness-v3 P2MR addresses: derive a wallet P2MR address, send to it, observe
# the pending receive through getaddressbalance/getaddressmempool, mine it, and
# verify the confirmed balance converges.

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

RUN_ID=$$
DATADIR="/tmp/p2mr_address_index_rpc_smoke_${RUN_ID}"
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
PORT_RPC="${RPC_PORT:-$(alloc_port_base)}"
PORT_P2P="${P2P_PORT:-$((PORT_RPC + 1))}"
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"
WALLET_PASSWORD="p2mr-address-smoke-password"

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
    local waited=0
    while [ "$waited" -lt 30 ]; do
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
    local waited=0
    while [ "$waited" -lt 20 ]; do
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
    : > "$DATADIR/daemon.log"
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --listen=0 \
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
start_daemon

rpc_result "wallet.createhd" '["p2mr_address_index_wallet"]' >/dev/null
rpc_result "wallet.encrypt" "[\"$WALLET_PASSWORD\"]" >/dev/null
rpc_result "wallet.unlock" "[\"$WALLET_PASSWORD\",600]" >/dev/null
pass "wallet created, encrypted, and unlocked"

MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[\"taproot\",\"miner\"]" '.address // empty')"
[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty mining address"

info "Mining mature funds"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null

P2MR_ROW="$(rpc_result "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0,"leaf_index":0,"label":"p2mr-address-index-smoke"}')"
P2MR_ADDR="$(echo "$P2MR_ROW" | jq -r '.address // empty')"
P2MR_ROOT="$(echo "$P2MR_ROW" | jq -r '.merkle_root_hex // empty')"
[ -n "$P2MR_ADDR" ] || fail "wallet.getnewp2mraddress returned empty address"
[[ "$P2MR_ADDR" == din1r* ]] || fail "expected din1r P2MR address, got $P2MR_ADDR"
[[ "$P2MR_ROOT" =~ ^[0-9a-f]{64}$ ]] || fail "expected 32-byte merkle_root_hex, got $P2MR_ROOT"
pass "derived P2MR address ${P2MR_ADDR:0:18}..."

ZERO_BALANCE="$(rpc_result "getaddressbalance" "[\"$P2MR_ADDR\"]")"
[ "$(echo "$ZERO_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "expected zero confirmed before send"
[ "$(echo "$ZERO_BALANCE" | jq -r '.estimated_balance')" = "0" ] || fail "expected zero estimated before send"
[ "$(rpc_result "getaddressmempool" "[\"$P2MR_ADDR\"]" | jq -r '.transactions | length')" = "0" ] || fail "expected no P2MR mempool entries before send"
pass "P2MR address-index query starts empty"

info "Sending 2 DIN to P2MR address"
SEND_RESULT="$(rpc_result "wallet.sendtoaddress" "[\"$P2MR_ADDR\",2.0]")"
TXID="$(echo "$SEND_RESULT" | jq -r '.txid // empty')"
[ -n "$TXID" ] || fail "wallet.sendtoaddress did not return txid: $(echo "$SEND_RESULT" | jq -c '.')"

wait_for_mempool_address "$P2MR_ADDR" || fail "P2MR receive never appeared in getaddressmempool"

PENDING_BALANCE="$(rpc_result "getaddressbalance" "[\"$P2MR_ADDR\"]")"
[ "$(echo "$PENDING_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "pending P2MR receive must not change confirmed balance"
[ "$(echo "$PENDING_BALANCE" | jq -r '.balance')" = "0" ] || fail "P2MR balance must remain confirmed-only while tx is pending"
[ "$(echo "$PENDING_BALANCE" | jq -r '.unconfirmed')" = "200000000" ] || fail "expected +200000000 P2MR unconfirmed delta"
[ "$(echo "$PENDING_BALANCE" | jq -r '.estimated_balance')" = "200000000" ] || fail "expected estimated balance to include P2MR mempool delta"
[ "$(echo "$PENDING_BALANCE" | jq -r '.unconfirmed_din')" = "2.00000000" ] || fail "expected P2MR unconfirmed DIN string"

PENDING_MEMPOOL="$(rpc_result "getaddressmempool" "[\"$P2MR_ADDR\"]")"
[ "$(echo "$PENDING_MEMPOOL" | jq -r '.transactions | length')" = "1" ] || fail "expected one P2MR mempool entry"
[ "$(echo "$PENDING_MEMPOOL" | jq -r '.transactions[0].txid // empty')" = "$TXID" ] || fail "P2MR mempool txid mismatch"
[ "$(echo "$PENDING_MEMPOOL" | jq -r '.transactions[0].type // empty')" = "receive" ] || fail "P2MR mempool entry should be receive"
pass "pending P2MR receive is visible through address-index mempool RPCs"

info "Mining confirmation block"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null

CONFIRMED_BALANCE="$(rpc_result "getaddressbalance" "[\"$P2MR_ADDR\"]")"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.confirmed')" = "200000000" ] || fail "confirmed P2MR balance should reflect mined receive"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "P2MR unconfirmed delta should clear after confirmation"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.balance')" = "200000000" ] || fail "P2MR balance should equal confirmed after mining"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.estimated_balance')" = "200000000" ] || fail "P2MR estimated balance should match confirmed after mining"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.balance_din')" = "2.00000000" ] || fail "expected P2MR confirmed DIN string after mining"
[ "$(rpc_result "getaddressmempool" "[\"$P2MR_ADDR\"]" | jq -r '.transactions | length')" = "0" ] || fail "P2MR mempool overlay should clear after confirmation"
pass "confirmed P2MR address-index balance converges after mining"

# wallet.listaddresses backs the Receive address table. P2MR UTXOs are stored
# under their canonical 5320... scriptPubKey, so the per-address row must use
# that script lookup rather than looking up the human-readable din1r address.
LISTED_P2MR="$(rpc_result "wallet.listaddresses" | jq -c --arg address "$P2MR_ADDR" '[.[] | select(.address == $address)]')"
[ "$(echo "$LISTED_P2MR" | jq -r 'length')" = "1" ] || fail "wallet.listaddresses must contain exactly one derived P2MR row"
[ "$(echo "$LISTED_P2MR" | jq -r '.[0].type')" = "p2mr" ] || fail "wallet.listaddresses P2MR row has wrong type"
[[ "$(echo "$LISTED_P2MR" | jq -r '.[0].scriptPubKey // empty')" =~ ^5320[0-9a-f]{64}$ ]] || fail "wallet.listaddresses P2MR row must expose its canonical scriptPubKey"
LISTED_P2MR_BALANCE="$(echo "$LISTED_P2MR" | jq -r '.[0].balance')"
LISTED_P2MR_CONFIRMED="$(echo "$LISTED_P2MR" | jq -r '.[0].confirmed')"
LISTED_P2MR_SPENDABLE="$(echo "$LISTED_P2MR" | jq -r '.[0].spendable')"
echo "$LISTED_P2MR" | jq -e '.[0].balance == 2' >/dev/null || fail "wallet.listaddresses P2MR balance must equal 2 DIN (got $LISTED_P2MR_BALANCE; row=$LISTED_P2MR)"
echo "$LISTED_P2MR" | jq -e '.[0].confirmed == 2' >/dev/null || fail "wallet.listaddresses P2MR confirmed balance must equal 2 DIN (got $LISTED_P2MR_CONFIRMED)"
echo "$LISTED_P2MR" | jq -e '.[0].spendable == 2' >/dev/null || fail "wallet.listaddresses P2MR spendable balance must equal 2 DIN (got $LISTED_P2MR_SPENDABLE)"
[ "$(echo "$LISTED_P2MR" | jq -r '.[0].utxo_count')" = "1" ] || fail "wallet.listaddresses P2MR UTXO count must equal one"

pass "wallet.listaddresses reports the canonical P2MR script balance"

echo -e "${GREEN}SUCCESS:${NC} P2MR address-index RPC smoke checks passed"
