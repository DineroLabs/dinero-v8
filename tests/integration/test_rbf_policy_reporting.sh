#!/usr/bin/env bash
#
# Regression: rbf.* RPCs must report the same runtime policy the mempool is
# actually enforcing, both when RBF is disabled and when it is explicitly
# enabled.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATADIR="/tmp/rbf_policy_reporting_$$"
PORT_RPC=23144
PORT_P2P=23145
DAEMON_PID=""

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"

info() { echo -e "${BLUE}INFO:${NC} $*"; }
pass() { echo -e "${GREEN}PASS:${NC} $*"; }

fail() {
    echo -e "${RED}FAIL:${NC} $*" >&2
    if [ -f "$DATADIR/daemon.log" ]; then
        echo -e "${YELLOW}---- daemon.log tail ----${NC}" >&2
        tail -n 160 "$DATADIR/daemon.log" >&2 || true
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
        waited=$((waited + 1))
    done
    return 1
}

start_daemon() {
    local extra_flag="${1:-}"
    mkdir -p "$DATADIR"
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --debug \
        ${extra_flag:+$extra_flag} \
        >> "$DATADIR/daemon.log" 2>&1 &
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

reset_datadir() {
    rm -rf "$DATADIR"
    mkdir -p "$DATADIR"
    : > "$DATADIR/daemon.log"
}

cleanup() {
    stop_daemon
    rm -rf "$DATADIR"
}

assert_disabled_reporting() {
    local config status estimate
    config="$(rpc_result "rbf.config" "[]")"
    status="$(rpc_result "rbf.status" "[]")"
    estimate="$(rpc_result "rbf.estimate" "[100000]")"

    [ "$(echo "$config" | jq -r '.enabled')" = "false" ] || fail "rbf.config should report disabled by default"
    [ "$(echo "$config" | jq -r '.policy')" = "disabled" ] || fail "rbf.config policy should be disabled"
    [ "$(echo "$status" | jq -r '.enabled')" = "false" ] || fail "rbf.status should report disabled by default"
    [ "$(echo "$estimate" | jq -r '.enabled')" = "false" ] || fail "rbf.estimate should report disabled by default"
    [ "$(echo "$estimate" | jq -r '.description')" = "RBF is disabled on this node; replacements will be rejected" ] \
        || fail "rbf.estimate should explain that RBF is disabled"
}

assert_enabled_reporting() {
    local config status estimate
    config="$(rpc_result "rbf.config" "[]")"
    status="$(rpc_result "rbf.status" "[]")"
    estimate="$(rpc_result "rbf.estimate" "[100000,205,205]")"

    [ "$(echo "$config" | jq -r '.enabled')" = "true" ] || fail "rbf.config should report enabled when configured"
    [ "$(echo "$config" | jq -r '.min_relay_fee_rate')" = "1000" ] || fail "unexpected min relay fee rate"
    [ "$(echo "$config" | jq -r '.incremental_relay_fee')" = "1000" ] || fail "unexpected incremental relay fee"
    [ "$(echo "$config" | jq -r '.max_replacement_count')" = "100" ] || fail "unexpected replacement limit"
    [ "$(echo "$status" | jq -r '.enabled')" = "true" ] || fail "rbf.status should report enabled when configured"
    [ "$(echo "$estimate" | jq -r '.enabled')" = "true" ] || fail "rbf.estimate should report enabled when configured"
    [ "$(echo "$estimate" | jq -r '.replacement_vsize')" = "205" ] || fail "rbf.estimate should echo replacement_vsize"
    [ "$(echo "$estimate" | jq -r '.replaced_vsize')" = "205" ] || fail "rbf.estimate should echo replaced_vsize"
    [ "$(echo "$estimate" | jq -r '.total_relay_vsize')" = "410" ] || fail "rbf.estimate total_relay_vsize mismatch"
    [ "$(echo "$estimate" | jq -r '.required_additional_fee')" = "410" ] || fail "rbf.estimate required_additional_fee mismatch"
    [ "$(echo "$estimate" | jq -r '.required_fee')" = "100410" ] || fail "rbf.estimate required_fee mismatch"
}

trap cleanup EXIT

[ -x "$DINEROD" ] || fail "missing dinerod binary at $DINEROD"
command -v jq >/dev/null 2>&1 || fail "jq is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

info "Starting daemon with default mempool policy"
reset_datadir
start_daemon
assert_disabled_reporting
pass "rbf.* reports disabled policy when mempool.enable_rbf is unset"

info "Restarting daemon with mempool.enable_rbf=1"
stop_daemon
reset_datadir
start_daemon "--mempool.enable_rbf=1"
assert_enabled_reporting
pass "rbf.* reports the live enabled policy and estimate inputs"
