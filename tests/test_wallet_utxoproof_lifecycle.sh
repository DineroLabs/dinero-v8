#!/usr/bin/env bash
#
# Wallet Utreexo proof lifecycle test:
# - wallet.utxoproof returns live proofs (no placeholder)
# - wallet.verifyutxoproof validates current proofs
# - stale proofs fail after reorg/invalidation
# - refreshed proofs validate again on new tip
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

DATADIR="/tmp/wallet_utxoproof_lifecycle_$$"
PORT_RPC=22120
PORT_P2P=22119
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

rpc_result "wallet.createhd" '["proof_wallet"]' >/dev/null 2>&1 || true
ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ADDR" ] || fail "wallet.getnewaddress returned empty address"
info "Mining address: $ADDR"

rpc_result "generatetoaddress" "[2,\"$ADDR\"]" >/dev/null
TIP_HASH="$(rpc_scalar "getbestblockhash" "[]" '.')"
COINBASE_TXID="$(rpc_result "getblock" "[\"$TIP_HASH\",1]" | jq -r '.tx[0] // empty')"
[ -n "$COINBASE_TXID" ] || fail "failed to read tip coinbase txid"
info "Tip coinbase: ${COINBASE_TXID:0:16}..."

PROOF_BUNDLE="$(rpc_result "wallet.utxoproof" "[\"${COINBASE_TXID}:0\"]" | jq -c '.')"
echo "$PROOF_BUNDLE" | jq -e '.error == null' >/dev/null || fail "wallet.utxoproof returned error"
echo "$PROOF_BUNDLE" | jq -e '.utreexo_proof.siblings | type == "array"' >/dev/null || fail "wallet.utxoproof missing siblings[]"
BOUND_ROOT="$(echo "$PROOF_BUNDLE" | jq -r '.utreexo_root // empty')"
BOUND_TIP="$(echo "$PROOF_BUNDLE" | jq -r '.tip_hash // empty')"
[ "${#BOUND_ROOT}" -eq 64 ] || fail "wallet.utxoproof missing/invalid utreexo_root binding"
[ "${#BOUND_TIP}" -eq 64 ] || fail "wallet.utxoproof missing/invalid tip_hash binding"
pass "wallet.utxoproof returns a live proof object"

VERIFY_LIVE="$(rpc_result "wallet.verifyutxoproof" "[$PROOF_BUNDLE]" | jq -c '.')"
[ "$(echo "$VERIFY_LIVE" | jq -r '.valid')" = "true" ] || fail "live proof did not verify: $VERIFY_LIVE"
pass "wallet.verifyutxoproof accepts current proof"

