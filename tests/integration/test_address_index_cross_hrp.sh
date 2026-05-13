#!/usr/bin/env bash
#
# Regression: address-index RPCs must resolve the same witness program even if
# the queried Dinero bech32m address uses a different supported HRP.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/address_index_cross_hrp_$$"
PORT_RPC=23150
PORT_P2P=23151
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

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

wait_for_mempool_address() {
    local address="$1"
    local max_wait=20
    local waited=0
    while [ "$waited" -lt "$max_wait" ]; do
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

convert_taproot_address_to_mainnet() {
    local source_address="$1"
    python3 - "$source_address" <<'PY'
import sys

CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
CHARSET_REV = {c: i for i, c in enumerate(CHARSET)}
GENERATOR = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]
BECH32M_CONST = 0x2bc830a3

def hrp_expand(hrp):
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]

def polymod(values):
    chk = 1
    for value in values:
        top = chk >> 25
        chk = ((chk & 0x1ffffff) << 5) ^ value
        for i in range(5):
            if (top >> i) & 1:
                chk ^= GENERATOR[i]
    return chk

def create_checksum(hrp, data):
    values = hrp_expand(hrp) + data
    polymod_value = polymod(values + [0, 0, 0, 0, 0, 0]) ^ BECH32M_CONST
    return [(polymod_value >> 5 * (5 - i)) & 31 for i in range(6)]

def convertbits(data, from_bits, to_bits, pad=True):
    acc = 0
    bits = 0
    ret = []
    maxv = (1 << to_bits) - 1
    max_acc = (1 << (from_bits + to_bits - 1)) - 1
    for value in data:
        if value < 0 or value >> from_bits:
            raise ValueError("invalid data range")
        acc = ((acc << from_bits) | value) & max_acc
        bits += from_bits
        while bits >= to_bits:
            bits -= to_bits
            ret.append((acc >> bits) & maxv)
    if pad:
        if bits:
            ret.append((acc << (to_bits - bits)) & maxv)
    elif bits >= from_bits or ((acc << (to_bits - bits)) & maxv):
        raise ValueError("invalid padding")
    return ret

def verify_checksum(hrp, data):
    return polymod(hrp_expand(hrp) + data) == BECH32M_CONST

address = sys.argv[1].strip().lower()
pos = address.rfind("1")
if pos <= 0:
    raise ValueError("invalid bech32m address separator")
hrp = address[:pos]
data = [CHARSET_REV[c] for c in address[pos + 1:]]
if not verify_checksum(hrp, data):
    raise ValueError("invalid bech32m checksum")
payload = data[:-6]
if not payload or payload[0] != 1:
    raise ValueError("not a taproot witness version")
witness_program = bytes(convertbits(payload[1:], 5, 8, False))
if len(witness_program) != 32:
    raise ValueError("unexpected taproot witness program length")
data = [1] + convertbits(witness_program, 8, 5, True)
checksum = create_checksum("din", data)
print("din1" + "".join(CHARSET[d] for d in data + checksum))
PY
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

info "Starting daemon"
reset_datadir
start_daemon

rpc_result "wallet.createhd" '["address_index_cross_hrp_wallet"]' >/dev/null 2>&1 || true
MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$MINER_ADDR" ] || fail "wallet.getnewaddress returned empty mining address"

REGTEST_RECIPIENT="$(rpc_scalar "wallet.getnewaddress" "[]" '.address // empty')"
[ -n "$REGTEST_RECIPIENT" ] || fail "wallet.getnewaddress returned empty recipient address"

MAINNET_RECIPIENT="$(convert_taproot_address_to_mainnet "$REGTEST_RECIPIENT")"
[ -n "$MAINNET_RECIPIENT" ] || fail "failed to encode mainnet taproot address"
pass "mainnet cross-HRP address encoded for same witness program"

info "Mining 101 blocks to create mature funds"
rpc_result "generatetoaddress" "[101,\"$MINER_ADDR\"]" >/dev/null

ZERO_BALANCE="$(rpc_result "getaddressbalance" "[\"$MAINNET_RECIPIENT\"]")"
[ "$(echo "$ZERO_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "expected zero confirmed balance before send"
[ "$(echo "$ZERO_BALANCE" | jq -r '.estimated_balance')" = "0" ] || fail "expected zero estimated balance before send"
[ "$(rpc_result "getaddresshistory" "[\"$MAINNET_RECIPIENT\"]" | jq -r '.transactions | length')" = "0" ] || fail "expected zero history before send"
pass "cross-HRP balance/history query starts empty"

info "Sending 1.25 DIN to regtest address and querying via mainnet HRP"
SEND_RESULT="$(rpc_result "wallet.sendtoaddress" "[\"$REGTEST_RECIPIENT\",1.25]")"
TXID="$(echo "$SEND_RESULT" | jq -r '.txid // empty')"
[ -n "$TXID" ] || fail "wallet.sendtoaddress did not return txid"

wait_for_mempool_address "$MAINNET_RECIPIENT" || fail "cross-HRP address never appeared in getaddressmempool"

PENDING_BALANCE="$(rpc_result "getaddressbalance" "[\"$MAINNET_RECIPIENT\"]")"
[ "$(echo "$PENDING_BALANCE" | jq -r '.confirmed')" = "0" ] || fail "pending cross-HRP query must not change confirmed balance"
[ "$(echo "$PENDING_BALANCE" | jq -r '.unconfirmed')" = "125000000" ] || fail "expected positive unconfirmed delta via cross-HRP query"
[ "$(echo "$PENDING_BALANCE" | jq -r '.estimated_balance')" = "125000000" ] || fail "expected advisory estimated balance via cross-HRP query"

PENDING_MEMPOOL="$(rpc_result "getaddressmempool" "[\"$MAINNET_RECIPIENT\"]")"
[ "$(echo "$PENDING_MEMPOOL" | jq -r '.transactions | length')" = "1" ] || fail "expected one cross-HRP mempool entry"
[ "$(echo "$PENDING_MEMPOOL" | jq -r '.transactions[0].txid // empty')" = "$TXID" ] || fail "cross-HRP mempool txid mismatch"
[ "$(echo "$PENDING_MEMPOOL" | jq -r '.transactions[0].type // empty')" = "receive" ] || fail "cross-HRP mempool type should be receive"
pass "cross-HRP mempool overlay resolves to the same witness program"

info "Mining confirmation block"
rpc_result "generatetoaddress" "[1,\"$MINER_ADDR\"]" >/dev/null

CONFIRMED_BALANCE="$(rpc_result "getaddressbalance" "[\"$MAINNET_RECIPIENT\"]")"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.confirmed')" = "125000000" ] || fail "cross-HRP confirmed balance mismatch"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.unconfirmed')" = "0" ] || fail "cross-HRP unconfirmed delta should clear after mining"
[ "$(echo "$CONFIRMED_BALANCE" | jq -r '.estimated_balance')" = "125000000" ] || fail "cross-HRP estimated balance should match confirmed after mining"

CONFIRMED_HISTORY="$(rpc_result "getaddresshistory" "[\"$MAINNET_RECIPIENT\"]")"
[ "$(echo "$CONFIRMED_HISTORY" | jq -r '.transactions | length')" = "1" ] || fail "expected one confirmed cross-HRP history entry"
[ "$(echo "$CONFIRMED_HISTORY" | jq -r '.transactions[0].txid // empty')" = "$TXID" ] || fail "cross-HRP confirmed history txid mismatch"
pass "cross-HRP confirmed history resolves to the same witness program"

echo -e "${GREEN}SUCCESS:${NC} address-index cross-HRP query checks passed"
