#!/usr/bin/env bash
#
# Mempool acceptance stress (regtest).
# Focus: transaction admission, ancestor-chain policy, and mined inclusion.
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

DATADIR="/tmp/dinero_mempool_stress_$$"
PORT_RPC="${PORT_RPC:-20996}"
PORT_P2P="${PORT_P2P:-21001}"
DAEMON_PID=""

DINEROD="${DINEROD:-$ROOT_DIR/build/dinerod}"
MINER_ADDR=""
FUNDING_UTXO_TXID=""
FUNDING_UTXO_VOUT=""

print_header() {
    echo -e "\n${CYAN}===============================================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}===============================================================${NC}"
}

print_test() {
    echo -e "\n${BLUE}[$1]${NC} $2"
    echo -e "${BLUE}---------------------------------------------------------------${NC}"
}

pass() {
    echo -e "${GREEN}PASS:${NC} $1"
    ((TESTS_PASSED++))
}

fail() {
    echo -e "${RED}FAIL:${NC} $1"
    ((TESTS_FAILED++))
}

skip() {
    echo -e "${YELLOW}SKIP:${NC} $1"
    ((TESTS_SKIPPED++))
}

info() {
    echo -e "${YELLOW}>${NC} $1"
}

get_cookie() {
    cat "$DATADIR/.cookie" 2>/dev/null || true
}

rpc_raw() {
    local method="$1"
    local params="${2:-[]}"
    local auth
    auth="$(get_cookie)"
    if [ -z "$auth" ]; then
        echo '{"jsonrpc":"2.0","error":{"code":-1,"message":"missing rpc cookie"},"result":null}'
        return 1
    fi

    curl -sS --user "$auth" \
        --data-binary "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}" \
        "http://127.0.0.1:$PORT_RPC"
}

rpc_result() {
    local method="$1"
    local params="${2:-[]}"
    local response
    response="$(rpc_raw "$method" "$params")"
    if echo "$response" | jq -e '.error != null' >/dev/null 2>&1; then
        local emsg
        emsg="$(echo "$response" | jq -r '.error.message // (.error | tostring)')"
        echo "RPC error [$method]: $emsg" >&2
        return 1
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
    pkill -f "dinerod.*$PORT_RPC" 2>/dev/null || true
    sleep 1

    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --debug \
        > "$DATADIR/daemon.log" 2>&1 &

    DAEMON_PID=$!
    wait_for_daemon
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
    pkill -f "dinerod.*$PORT_RPC" 2>/dev/null || true
    sleep 1
}

cleanup() {
    stop_daemon
    rm -rf "$DATADIR"
}

trap cleanup EXIT

mine_blocks() {
    local n="$1"
    local addr="$2"
    rpc_result "generatetoaddress" "[$n,\"$addr\"]" >/dev/null
}

prepare_wallet() {
    rpc_result "wallet.createhd" '["stress"]' >/dev/null 2>&1 || true
    MINER_ADDR="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    mine_blocks 130 "$MINER_ADDR"
    rpc_result "wallet.rescanblockchain" "[0]" >/dev/null || true

    local spendable
    spendable="$(rpc_scalar "wallet.getbalance" "[]" '.spendable // .confirmed // 0')"
    info "Wallet spendable balance: $spendable DIN"

    if [ "$spendable" = "0" ] || [ -z "$spendable" ]; then
        fail "Wallet funding failed (spendable balance is zero)"
        return 1
    fi

    FUNDING_UTXO_TXID="$(rpc_result "wallet.listunspent" "[]" | jq -r '.[0].txid // empty')"
    FUNDING_UTXO_VOUT="$(rpc_result "wallet.listunspent" "[]" | jq -r '.[0].vout // empty')"
    if [ -z "$FUNDING_UTXO_TXID" ] || [ -z "$FUNDING_UTXO_VOUT" ]; then
        fail "No funding UTXO available"
        return 1
    fi

    pass "Wallet prepared and funded"
}

send_test_tx() {
    local dest_addr="$1"
    local amount="$2"
    rpc_result "wallet.sendtoaddress" "[\"$dest_addr\",$amount,\"\",\"\",true]"
}

clear_mempool() {
    rpc_result "mempool.clear" "[]" >/dev/null 2>&1 || true
}

unlock_all_utxos() {
    rpc_result "wallet.lockunspent" "[true,[]]" >/dev/null 2>&1 || true
}

test_acceptance_burst() {
    print_test "TEST 1" "Acceptance burst (independent transactions)"
    clear_mempool
    unlock_all_utxos

    local accepted=0
    local attempts=20

    for ((i=0; i<attempts; i++)); do
        local dest result ok
        dest="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
        result="$(send_test_tx "$dest" "0.10")"
        ok="$(echo "$result" | jq -r '.accepted // false')"
        if [ "$ok" == "true" ]; then
            ((accepted++))
        fi
    done

    local mempool_size
    mempool_size="$(rpc_scalar "mempool.getinfo" "[]" '.size')"

    info "Accepted: $accepted / $attempts, mempool size: $mempool_size"
    if [ "$accepted" -eq 0 ]; then
        fail "No transactions accepted during burst"
        return 1
    fi

    if [ "$mempool_size" -lt "$accepted" ]; then
        fail "Mempool size ($mempool_size) smaller than accepted tx count ($accepted)"
        return 1
    fi

    pass "Mempool accepted burst traffic"
}

