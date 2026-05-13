#!/usr/bin/env bash
#
# Regression: mined blocks and served filters must stay coherent across the
# DNRF activation boundary.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/filter_commitment_activation_boundary_$$"
PORT_RPC=23130
PORT_P2P=23129
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"
FILTER_HEADER="$ROOT_DIR/include/consensus/filter_commitment.h"

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

hex_reverse_bytes() {
    python3 - "$1" <<'PY'
import sys
h = sys.argv[1].strip()
if len(h) % 2:
    raise SystemExit("hex length must be even")
print("".join(reversed([h[i:i+2] for i in range(0, len(h), 2)])))
PY
}

mine_to_height() {
    local target_height="$1"
    local address="$2"
    local current_height remaining batch mined_count result
    current_height="$(rpc_scalar "getblockcount" "[]" '.')"
    while [ "$current_height" -lt "$target_height" ]; do
        remaining=$((target_height - current_height))
        batch=1000
        if [ "$remaining" -lt "$batch" ]; then
            batch="$remaining"
        fi

        result="$(rpc_result "generatetoaddress" "[$batch,\"$address\"]")"
        mined_count="$(echo "$result" | jq -r '(.blocks // .) | length')"
        [ "$mined_count" -eq "$batch" ] || fail "requested $batch blocks, mined $mined_count"

        current_height="$(rpc_scalar "getblockcount" "[]" '.')"
    done
}

validate_filter_commitment_height() {
    local height="$1"

    local filter_json row block_hash filter_hash coinbase_tx coinbase_txid tx_count decoded dnrf_script committed_raw committed_display header_hash
    filter_json="$(rpc_result "blockchain.getblockfilters" "[$height,1]")"
    [ "$(echo "$filter_json" | jq -r '.count')" = "1" ] || fail "no filter row returned at height $height"
    row="$(echo "$filter_json" | jq '.filters[0]')"

    block_hash="$(echo "$row" | jq -r '.block_hash // empty')"
    filter_hash="$(echo "$row" | jq -r '.filter_hash // empty')"
    coinbase_tx="$(echo "$row" | jq -r '.coinbase_tx // empty')"
    coinbase_txid="$(echo "$row" | jq -r '.coinbase_txid // empty')"
    tx_count="$(echo "$row" | jq -r '.tx_count // 0')"

    [ "$(echo "$row" | jq -r '.height')" = "$height" ] || fail "filter row height mismatch at $height"
    [ "${#block_hash}" -eq 64 ] || fail "missing block_hash at height $height"
    [ "${#filter_hash}" -eq 64 ] || fail "missing filter_hash at height $height"
    [ -n "$coinbase_tx" ] || fail "missing coinbase_tx at height $height"
    [ "${#coinbase_txid}" -eq 64 ] || fail "missing coinbase_txid at height $height"
    [ "$tx_count" -ge 1 ] || fail "invalid tx_count at height $height"

    header_hash="$(rpc_scalar "getblockhash" "[$height]" '.')"
    [ "$header_hash" = "$block_hash" ] || fail "block_hash/header mismatch at height $height"

    decoded="$(rpc_result "decoderawtransaction" "[\"$coinbase_tx\"]")"
    dnrf_script="$(echo "$decoded" | jq -r '[.vout[]?.scriptPubKey.hex | select(startswith("6a25444e524601"))] | last // empty')"
    [ -n "$dnrf_script" ] || fail "coinbase is missing DNRF commitment at height $height"
    [ "${#dnrf_script}" -eq 78 ] || fail "unexpected DNRF script size at height $height"

    committed_raw="${dnrf_script:14:64}"
    [ "${#committed_raw}" -eq 64 ] || fail "failed to extract committed filter hash at height $height"
    committed_display="$(hex_reverse_bytes "$committed_raw")"
    [ "$committed_display" = "$filter_hash" ] || fail "DNRF hash does not match served filter hash at height $height"

    pass "height $height DNRF commitment matches served filter"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
[ -f "$FILTER_HEADER" ] || fail "missing filter commitment header at $FILTER_HEADER"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

FILTER_ACTIVATION_HEIGHT="$(python3 - "$FILTER_HEADER" <<'PY'
import pathlib
import re
import sys
text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'ACTIVATION_HEIGHT\s*=\s*(\d+)', text)
if not match:
    raise SystemExit("could not parse activation height")
print(match.group(1))
PY
)"

PRE_HEIGHT=$((FILTER_ACTIVATION_HEIGHT - 1))
AT_HEIGHT="$FILTER_ACTIVATION_HEIGHT"
POST_HEIGHT=$((FILTER_ACTIVATION_HEIGHT + 1))

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["filter_activation_wallet"]' >/dev/null 2>&1 || true
ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ADDR" ] || fail "wallet.getnewaddress returned empty address"

info "Mining to activation boundary at height $POST_HEIGHT"
mine_to_height "$POST_HEIGHT" "$ADDR"
[ "$(rpc_scalar "getblockcount" "[]" '.')" = "$POST_HEIGHT" ] || fail "unexpected tip after activation mining"

validate_filter_commitment_height "$PRE_HEIGHT"
validate_filter_commitment_height "$AT_HEIGHT"
validate_filter_commitment_height "$POST_HEIGHT"

echo -e "${GREEN}SUCCESS:${NC} Filter commitment activation boundary checks passed"
