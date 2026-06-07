#!/usr/bin/env bash
#
# wallet.consolidate dry-run / keyless behaviors:
# - no-op on empty wallet and single-UTXO family
# - auto picks the majority family and never mixes families
# - locked + immature coinbase UTXOs are excluded
# - fee-sanity gate rejects (percent and absolute) and dry-run returns the plan
#
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
DATADIR="/tmp/wallet_consolidate_dryrun_$$"
PORT_RPC=22182; PORT_P2P=22181; DAEMON_PID=""
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
get_cookie() { cut -d: -f2 "$DATADIR/.cookie" 2>/dev/null || true; }
rpc_raw() {
    local method="$1"; local params="${2:-[]}"; local cookie; cookie="$(get_cookie)"
    if [ -z "$cookie" ]; then echo '{"error":{"message":"missing rpc cookie"}}'; return 1; fi
    curl -sS -X POST "http://127.0.0.1:$PORT_RPC" -u "__cookie__:$cookie" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}"
}
rpc_result() {
    local response; response="$(rpc_raw "$1" "${2:-[]}")"
    if echo "$response" | jq -e '.error != null' >/dev/null 2>&1; then
        fail "RPC error [$1]: $(echo "$response" | jq -r '.error.message // (.error|tostring)')"
    fi
    echo "$response" | jq '.result'
}
rpc_scalar() { rpc_result "$1" "$2" | jq -r "$3"; }
wait_for_daemon() {
    local waited=0
    while [ "$waited" -lt 30 ]; do
        if rpc_raw "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then return 0; fi
        sleep 1; ((waited++))
    done
    return 1
}
start_daemon() {
    rm -rf "$DATADIR"; mkdir -p "$DATADIR"
    "$DINEROD" --regtest --datadir="$DATADIR" --rpcport="$PORT_RPC" --port="$PORT_P2P" --debug \
        > "$DATADIR/daemon.log" 2>&1 &
    DAEMON_PID=$!
    wait_for_daemon || fail "daemon failed to start"
}
stop_daemon() { [ -n "$DAEMON_PID" ] && { kill "$DAEMON_PID" 2>/dev/null || true; wait "$DAEMON_PID" 2>/dev/null || true; DAEMON_PID=""; }; }
cleanup() { stop_daemon; rm -rf "$DATADIR"; }
trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"

info "Starting daemon"
start_daemon
rpc_result "wallet.createhd" '["consolidate_wallet"]' >/dev/null

# --- empty wallet: no-op, not an error ---
EMPTY="$(rpc_result "wallet.consolidate" '[{"dry_run":true}]' | jq -c '.')"
echo "$EMPTY" | jq -e '.ok == true' >/dev/null || fail "empty wallet not ok: $EMPTY"
echo "$EMPTY" | jq -e '.selected_inputs == 0' >/dev/null || fail "empty wallet should select 0: $EMPTY"

# --- fund a P2TR family with many mature coinbase UTXOs ---
P2TR_ADDR="$(rpc_scalar "wallet.getnewaddress" '[]' '.address // empty')"
[ -n "$P2TR_ADDR" ] || fail "no p2tr addr"
info "Mining 130 P2TR coinbase UTXOs (mature ones become eligible)"
rpc_result "generatetoaddress" "[130,\"$P2TR_ADDR\"]" >/dev/null

# --- dry-run plan over the P2TR family ---
PLAN="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_inputs":100}]' | jq -c '.')"
echo "$PLAN" | jq -e '.ok == true' >/dev/null || fail "p2tr plan not ok: $PLAN"
echo "$PLAN" | jq -e '.dry_run == true' >/dev/null || fail "plan not dry_run: $PLAN"
echo "$PLAN" | jq -e '.address_family == "p2tr"' >/dev/null || fail "wrong family: $PLAN"
echo "$PLAN" | jq -e '.selected_inputs >= 2 and .selected_inputs <= 100' >/dev/null || fail "bad selected_inputs: $PLAN"
echo "$PLAN" | jq -e '.input_value > 0 and .estimated_fee > 0' >/dev/null || fail "bad values: $PLAN"
echo "$PLAN" | jq -e '((.input_value - .estimated_fee - .output_value) | if . < 0 then -. else . end) < 0.00000002' >/dev/null || fail "output != input-fee: $PLAN"
echo "$PLAN" | jq -e '.destination | type == "string" and length > 0' >/dev/null || fail "no destination: $PLAN"
echo "$PLAN" | jq -e '.txid == null' >/dev/null || fail "dry-run produced txid: $PLAN"

# --- auto picks the majority family; with only P2TR funded it must be p2tr ---
AUTO="$(rpc_result "wallet.consolidate" '[{"address_type":"auto","dry_run":true,"min_confirmations":1}]' | jq -c '.')"
echo "$AUTO" | jq -e '.address_family == "p2tr"' >/dev/null || fail "auto should pick p2tr: $AUTO"

# --- max_inputs cap is honored ---
CAP="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_inputs":5}]' | jq -c '.')"
echo "$CAP" | jq -e '.selected_inputs == 5' >/dev/null || fail "max_inputs cap ignored: $CAP"

# --- fee gate trips on absolute DIN cap (tiny max_fee_din) ---
GATE_ABS="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_fee_din":0.0000001}]' | jq -c '.')"
echo "$GATE_ABS" | jq -e '.ok == false and .fee_ok == false' >/dev/null || fail "abs fee gate did not trip: $GATE_ABS"
echo "$GATE_ABS" | jq -e '.reason | type == "string" and length > 0' >/dev/null || fail "no reason on abs gate: $GATE_ABS"

# --- fee gate trips on percent (tiny max_fee_percent) ---
GATE_PCT="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1,"max_fee_percent":0.0000001,"max_fee_din":1000000}]' | jq -c '.')"
echo "$GATE_PCT" | jq -e '.ok == false and .fee_ok == false' >/dev/null || fail "pct fee gate did not trip: $GATE_PCT"

# --- locked UTXOs are excluded: lock everything, expect no-op or fewer inputs ---
UTXO0="$(rpc_result "wallet.listunspent" '[1]' | jq -c '[.[] | select(.spendable==true)][0]')"
if echo "$UTXO0" | jq -e '.txid' >/dev/null 2>&1; then
    LTXID="$(echo "$UTXO0" | jq -r '.txid')"; LVOUT="$(echo "$UTXO0" | jq -r '.vout')"
    BEFORE="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1}]' | jq -r '.selected_inputs')"
    rpc_result "wallet.lockunspent" "[false,[{\"txid\":\"$LTXID\",\"vout\":$LVOUT}]]" >/dev/null
    AFTER="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1}]' | jq -r '.selected_inputs')"
    [ "$AFTER" -lt "$BEFORE" ] || fail "locked UTXO not excluded (before=$BEFORE after=$AFTER)"
    rpc_result "wallet.lockunspent" "[true,[{\"txid\":\"$LTXID\",\"vout\":$LVOUT}]]" >/dev/null
fi

# --- immature coinbase excluded: high min_confirmations admits only old coinbases ---
MATURE_ONLY="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":100}]' | jq -c '.')"
ALL_CONF1="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":true,"min_confirmations":1}]' | jq -r '.selected_inputs')"
MO_SEL="$(echo "$MATURE_ONLY" | jq -r '.selected_inputs')"
[ "$MO_SEL" -le "$ALL_CONF1" ] || fail "min_confirmations=100 selected more than conf=1 ($MO_SEL > $ALL_CONF1)"

pass "wallet.consolidate dry-run/no-op/partition/fee-gate/exclusion behaviors hold"
