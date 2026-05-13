#!/usr/bin/env bash
#
# Regression: mirror the DineroDPI lightweight-client startup path.
#  1. Sync headers / fetch compact filters
#  2. Fetch root-bound proof bundle
#  3. Cross-check bundle.accumulator_root against header.utreexo_root at bundle.height
#  4. Restart on the same datadir and ensure filters / proof bundle survive restore
#  5. Force a reorg and ensure stale root is detected, then refreshed bundle matches headers again
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/dpi_header_filter_proof_flow_$$"
PORT_RPC=23120
PORT_P2P=23119
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

BUILD_DIR="$(cd "$(dirname "$0")/../.." && pwd)/build"
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

assert_bundle_matches_headers() {
    local bundle_json="$1"
    local label="$2"

    local bundle_root bundle_hash bundle_height utxo_count local_hash raw_block_hex local_root header_json block_json header_root_raw block_root_raw
    local stump_leaves stump_roots proof_script_count
    bundle_root="$(echo "$bundle_json" | jq -r '.accumulator_root // empty')"
    bundle_hash="$(echo "$bundle_json" | jq -r '.block_hash // empty')"
    bundle_height="$(echo "$bundle_json" | jq -r '.height // -1')"
    utxo_count="$(echo "$bundle_json" | jq -r '.utxo_count // 0')"
    stump_leaves="$(echo "$bundle_json" | jq -r '(.stump_num_leaves // .num_leaves // 0)')"
    stump_roots="$(echo "$bundle_json" | jq -r '(.stump_roots // .roots // []) | length')"
    proof_script_count="$(echo "$bundle_json" | jq -r '[.proofs[]? | select(.success == true and ((.script_pubkey // "") | length > 0))] | length')"

    [ "${#bundle_root}" -eq 64 ] || fail "$label: missing/invalid accumulator_root"
    [ "${#bundle_hash}" -eq 64 ] || fail "$label: missing/invalid block_hash"
    [ "$bundle_height" -ge 1 ] || fail "$label: invalid height"
    [ "$utxo_count" -ge 1 ] || fail "$label: expected at least one proven UTXO"
    [ "$stump_leaves" -ge 1 ] || fail "$label: missing stump_num_leaves"
    [ "$stump_roots" -ge 1 ] || fail "$label: missing stump_roots"
    [ "$proof_script_count" -ge 1 ] || fail "$label: proofs missing script_pubkey"

    local_hash="$(rpc_scalar "getblockhash" "[$bundle_height]" '.')"
    [ "$local_hash" = "$bundle_hash" ] || fail "$label: bundle block_hash does not match header chain at height $bundle_height"

    header_json="$(rpc_result "getblockheader" "[\"$local_hash\"]")"
    header_root_raw="$(echo "$header_json" | jq -r '.utreexo_root_raw // empty')"
    [ "${#header_root_raw}" -eq 64 ] || fail "$label: getblockheader missing utreexo_root_raw"

    block_json="$(rpc_result "getblock" "[\"$local_hash\",1]")"
    block_root_raw="$(echo "$block_json" | jq -r '.utreexocommitment_raw // empty')"
    [ "${#block_root_raw}" -eq 64 ] || fail "$label: getblock missing utreexocommitment_raw"

    # DineroDPI verifies against raw header bytes, not display-order JSON hex.
    raw_block_hex="$(rpc_scalar "getblock" "[\"$local_hash\",0]" '.')"
    [ "${#raw_block_hex}" -ge 200 ] || fail "$label: getblock raw payload too short"
    local_root="${raw_block_hex:136:64}"
    [ "${#local_root}" -eq 64 ] || fail "$label: failed to extract raw utreexo_root bytes"
    [ "$header_root_raw" = "$local_root" ] || fail "$label: getblockheader utreexo_root_raw does not match raw header bytes"
    [ "$block_root_raw" = "$local_root" ] || fail "$label: getblock utreexocommitment_raw does not match raw header bytes"
    [ "$local_root" = "$bundle_root" ] || fail "$label: bundle root does not match header-chain root at height $bundle_height"

    pass "$label: proof bundle root matches header chain at height $bundle_height"
}

