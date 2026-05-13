#!/usr/bin/env bash
#
# wallet.createfundedpsbt metadata smoke test:
# - returns fee/change metadata for linked hardware-wallet tracking
# - returns selected input outpoints and amounts
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/wallet_createfundedpsbt_rpc_$$"
PORT_RPC=22180
PORT_P2P=22179
DAEMON_PID=""

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
    rm -rf "$DATADIR"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"

info "Starting daemon"
start_daemon

rpc_result "wallet.createhd" '["psbt_meta_wallet"]' >/dev/null
FUNDING_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$FUNDING_ADDR" ] || fail "wallet.getnewaddress returned empty funding address"
RECIPIENT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$RECIPIENT_ADDR" ] || fail "wallet.getnewaddress returned empty recipient address"

info "Mining spendable funds"
rpc_result "generatetoaddress" "[101,\"$FUNDING_ADDR\"]" >/dev/null

PSBT_RESULT="$(rpc_result "wallet.createfundedpsbt" "[{\"$RECIPIENT_ADDR\":1.25},{\"fee_rate\":1.0}]" | jq -c '.')"
echo "$PSBT_RESULT" | jq -e '.psbt | type == "string" and length > 0' >/dev/null || fail "missing psbt: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.fee_paid_una > 0' >/dev/null || fail "missing fee_paid_una: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.selected_inputs | type == "array" and length >= 1' >/dev/null || fail "missing selected_inputs: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.selected_inputs[0].txid | type == "string" and length == 64' >/dev/null || fail "selected input txid missing: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.selected_inputs[0].vout | type == "number"' >/dev/null || fail "selected input vout missing: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.selected_inputs[0].amount_una > 0' >/dev/null || fail "selected input amount missing: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.change_position | type == "number"' >/dev/null || fail "missing change_position: $PSBT_RESULT"
echo "$PSBT_RESULT" | jq -e '.change_amount_una | type == "number"' >/dev/null || fail "missing change_amount_una: $PSBT_RESULT"
if echo "$PSBT_RESULT" | jq -e '.change_amount_una > 0' >/dev/null 2>&1; then
    echo "$PSBT_RESULT" | jq -e '.change_address | type == "string" and length > 0' >/dev/null || fail "missing change_address for nonzero change: $PSBT_RESULT"
fi

pass "wallet.createfundedpsbt returns tracking metadata for linked hardware-wallet sends"
