#!/usr/bin/env bash
#
# Stateless wallet validation pilot test
#
# Validates that wallet.validatestatelessbalance:
# - Generates + verifies Utreexo proofs for wallet UTXOs
# - Reports consistent proof-backed balances
# - Supports bounded scans via max_utxos
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/wallet_stateless_validation_pilot_$$"
PORT_RPC=$((22300 + ($$ % 700)))
PORT_P2P=$((PORT_RPC - 1))
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DINEROD="${DINEROD:-$PROJECT_ROOT/build/dinerod}"

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }
fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [[ -f "$DATADIR/daemon.log" ]]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 120 "$DATADIR/daemon.log" >&2 || true
    fi
    exit 1
}

rpc_raw() {
    local method="$1"
    local params="${2:-[]}"
    local cookie
    cookie="$(cat "$DATADIR/.cookie" 2>/dev/null || true)"
    if [[ -z "$cookie" ]]; then
        echo '{"jsonrpc":"2.0","error":{"code":-1,"message":"missing rpc cookie"},"id":1}'
        return 1
    fi

    curl -sS --connect-timeout 2 --max-time 10 \
        -u "$cookie" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}" \
        "http://127.0.0.1:${PORT_RPC}"
}

rpc_result() {
    local method="$1"
    local params="${2:-[]}"
    local response
    response="$(rpc_raw "$method" "$params")"
    if echo "$response" | jq -e '.error != null' >/dev/null 2>&1; then
        local msg
        msg="$(echo "$response" | jq -r '.error.message // (.error|tostring)')"
        fail "RPC error [$method]: $msg"
    fi
    echo "$response" | jq '.result'
}

wait_for_daemon() {
    local waited=0
    local max_wait=40
    while (( waited < max_wait )); do
        if rpc_raw "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
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
        --listen=0 \
        >"$DATADIR/daemon.log" 2>&1 &
    DAEMON_PID=$!

    wait_for_daemon || fail "daemon failed to start"
}

stop_daemon() {
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
}

cleanup() {
    stop_daemon
    if [[ "$KEEP_DATADIR" == "1" ]]; then
        info "KEEP_DATADIR=1 preserving $DATADIR"
    else
        rm -rf "$DATADIR"
    fi
}
trap cleanup EXIT

[[ -x "$DINEROD" ]] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"

info "Starting daemon"
start_daemon

rpc_result "wallet.createhd" '["stateless_pilot_wallet"]' >/dev/null 2>&1 || true
ADDR="$(rpc_result "wallet.getnewaddress" "[]" | jq -r '.address // empty')"
[[ -n "$ADDR" ]] || fail "wallet.getnewaddress returned empty address"
info "Mining address: $ADDR"

# Mine enough blocks for mature spendable UTXOs (coinbase maturity = 100).
rpc_result "generatetoaddress" "[105, \"$ADDR\"]" >/dev/null
HEIGHT="$(rpc_result "getblockcount" "[]" | jq -r '.')"
info "Chain height after mining: $HEIGHT"

PILOT="$(rpc_result "wallet.validatestatelessbalance" "[]" | jq -c '.')"
echo "$PILOT" | jq -e '.pilot_pass == true' >/dev/null || fail "pilot_pass is false: $PILOT"
echo "$PILOT" | jq -e '.proofs.verified_invalid == 0' >/dev/null || fail "invalid proofs detected: $PILOT"
echo "$PILOT" | jq -e '.proofs.generation_failed == 0' >/dev/null || fail "proof generation failures detected: $PILOT"
echo "$PILOT" | jq -e '.summary.selected_utxos > 0' >/dev/null || fail "expected selected_utxos > 0: $PILOT"
echo "$PILOT" | jq -e '.summary.full_wallet_scope == true' >/dev/null || fail "expected full wallet scope validation: $PILOT"
pass "Default stateless wallet validation pilot passed"

PILOT_BOUNDED="$(rpc_result "wallet.validatestatelessbalance" '[{"max_utxos":2,"include_details":true}]' | jq -c '.')"
echo "$PILOT_BOUNDED" | jq -e '.summary.selected_utxos <= 2' >/dev/null || fail "max_utxos bound not respected: $PILOT_BOUNDED"
echo "$PILOT_BOUNDED" | jq -e '.pilot_pass == true' >/dev/null || fail "bounded pilot_pass is false: $PILOT_BOUNDED"
pass "Bounded stateless wallet validation pilot passed"

echo -e "${GREEN}SUCCESS:${NC} wallet stateless validation pilot checks passed"
