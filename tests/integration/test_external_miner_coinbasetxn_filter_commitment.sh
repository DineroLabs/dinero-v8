#!/usr/bin/env bash
#
# Regression: external miners must consume daemon-owned coinbasetxn exactly,
# and the accepted block's DNRF commitment must agree with served filter data.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="${DATADIR:-/tmp/external_miner_coinbasetxn_$$}"
PORT_RPC="${PORT_RPC:-23230}"
PORT_P2P="${PORT_P2P:-23229}"
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"
PRESEEDED_DATADIR="${PRESEEDED_DATADIR:-0}"

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"
MINER="${MINER:-$ROOT_DIR/tests/mining/dinero_cpu_miner.py}"
FILTER_HEADER="$ROOT_DIR/include/consensus/filter_commitment.h"
REPORT="$DATADIR/miner-report.json"

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }

fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [ -f "$REPORT" ]; then
        echo -e "${YELLOW}---- miner report ----${NC}" >&2
        cat "$REPORT" >&2 || true
    fi
    if [ -f "$DATADIR/daemon.log" ]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 120 "$DATADIR/daemon.log" >&2 || true
    fi
    exit 1
}

get_cookie() {
    cat "$DATADIR/.cookie" 2>/dev/null || true
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
        -u "$cookie" \
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

wait_for_wallet() {
    local max_wait=120
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
        if rpc_raw "wallet.getstatus" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
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

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
[ -f "$MINER" ] || fail "missing external miner script at $MINER"
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

info "Starting daemon"
if [ "$PRESEEDED_DATADIR" = "1" ]; then
    mkdir -p "$DATADIR"
    : > "$DATADIR/daemon.log"
else
    reset_datadir
fi
start_daemon

rpc_result "wallet.createhd" '["external_miner_wallet"]' >/dev/null 2>&1 || true
wait_for_wallet || fail "wallet RPC failed to become ready"
ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$ADDR" ] || fail "wallet.getnewaddress returned empty address"

CURRENT_HEIGHT="$(rpc_scalar "getblockcount" "[]" '.')"
if [ "$CURRENT_HEIGHT" -lt "$FILTER_ACTIVATION_HEIGHT" ]; then
    info "Mining to activation height $FILTER_ACTIVATION_HEIGHT"
    mine_to_height "$FILTER_ACTIVATION_HEIGHT" "$ADDR"
    CURRENT_HEIGHT="$(rpc_scalar "getblockcount" "[]" '.')"
fi

[ "$CURRENT_HEIGHT" -ge "$FILTER_ACTIVATION_HEIGHT" ] || fail "tip did not reach activation height"
TARGET_HEIGHT=$((CURRENT_HEIGHT + 1))

info "Mining one external block via daemon-owned coinbasetxn"
python3 "$MINER" \
    --rpc-url "http://127.0.0.1:$PORT_RPC" \
    --cookie "$DATADIR/.cookie" \
    --address "$ADDR" \
    --blocks 1 \
    --report "$REPORT" \
    >/tmp/external-miner-stdout-$$.log 2>/tmp/external-miner-stderr-$$.log \
    || fail "external miner failed"

[ -f "$REPORT" ] || fail "external miner did not write report"
[ "$(jq -r '.accepted' "$REPORT")" = "true" ] || fail "external miner report says submitblock was rejected"
[ "$(jq -r '.template_coinbasetxn_present' "$REPORT")" = "true" ] || fail "template did not include coinbasetxn"
[ "$(jq -r '.template_height' "$REPORT")" = "$TARGET_HEIGHT" ] || fail "unexpected template height in miner report"
[ "$(rpc_scalar "getblockcount" "[]" '.')" = "$TARGET_HEIGHT" ] || fail "unexpected tip after external mining"

FILTER_JSON="$(rpc_result "blockchain.getblockfilters" "[$TARGET_HEIGHT,1]")"
[ "$(echo "$FILTER_JSON" | jq -r '.count')" = "1" ] || fail "no filter row returned at height $TARGET_HEIGHT"
ROW="$(echo "$FILTER_JSON" | jq '.filters[0]')"

FILTER_HASH="$(echo "$ROW" | jq -r '.filter_hash // empty')"
COINBASE_TX="$(echo "$ROW" | jq -r '.coinbase_tx // empty')"
COINBASE_TXID="$(echo "$ROW" | jq -r '.coinbase_txid // empty')"
BLOCK_HASH="$(echo "$ROW" | jq -r '.block_hash // empty')"
TEMPLATE_COINBASE_HEX="$(jq -r '.template_coinbase_hex // empty' "$REPORT")"
TEMPLATE_COINBASE_TXID="$(jq -r '.template_coinbase_txid // empty' "$REPORT")"

[ "${#FILTER_HASH}" -eq 64 ] || fail "missing filter hash"
[ -n "$COINBASE_TX" ] || fail "missing coinbase_tx in filter row"
[ "${#COINBASE_TXID}" -eq 64 ] || fail "missing coinbase_txid in filter row"
[ "${#BLOCK_HASH}" -eq 64 ] || fail "missing block hash in filter row"
[ "$COINBASE_TX" = "$TEMPLATE_COINBASE_HEX" ] || fail "accepted block coinbase differs from daemon template coinbasetxn"
[ "$COINBASE_TXID" = "$TEMPLATE_COINBASE_TXID" ] || fail "accepted block coinbase txid differs from daemon template coinbasetxn"

DECODED="$(rpc_result "decoderawtransaction" "[\"$COINBASE_TX\"]")"
DNRF_SCRIPT="$(echo "$DECODED" | jq -r '[.vout[]?.scriptPubKey.hex | select(startswith("6a25444e524601"))] | last // empty')"
[ -n "$DNRF_SCRIPT" ] || fail "coinbase is missing DNRF commitment"
[ "${#DNRF_SCRIPT}" -eq 78 ] || fail "unexpected DNRF script size"

COMMITTED_RAW="${DNRF_SCRIPT:14:64}"
[ "${#COMMITTED_RAW}" -eq 64 ] || fail "failed to extract committed filter hash"
COMMITTED_DISPLAY="$(hex_reverse_bytes "$COMMITTED_RAW")"
[ "$COMMITTED_DISPLAY" = "$FILTER_HASH" ] || fail "DNRF hash does not match served filter hash"

pass "external miner used daemon coinbasetxn exactly"
pass "accepted block filter commitment matches served filter data"
echo -e "${GREEN}SUCCESS:${NC} External miner coinbasetxn/filter commitment checks passed"
