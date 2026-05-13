#!/usr/bin/env bash
#
# Hardware wallet RPC smoke test:
# - registration is live
# - USB session status is truthful when idle
# - connect requires a device_id
# - disconnect is idempotent without hardware
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="$(mktemp -d "/tmp/hardware_wallet_rpc.XXXXXX")"
PORT_RPC="${PORT_RPC:-$((22000 + ($$ % 1000)))}"
PORT_P2P="${PORT_P2P:-$((24000 + ($$ % 1000)))}"
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
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --debug \
        > "$DATADIR/daemon.log" 2>&1 &
    DAEMON_PID=$!
    disown "$DAEMON_PID" 2>/dev/null || true
    wait_for_daemon || fail "daemon failed to start"
}

wait_for_process_exit() {
    local pid="$1"
    local waited=0
    local max_wait="${2:-10}"
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$max_wait" ]; then
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 0
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        rpc_raw "stop" "[]" >/dev/null 2>&1 || true
        if ! wait_for_process_exit "$DAEMON_PID" 5; then
            kill "$DAEMON_PID" 2>/dev/null || true
        fi
        if ! wait_for_process_exit "$DAEMON_PID" 5; then
            kill -9 "$DAEMON_PID" 2>/dev/null || true
        fi
        if kill -0 "$DAEMON_PID" 2>/dev/null; then
            info "daemon PID $DAEMON_PID did not exit cleanly after shutdown signals; continuing without blocking on wait"
        fi
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

ENUMERATION="$(rpc_result "hwallet.enumeratehwdevices" '[]' | jq -c '.')"
echo "$ENUMERATION" | jq -e '.result.interactive_usb_backend | type == "string"' >/dev/null || fail "enumeration backend marker missing: $ENUMERATION"
echo "$ENUMERATION" | jq -e '.result.devices | type == "array"' >/dev/null || fail "enumeration devices missing: $ENUMERATION"

DEVICE_INFO="$(rpc_result "hwallet.gethwdeviceinfo" '[]' | jq -c '.')"
echo "$DEVICE_INFO" | jq -e '.result.connected == false' >/dev/null || fail "idle USB session should not be connected: $DEVICE_INFO"
echo "$DEVICE_INFO" | jq -e '.result.interactive_usb_backend | type == "string"' >/dev/null || fail "USB info backend marker missing: $DEVICE_INFO"

FINGERPRINT_IDLE="$(rpc_result "hwallet.getmasterfingerprint" '[]' | jq -c '.')"
echo "$FINGERPRINT_IDLE" | jq -e '.error.message == "No active USB hardware-wallet session."' >/dev/null || fail "idle fingerprint export returned unexpected response: $FINGERPRINT_IDLE"

ADDRESS_IDLE="$(rpc_result "hwallet.gethwaddress" '{"derivation_path":"m/86'\''/1447'\''/0'\''/0/0"}' | jq -c '.')"
echo "$ADDRESS_IDLE" | jq -e '.error.message == "No active USB hardware-wallet session."' >/dev/null || fail "idle USB address lookup returned unexpected response: $ADDRESS_IDLE"

DESCRIPTOR_IDLE="$(rpc_result "hwallet.gethwaccountdescriptor" '{"derivation_path":"m/86'\''/1447'\''/0'\''"}' | jq -c '.')"
echo "$DESCRIPTOR_IDLE" | jq -e '.error.message == "No active USB hardware-wallet session."' >/dev/null || fail "idle USB descriptor export returned unexpected response: $DESCRIPTOR_IDLE"

SIGN_IDLE="$(rpc_result "hwallet.signpsbt" '{"psbt":"cHNidP8BAAoCAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/////AQEAAAAAAAAAFgAUAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="}' | jq -c '.')"
echo "$SIGN_IDLE" | jq -e '.error.message == "No active USB hardware-wallet session."' >/dev/null || fail "idle USB sign returned unexpected response: $SIGN_IDLE"

DISCONNECT="$(rpc_result "hwallet.disconnecthwdevice" '[]' | jq -c '.')"
echo "$DISCONNECT" | jq -e '.result.disconnected == true' >/dev/null || fail "disconnect did not report success: $DISCONNECT"
echo "$DISCONNECT" | jq -e '.result.had_active_session == false' >/dev/null || fail "disconnect should be idempotent when idle: $DISCONNECT"

CONNECT_RESPONSE="$(rpc_raw "hwallet.connecthwdevice" '{}' | jq -c '.')"
echo "$CONNECT_RESPONSE" | jq -e '.result.error.message == "device_id parameter required"' >/dev/null || fail "connect without device_id returned unexpected response: $CONNECT_RESPONSE"

pass "hardware wallet RPCs expose truthful idle USB session state"
