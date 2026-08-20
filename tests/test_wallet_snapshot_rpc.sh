#!/usr/bin/env bash
#
# wallet.snapshot smoke test:
# - returns a stable aggregate object
# - includes wallet identity, balances, sync, funded addresses, history, proof context
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

DATADIR="/tmp/wallet_snapshot_rpc_$$"
PORT_RPC=22140
PORT_P2P=22139
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

BUILD_DIR="$(dirname "$0")/../build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }

fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [ -f "$DATADIR/daemon.log" ]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 80 "$DATADIR/daemon.log" >&2 || true
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
        ((waited++))
    done
    return 1
}

wait_for_wallet_scan() {
    local target_height="$1"
    local max_wait=30
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        local snapshot
        snapshot="$(rpc_result "wallet.snapshot" '[{"history_count":10,"funded_address_limit":5}]')"
        if echo "$snapshot" | jq -e --argjson target "$target_height" \
            '.sync.wallet_scan_height >= $target' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

start_daemon() {
    rm -rf "$DATADIR"
    mkdir -p "$DATADIR"
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --debug \
        > "$DATADIR/daemon.log" 2>&1 &
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

info "Starting daemon"
start_daemon

rpc_result "wallet.createhd" '["snapshot_wallet"]' >/dev/null
ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ADDR" ] || fail "wallet.getnewaddress returned empty address"
info "Mining address: $ADDR"

rpc_result "generatetoaddress" "[2,\"$ADDR\"]" >/dev/null
TIP_HEIGHT="$(rpc_scalar "getblockcount" "[]" '.')"
wait_for_wallet_scan "$TIP_HEIGHT" || fail "wallet worker did not scan through height $TIP_HEIGHT"

SNAPSHOT="$(rpc_result "wallet.snapshot" '[{"history_count":10,"funded_address_limit":5}]' | jq -c '.')"
echo "$SNAPSHOT" | jq -e '.rpc_schema == "din.wallet.snapshot.v1"' >/dev/null || fail "unexpected rpc_schema: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.wallet.loaded == true' >/dev/null || fail "wallet not marked loaded: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.wallet.name == "snapshot_wallet"' >/dev/null || fail "wallet name missing from snapshot: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.balances.total > 0' >/dev/null || fail "snapshot missing wallet balance: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.receive.current_address | type == "string" and length > 0' >/dev/null || fail "snapshot missing current receive address: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.receive.funded_address_count >= 1' >/dev/null || fail "snapshot missing funded addresses: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.addresses.funded | type == "array" and length >= 1' >/dev/null || fail "snapshot funded address array missing: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.history.items | type == "array"' >/dev/null || fail "snapshot history missing: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.history.items | map(select(.type == "mined")) | length >= 1' >/dev/null || fail "snapshot history missing typed mined rows: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.proof_context.tip_hash | type == "string" and length == 64' >/dev/null || fail "snapshot missing tip_hash: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.proof_context.utreexo_root | type == "string" and length == 64' >/dev/null || fail "snapshot missing utreexo_root: $SNAPSHOT"
echo "$SNAPSHOT" | jq -e '.sync.chain_height >= 2' >/dev/null || fail "snapshot sync height too low: $SNAPSHOT"

SCOPED_SNAPSHOT="$(rpc_result "wallet.snapshot" "{\"addresses\":[{\"address\":\"$ADDR\",\"path\":\"m/86'/383'/0'/0/0\",\"account\":0,\"change\":0,\"index\":0}],\"current_address\":\"$ADDR\",\"history_count\":10,\"funded_address_limit\":5}" | jq -c '.')"
echo "$SCOPED_SNAPSHOT" | jq -e '.rpc_schema == "din.wallet.snapshot.v1"' >/dev/null || fail "unexpected scoped rpc_schema: $SCOPED_SNAPSHOT"
echo "$SCOPED_SNAPSHOT" | jq -e '.wallet.scope == "address_index"' >/dev/null || fail "scoped snapshot did not switch scope: $SCOPED_SNAPSHOT"
echo "$SCOPED_SNAPSHOT" | jq -e --arg address "$ADDR" '.receive.current_address == $address' >/dev/null || fail "scoped snapshot lost current address: $SCOPED_SNAPSHOT"
echo "$SCOPED_SNAPSHOT" | jq -e '.receive.known_address_count == 1' >/dev/null || fail "scoped snapshot missing known address count: $SCOPED_SNAPSHOT"
echo "$SCOPED_SNAPSHOT" | jq -e '.balances.confirmed_una > 0' >/dev/null || fail "scoped snapshot missing confirmed balance: $SCOPED_SNAPSHOT"
echo "$SCOPED_SNAPSHOT" | jq -e --arg address "$ADDR" '.addresses.funded | (type == "array") and any(.address == $address)' >/dev/null || fail "scoped snapshot missing funded address entry: $SCOPED_SNAPSHOT"
echo "$SCOPED_SNAPSHOT" | jq -e --arg address "$ADDR" '.history.items | map(select(.address == $address and .type == "mined")) | length >= 1' >/dev/null || fail "scoped snapshot missing mined history rows: $SCOPED_SNAPSHOT"

pass "wallet.snapshot returns wallet and address-scoped aggregate state"