test_ancestor_chain_policy() {
    print_test "TEST 2" "Ancestor-chain pressure (single UTXO path)"
    clear_mempool

    # Force chain-building from one spend source:
    # lock all UTXOs, unlock one, then repeatedly spend to self.
    rpc_result "wallet.lockunspent" "[false,[]]" >/dev/null
    rpc_result "wallet.lockunspent" "[true,[{\"txid\":\"$FUNDING_UTXO_TXID\",\"vout\":$FUNDING_UTXO_VOUT}]]" >/dev/null

    local accepted=0
    local rejected=0
    local reject_reason=""
    local limit_attempts=30

    for ((i=1; i<=limit_attempts; i++)); do
        local result ok reason
        result="$(send_test_tx "$MINER_ADDR" "0.05")"
        ok="$(echo "$result" | jq -r '.accepted // false')"
        if [ "$ok" == "true" ]; then
            ((accepted++))
            continue
        fi

        rejected=1
        reason="$(echo "$result" | jq -r '.reject_reason // .error // "unknown rejection"')"
        reject_reason="$reason"
        info "First rejection at tx #$i: $reject_reason"
        break
    done

    unlock_all_utxos

    if [ "$accepted" -eq 0 ]; then
        fail "Ancestor test produced zero accepted transactions"
        return 1
    fi

    if [ "$rejected" -eq 0 ]; then
        fail "No rejection observed by tx #$limit_attempts; ancestor policy may be unenforced"
        return 1
    fi

    if echo "$reject_reason" | tr '[:upper:]' '[:lower:]' | grep -Eq "ancestor|chain|too-long-mempool-chain"; then
        pass "Ancestor policy rejection observed after $accepted accepted txs"
    else
        fail "Unexpected rejection reason: $reject_reason"
        return 1
    fi
}

test_decode_rejection_surface() {
    print_test "TEST 3" "Decode rejection surface (testmempoolaccept)"

    local result allowed reason
    result="$(rpc_result "mempool.testmempoolaccept" "[\"deadbeef\"]")"
    allowed="$(echo "$result" | jq -r '.[0].allowed // false')"
    reason="$(echo "$result" | jq -r '.[0]["reject-reason"] // "none"')"

    if [ "$allowed" == "false" ]; then
        pass "Malformed raw tx rejected: $reason"
    else
        fail "Malformed raw tx unexpectedly accepted"
        return 1
    fi
}

test_mined_inclusion() {
    print_test "TEST 4" "Accepted transaction is mined and chain advances"
    clear_mempool
    unlock_all_utxos

    local dest result ok txid tip_hash in_block mempool_size mempool_ids tx_still_in_mempool
    dest="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    result="$(send_test_tx "$dest" "0.25")"
    ok="$(echo "$result" | jq -r '.accepted // false')"
    txid="$(echo "$result" | jq -r '.txid // empty')"

    if [ "$ok" != "true" ] || [ -z "$txid" ]; then
        fail "Could not submit tx for mined inclusion test"
        return 1
    fi

    mine_blocks 1 "$MINER_ADDR"
    tip_hash="$(rpc_scalar "getbestblockhash" "[]" '.')"
    in_block="$(rpc_result "getblock" "[\"$tip_hash\"]" | jq -r --arg txid "$txid" '.tx | index($txid) != null')"
    mempool_size="$(rpc_scalar "mempool.getinfo" "[]" '.size')"
    mempool_ids="$(rpc_result "mempool.getrawmempool" "[]")"
    tx_still_in_mempool="$(echo "$mempool_ids" | jq -r --arg txid "$txid" 'index($txid) != null')"

    if [ "$in_block" == "true" ]; then
        pass "Submitted tx was included in mined block"
    else
        fail "Submitted tx not found in mined block"
        return 1
    fi

    if [ "$tx_still_in_mempool" == "false" ]; then
        pass "Submitted tx left mempool after mining (remaining size=$mempool_size)"
    else
        fail "Submitted tx still in mempool after mining (size=$mempool_size)"
        return 1
    fi
}

main() {
    print_header "MEMPOOL ACCEPTANCE STRESS SUITE"
    echo -e "${YELLOW}Runtime policy checks on regtest.${NC}"

    if [ ! -x "$DINEROD" ]; then
        echo -e "${RED}ERROR:${NC} dinerod not found at $DINEROD"
        exit 1
    fi
    if ! command -v jq >/dev/null 2>&1; then
        echo -e "${RED}ERROR:${NC} jq is required"
        exit 1
    fi

    start_daemon
    prepare_wallet || true
    test_acceptance_burst || true
    test_ancestor_chain_policy || true
    test_decode_rejection_surface || true
    test_mined_inclusion || true

    print_header "SUMMARY"
    echo -e "${GREEN}PASSED:${NC}  $TESTS_PASSED"
    echo -e "${RED}FAILED:${NC}  $TESTS_FAILED"
    echo -e "${YELLOW}SKIPPED:${NC} $TESTS_SKIPPED"

    if [ "$TESTS_FAILED" -eq 0 ]; then
        echo -e "${GREEN}Mempool acceptance stress suite PASSED.${NC}"
        exit 0
    fi

    echo -e "${RED}Mempool acceptance stress suite FAILED.${NC}"
    exit 1
}

main "$@"
