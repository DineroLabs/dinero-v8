#!/usr/bin/env bash
#
# wallet.consolidate execution path:
# - dry_run:false, broadcast:false → returns signed rawtx, no txid (not broadcast)
# - dry_run:false, broadcast:true  → returns a confirmable txid
#
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; NC='\033[0m'
DATADIR="/tmp/wallet_consolidate_broadcast_$$"
PORT_RPC=22184; PORT_P2P=22183; DAEMON_PID=""
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
rpc_result "wallet.createhd" '["consolidate_bcast_wallet"]' >/dev/null
P2TR_ADDR="$(rpc_scalar "wallet.getnewaddress" '[]' '.address // empty')"
[ -n "$P2TR_ADDR" ] || fail "no p2tr addr"

info "Mining 130 P2TR coinbase UTXOs"
rpc_result "generatetoaddress" "[130,\"$P2TR_ADDR\"]" >/dev/null

# --- execute WITHOUT broadcast: returns signed rawtx, nothing enters mempool ---
SIGN="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":false,"broadcast":false,"min_confirmations":1,"max_inputs":50}]' | jq -c '.')"
echo "$SIGN" | jq -e '.ok == true and .dry_run == false' >/dev/null || fail "sign-only not ok: $SIGN"
echo "$SIGN" | jq -e '.txid == null' >/dev/null || fail "sign-only should not broadcast (txid must be null): $SIGN"
echo "$SIGN" | jq -e '.rawtx | type == "string" and (length > 20)' >/dev/null || fail "sign-only missing rawtx: $SIGN"

# --- execute WITH broadcast: returns a txid that confirms ---
BCAST="$(rpc_result "wallet.consolidate" '[{"address_type":"p2tr","dry_run":false,"broadcast":true,"min_confirmations":1,"max_inputs":50}]' | jq -c '.')"
echo "$BCAST" | jq -e '.ok == true' >/dev/null || fail "broadcast not ok: $BCAST"
echo "$BCAST" | jq -e '.broadcast == true' >/dev/null || fail "broadcast flag not set: $BCAST"
echo "$BCAST" | jq -e '.txid | type == "string" and length == 64' >/dev/null || fail "broadcast missing txid: $BCAST"
echo "$BCAST" | jq -e '.selected_inputs >= 2' >/dev/null || fail "broadcast selected too few: $BCAST"
echo "$BCAST" | jq -e '.fee > 0 and .output_value > 0' >/dev/null || fail "broadcast bad fee/output: $BCAST"

TXID="$(echo "$BCAST" | jq -r '.txid')"
info "Mining 1 block to confirm $TXID"
CONFIRM_ADDR="$(rpc_scalar "wallet.getnewaddress" '[]' '.address // empty')"
rpc_result "generatetoaddress" "[1,\"$CONFIRM_ADDR\"]" >/dev/null
GTX="$(rpc_result "blockchain.gettransaction" "[\"$TXID\"]" | jq -c '.')"
echo "$GTX" | jq -e '.' >/dev/null || fail "consolidation tx not found after mining: $TXID"

pass "wallet.consolidate signs without broadcast and broadcasts a confirmable tx"