VERIFY_STRICT="$(rpc_result "wallet.verifyutxoproof" "[$PROOF_BUNDLE,{\"enforce_bound_context\":true}]" | jq -c '.')"
[ "$(echo "$VERIFY_STRICT" | jq -r '.valid')" = "true" ] || fail "strict context verification failed: $VERIFY_STRICT"
[ "$(echo "$VERIFY_STRICT" | jq -r '.context_enforced // false')" = "true" ] || fail "strict verification did not report context_enforced"
pass "wallet.verifyutxoproof enforces bundled context successfully"

if [ "${BOUND_ROOT:0:1}" = "0" ]; then
    BAD_ROOT="1${BOUND_ROOT:1}"
else
    BAD_ROOT="0${BOUND_ROOT:1}"
fi
VERIFY_BAD_ROOT="$(rpc_result "wallet.verifyutxoproof" "[$PROOF_BUNDLE,{\"expected_utreexo_root\":\"$BAD_ROOT\"}]" | jq -c '.')"
[ "$(echo "$VERIFY_BAD_ROOT" | jq -r '.valid')" = "false" ] || fail "expected bad root verification to fail: $VERIFY_BAD_ROOT"
[ "$(echo "$VERIFY_BAD_ROOT" | jq -r '.error_code // empty')" = "utreexo-root-mismatch" ] || fail "expected utreexo-root-mismatch, got: $VERIFY_BAD_ROOT"
pass "wallet.verifyutxoproof rejects mismatched expected_utreexo_root"

if [ "${BOUND_TIP:0:1}" = "0" ]; then
    BAD_TIP="1${BOUND_TIP:1}"
else
    BAD_TIP="0${BOUND_TIP:1}"
fi
VERIFY_BAD_TIP="$(rpc_result "wallet.verifyutxoproof" "[$PROOF_BUNDLE,{\"expected_tip_hash\":\"$BAD_TIP\"}]" | jq -c '.')"
[ "$(echo "$VERIFY_BAD_TIP" | jq -r '.valid')" = "false" ] || fail "expected bad tip verification to fail: $VERIFY_BAD_TIP"
[ "$(echo "$VERIFY_BAD_TIP" | jq -r '.error_code // empty')" = "tip-hash-mismatch" ] || fail "expected tip-hash-mismatch, got: $VERIFY_BAD_TIP"
pass "wallet.verifyutxoproof rejects mismatched expected_tip_hash"

INVALIDATE_RESULT="$(rpc_result "blockchain.invalidateblock" "[\"$TIP_HASH\"]" | jq -c '.')"
echo "$INVALIDATE_RESULT" | jq -e '.error == null' >/dev/null || fail "invalidateblock failed: $INVALIDATE_RESULT"
OLD_HEIGHT="$(echo "$INVALIDATE_RESULT" | jq -r '.old_height // -1')"
NEW_HEIGHT="$(echo "$INVALIDATE_RESULT" | jq -r '.new_height // -1')"
[ "$NEW_HEIGHT" -lt "$OLD_HEIGHT" ] || fail "invalidateblock did not lower chain height: $INVALIDATE_RESULT"
ALT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ALT_ADDR" ] || fail "failed to get alternate mining address after invalidate"
rpc_result "generatetoaddress" "[1,\"$ALT_ADDR\"]" >/dev/null
NEW_TIP="$(rpc_scalar "getbestblockhash" "[]" '.')"
[ "$NEW_TIP" != "$TIP_HASH" ] || fail "tip hash unchanged after invalidate+mine"

VERIFY_STALE="$(rpc_result "wallet.verifyutxoproof" "[$PROOF_BUNDLE]" | jq -c '.')"
[ "$(echo "$VERIFY_STALE" | jq -r '.valid')" = "false" ] || fail "stale proof unexpectedly valid after reorg: $VERIFY_STALE"
STALE_CODE="$(echo "$VERIFY_STALE" | jq -r '.error_code // empty')"
case "$STALE_CODE" in
    utxo-not-found|proof-invalid) ;;
    *) fail "expected stale-proof error_code utxo-not-found|proof-invalid, got: ${STALE_CODE:-<empty>}" ;;
esac
pass "stale proof is rejected after reorg"

NEW_COINBASE_TXID="$(rpc_result "getblock" "[\"$NEW_TIP\",1]" | jq -r '.tx[0] // empty')"
[ -n "$NEW_COINBASE_TXID" ] || fail "failed to read new tip coinbase txid"
NEW_BUNDLE="$(rpc_result "wallet.utxoproof" "[\"${NEW_COINBASE_TXID}:0\"]" | jq -c '.')"
echo "$NEW_BUNDLE" | jq -e '.error == null' >/dev/null || fail "wallet.utxoproof failed for new tip coinbase"
VERIFY_NEW="$(rpc_result "wallet.verifyutxoproof" "[$NEW_BUNDLE]" | jq -c '.')"
[ "$(echo "$VERIFY_NEW" | jq -r '.valid')" = "true" ] || fail "refreshed proof failed validation: $VERIFY_NEW"
pass "refreshed proof validates on new tip"

PROOF_OBJECT="$(echo "$NEW_BUNDLE" | jq -c '.utreexo_proof')"
VERIFY_DIRECT="$(rpc_result "wallet.verifyutxoproof" "[\"${NEW_COINBASE_TXID}:0\",$PROOF_OBJECT]" | jq -c '.')"
[ "$(echo "$VERIFY_DIRECT" | jq -r '.valid')" = "true" ] || fail "direct-proof form failed: $VERIFY_DIRECT"
pass "direct txid:vout + proof form validates"

echo -e "${GREEN}SUCCESS:${NC} wallet Utreexo proof lifecycle checks passed"