validate_filter_batch() {
    local filters_json="$1"
    local checked=0
    local filter_count
    filter_count="$(echo "$filters_json" | jq -r '.count // 0')"
    [ "$filter_count" -ge 1 ] || fail "blockchain.getblockfilters returned no filters"

    while IFS= read -r row; do
        local height block_hash filter_hash coinbase_tx coinbase_txid tx_count header_hash
        height="$(echo "$row" | jq -r '.height')"
        block_hash="$(echo "$row" | jq -r '.block_hash // empty')"
        filter_hash="$(echo "$row" | jq -r '.filter_hash // empty')"
        coinbase_tx="$(echo "$row" | jq -r '.coinbase_tx // empty')"
        coinbase_txid="$(echo "$row" | jq -r '.coinbase_txid // empty')"
        tx_count="$(echo "$row" | jq -r '.tx_count // 0')"

        [ "${#block_hash}" -eq 64 ] || fail "filter row height $height missing block_hash"
        [ "${#filter_hash}" -eq 64 ] || fail "filter row height $height missing filter_hash"
        [ -n "$coinbase_tx" ] || fail "filter row height $height missing coinbase_tx"
        [ "${#coinbase_txid}" -eq 64 ] || fail "filter row height $height missing coinbase_txid"
        [ "$tx_count" -ge 1 ] || fail "filter row height $height invalid tx_count"

        header_hash="$(rpc_scalar "getblockhash" "[$height]" '.')"
        [ "$header_hash" = "$block_hash" ] || fail "filter row height $height block_hash/header mismatch"

        checked=$((checked + 1))
    done < <(echo "$filters_json" | jq -c '.filters[]')

    [ "$checked" -ge 1 ] || fail "no filter rows validated"
    pass "block filter batch matches local header chain ($checked rows)"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["dpi_flow_wallet"]' >/dev/null 2>&1 || true
ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ADDR" ] || fail "wallet.getnewaddress returned empty address"
info "Mining address: $ADDR"

info "Mining 110 blocks to create mature spendable wallet UTXOs"
rpc_result "generatetoaddress" "[110,\"$ADDR\"]" >/dev/null
TIP_HEIGHT="$(rpc_scalar "getblockcount" "[]" '.')"
[ "$TIP_HEIGHT" -ge 110 ] || fail "expected height >= 110, got $TIP_HEIGHT"

FROM_HEIGHT=$((TIP_HEIGHT - 9))
if [ "$FROM_HEIGHT" -lt 0 ]; then
    FROM_HEIGHT=0
fi

FILTERS="$(rpc_result "blockchain.getblockfilters" "[$FROM_HEIGHT,10]")"
validate_filter_batch "$FILTERS"

INITIAL_BUNDLE="$(rpc_result "wallet.getproofbundle" "[{\"min_confirmations\":1,\"spendable_only\":true,\"max_utxos\":64}]")"
assert_bundle_matches_headers "$INITIAL_BUNDLE" "initial bundle"

FIRST_PROOF_ENVELOPE="$(echo "$INITIAL_BUNDLE" | jq -c '
    .proofs[] | select(.success == true) | {
        txid: .txid,
        vout: .vout,
        utreexo_root: $root,
        tip_hash: $tip,
        utreexo_proof: {
            siblings: .siblings,
            position: .position,
            num_leaves: .num_leaves
        }
    }' --arg root "$(echo "$INITIAL_BUNDLE" | jq -r '.accumulator_root')" --arg tip "$(echo "$INITIAL_BUNDLE" | jq -r '.block_hash')" | head -n 1)"
[ -n "$FIRST_PROOF_ENVELOPE" ] || fail "failed to build proof envelope from initial bundle"

VERIFY_RESULT="$(rpc_result "wallet.verifyutxoproof" "[$FIRST_PROOF_ENVELOPE,{\"enforce_bound_context\":true}]")"
[ "$(echo "$VERIFY_RESULT" | jq -r '.valid')" = "true" ] || fail "bound-context verification failed for initial bundle proof"
pass "initial proof bundle verifies under bound context"

OLD_ROOT="$(echo "$INITIAL_BUNDLE" | jq -r '.accumulator_root')"
OLD_TIP="$(echo "$INITIAL_BUNDLE" | jq -r '.block_hash')"
OLD_HEIGHT="$(echo "$INITIAL_BUNDLE" | jq -r '.height')"
OLD_FILTER_HASH="$(echo "$FILTERS" | jq -r '.filters[-1].filter_hash // empty')"
[ "${#OLD_FILTER_HASH}" -eq 64 ] || fail "initial filter batch missing tail filter_hash"

info "Restarting daemon to verify restored proof/filter state"
stop_daemon
start_daemon

TIP_AFTER_RESTART="$(rpc_scalar "getbestblockhash" "[]" '.')"
[ "$TIP_AFTER_RESTART" = "$OLD_TIP" ] || fail "tip hash changed across restart"
HEIGHT_AFTER_RESTART="$(rpc_scalar "getblockcount" "[]" '.')"
[ "$HEIGHT_AFTER_RESTART" = "$OLD_HEIGHT" ] || fail "tip height changed across restart"

FILTERS_AFTER_RESTART="$(rpc_result "blockchain.getblockfilters" "[$FROM_HEIGHT,10]")"
validate_filter_batch "$FILTERS_AFTER_RESTART"
NEW_FILTER_HASH="$(echo "$FILTERS_AFTER_RESTART" | jq -r '.filters[-1].filter_hash // empty')"
[ "$NEW_FILTER_HASH" = "$OLD_FILTER_HASH" ] || fail "filter hash changed across restart at restored tip"
pass "compact filters remain stable across restart"

RESTORED_STATUS="$(rpc_result "wallet.proofstatus" "[\"$OLD_ROOT\"]")"
[ "$(echo "$RESTORED_STATUS" | jq -r '.stale')" = "false" ] || fail "proofstatus marked bundle root stale after restart without reorg"
pass "proofstatus keeps pre-restart bundle root live after restore"

RESTORED_BUNDLE="$(rpc_result "wallet.getproofbundle" "[{\"min_confirmations\":1,\"spendable_only\":true,\"max_utxos\":64}]")"
assert_bundle_matches_headers "$RESTORED_BUNDLE" "restored bundle"
[ "$(echo "$RESTORED_BUNDLE" | jq -r '.accumulator_root')" = "$OLD_ROOT" ] || fail "bundle root changed across restart"
[ "$(echo "$RESTORED_BUNDLE" | jq -r '.block_hash')" = "$OLD_TIP" ] || fail "bundle block_hash changed across restart"
[ "$(echo "$RESTORED_BUNDLE" | jq -r '.height')" = "$OLD_HEIGHT" ] || fail "bundle height changed across restart"
pass "proof bundle bindings remain stable across restart"

RESTORED_PROOF_ENVELOPE="$(echo "$RESTORED_BUNDLE" | jq -c '
    .proofs[] | select(.success == true) | {
        txid: .txid,
        vout: .vout,
        utreexo_root: $root,
        tip_hash: $tip,
        utreexo_proof: {
            siblings: .siblings,
            position: .position,
            num_leaves: .num_leaves
        }
    }' --arg root "$(echo "$RESTORED_BUNDLE" | jq -r '.accumulator_root')" --arg tip "$(echo "$RESTORED_BUNDLE" | jq -r '.block_hash')" | head -n 1)"
[ -n "$RESTORED_PROOF_ENVELOPE" ] || fail "failed to build proof envelope from restored bundle"

RESTORED_VERIFY_RESULT="$(rpc_result "wallet.verifyutxoproof" "[$RESTORED_PROOF_ENVELOPE,{\"enforce_bound_context\":true}]")"
[ "$(echo "$RESTORED_VERIFY_RESULT" | jq -r '.valid')" = "true" ] || fail "bound-context verification failed after restart"
pass "restored proof bundle verifies under bound context"

ALT_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ALT_ADDR" ] || fail "failed to get alternate mining address"

info "Forcing tip reorg and refreshing proof bundle"
rpc_result "blockchain.invalidateblock" "[\"$OLD_TIP\"]" >/dev/null
rpc_result "generatetoaddress" "[1,\"$ALT_ADDR\"]" >/dev/null

STATUS_AFTER_REORG="$(rpc_result "wallet.proofstatus" "[\"$OLD_ROOT\"]")"
[ "$(echo "$STATUS_AFTER_REORG" | jq -r '.stale')" = "true" ] || fail "proofstatus did not mark old bundle root stale after reorg"
pass "proofstatus marks stale bundle root after reorg"

REFRESHED_BUNDLE="$(rpc_result "wallet.getproofbundle" "[{\"min_confirmations\":1,\"spendable_only\":true,\"max_utxos\":64}]")"
assert_bundle_matches_headers "$REFRESHED_BUNDLE" "refreshed bundle"

NEW_ROOT="$(echo "$REFRESHED_BUNDLE" | jq -r '.accumulator_root')"
[ "$NEW_ROOT" != "$OLD_ROOT" ] || fail "refreshed bundle root did not change after tip reorg"
pass "refreshed bundle root changed after reorg"

echo -e "${GREEN}SUCCESS:${NC} DineroDPI header/filter/proof flow checks passed"
