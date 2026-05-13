#!/usr/bin/env bash
#
# Utreexo consensus/regtest validation suite.
# This script is a runtime gate for cross-invariants between:
# - header utreexo_root commitments
# - chainstate accumulator commitment
# - reorg/restart behavior
# - proof generation surface
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

DATADIR="/tmp/utreexo_test_$$"
PORT_RPC=22020
PORT_P2P=22019
DAEMON_PID=""
KEEP_DATADIR="${KEEP_DATADIR:-0}"

BUILD_DIR="$(dirname "$0")/../build"
DINEROD="${DINEROD:-$BUILD_DIR/dinerod}"

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

mine_blocks() {
    local count="$1"
    local addr="$2"
    rpc_result "generatetoaddress" "[$count,\"$addr\"]" | jq -r '.blocks[]? // .[]?'
}

get_header_utreexo_root() {
    local block_hash="$1"
    rpc_result "getblockheader" "[\"$block_hash\"]" | jq -r '.utreexo_root_raw // .utreexo_root // .utreexocommitment // empty'
}

get_chainstate_commitment_raw() {
    rpc_scalar "blockchain.getutreexocommitment" "[]" '.commitment'
}

wait_for_daemon() {
    local max_wait=30
    local waited=0
    info "Waiting for daemon RPC..."
    while [ "$waited" -lt "$max_wait" ]; do
        if rpc_raw "getblockcount" "[]" | jq -e '.error == null' >/dev/null 2>&1; then
            info "Daemon ready after ${waited}s"
            return 0
        fi
        sleep 1
        ((waited++))
    done

    fail "Daemon failed to start within ${max_wait}s"
    return 1
}

start_daemon() {
    info "Starting dinerod with datadir: $DATADIR"
    pkill -f "dinerod.*$PORT_RPC" 2>/dev/null || true
    sleep 1
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
    wait_for_daemon
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        info "Stopping daemon PID $DAEMON_PID"
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
    pkill -f "dinerod.*$PORT_RPC" 2>/dev/null || true
    sleep 1
}

cleanup() {
    stop_daemon
    if [ "$KEEP_DATADIR" = "1" ]; then
        info "KEEP_DATADIR=1; preserving datadir: $DATADIR"
        return
    fi
    rm -rf "$DATADIR"
}

trap cleanup EXIT

ensure_wallet_and_mining_address() {
    rpc_result "wallet.createhd" '["test"]' >/dev/null 2>&1 || true
    rpc_scalar "wallet.getnewaddress" "[]" '.address'
}

assert_tip_header_matches_chainstate() {
    local tip_hash tip_header_root chainstate_root
    tip_hash="$(rpc_scalar "getbestblockhash" "[]" '.')"
    tip_header_root="$(get_header_utreexo_root "$tip_hash")"
    chainstate_root="$(get_chainstate_commitment_raw)"

    if [ "$tip_header_root" == "$chainstate_root" ]; then
        pass "Tip header root matches chainstate commitment"
        return 0
    fi

    fail "Tip/header mismatch: header=$tip_header_root chainstate=$chainstate_root"
    return 1
}

test_genesis_to_block1() {
    print_test "TEST 1" "Genesis -> Block 1 root transition"

    local zero_root genesis_hash genesis_header_root genesis_chainstate_root addr block1_hash block1_header_root chainstate_root
    zero_root="$(printf '0%.0s' {1..64})"

    genesis_hash="$(rpc_scalar "getblockhash" "[0]" '.')"
    genesis_header_root="$(get_header_utreexo_root "$genesis_hash")"
    genesis_chainstate_root="$(get_chainstate_commitment_raw)"
    info "Genesis hash: ${genesis_hash:0:16}..."
    info "Genesis header root: ${genesis_header_root:0:16}..."
    info "Genesis chainstate: ${genesis_chainstate_root:0:16}..."

    if [ "$genesis_header_root" == "$genesis_chainstate_root" ]; then
        pass "Genesis header root matches chainstate"
    else
        fail "Genesis mismatch: header=$genesis_header_root chainstate=$genesis_chainstate_root"
        return 1
    fi

    if [ "$genesis_header_root" != "$zero_root" ]; then
        pass "Genesis header root is non-zero"
    else
        fail "Genesis header root should not be zero"
        return 1
    fi

    addr="$(ensure_wallet_and_mining_address)"
    block1_hash="$(mine_blocks 1 "$addr" | head -n1)"
    block1_header_root="$(get_header_utreexo_root "$block1_hash")"
    chainstate_root="$(get_chainstate_commitment_raw)"

    info "Block 1 hash: ${block1_hash:0:16}..."
    info "Block 1 header root: ${block1_header_root:0:16}..."
    info "Chainstate: ${chainstate_root:0:16}..."

    if [ "$block1_header_root" == "$chainstate_root" ]; then
        pass "Block 1 header root matches chainstate"
    else
        fail "Block 1 mismatch: header=$block1_header_root chainstate=$chainstate_root"
        return 1
    fi

    if [ "$block1_header_root" != "$zero_root" ]; then
        pass "Block 1 header root is non-zero"
    else
        fail "Block 1 header root remained zero"
        return 1
    fi

    local leaves
    leaves="$(rpc_scalar "blockchain.getutreexostats" "[]" '.num_leaves')"
    if [ "$leaves" -ge 1 ]; then
        pass "Accumulator leaves >= 1 after first mined block"
    else
        fail "Accumulator leaf count invalid: $leaves"
        return 1
    fi
}

test_block1_to_block2() {
    print_test "TEST 2" "Block 1 -> Block 2 root evolution"
    local addr before_raw before_leaves block2_hash after_raw after_leaves block2_header_root

    addr="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    before_raw="$(get_chainstate_commitment_raw)"
    before_leaves="$(rpc_scalar "blockchain.getutreexostats" "[]" '.num_leaves')"

    block2_hash="$(mine_blocks 1 "$addr" | head -n1)"
    after_raw="$(get_chainstate_commitment_raw)"
    after_leaves="$(rpc_scalar "blockchain.getutreexostats" "[]" '.num_leaves')"

    block2_header_root="$(get_header_utreexo_root "$block2_hash")"
    if [ "$before_raw" != "$after_raw" ]; then
        pass "Commitment changed after mining new block"
    else
        fail "Commitment did not change after mining"
        return 1
    fi

    if [ "$after_leaves" -gt "$before_leaves" ]; then
        pass "Leaf count increased: $before_leaves -> $after_leaves"
    else
        fail "Leaf count did not increase: $before_leaves -> $after_leaves"
        return 1
    fi

    if [ "$block2_header_root" == "$after_raw" ]; then
        pass "Block 2 header root matches chainstate"
    else
        fail "Block 2 mismatch: header=$block2_header_root chainstate=$after_raw"
        return 1
    fi
}

test_reorg() {
    print_test "TEST 3" "Reorg invalidate/remine consistency"
    local addr height_start height_after_mine tip_before_invalidate height_after_invalidate height_after_remine

    addr="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    height_start="$(rpc_scalar "getblockcount" "[]" '.')"
    mine_blocks 3 "$addr" >/dev/null
    height_after_mine="$(rpc_scalar "getblockcount" "[]" '.')"
    tip_before_invalidate="$(rpc_scalar "getbestblockhash" "[]" '.')"

    rpc_result "blockchain.invalidateblock" "[\"$tip_before_invalidate\"]" >/dev/null
    height_after_invalidate="$(rpc_scalar "getblockcount" "[]" '.')"

    if [ "$height_after_invalidate" -eq $((height_after_mine - 1)) ]; then
        pass "Invalidate dropped height by one"
    else
        fail "Unexpected invalidate height: $height_after_invalidate (expected $((height_after_mine - 1)))"
        return 1
    fi

    mine_blocks $((height_after_mine - height_after_invalidate)) "$addr" >/dev/null
    height_after_remine="$(rpc_scalar "getblockcount" "[]" '.')"

    if [ "$height_after_remine" -eq "$height_after_mine" ]; then
        pass "Height restored after remine"
    else
        fail "Height not restored: $height_after_remine (expected $height_after_mine)"
        return 1
    fi

    assert_tip_header_matches_chainstate
}

test_spend_transaction() {
    print_test "TEST 4" "Spend transaction updates root deterministically"
    local addr current_height blocks_needed before_raw dest send_result accepted txid after_raw

    addr="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    current_height="$(rpc_scalar "getblockcount" "[]" '.')"
    blocks_needed=$((110 - current_height))
    if [ "$blocks_needed" -gt 0 ]; then
        mine_blocks "$blocks_needed" "$addr" >/dev/null
    fi

    rpc_result "wallet.rescanblockchain" "[0]" >/dev/null || true

    before_raw="$(get_chainstate_commitment_raw)"
    dest="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    send_result="$(rpc_result "wallet.sendtoaddress" "[\"$dest\",1.0,\"\",\"\",true]")"
    accepted="$(echo "$send_result" | jq -r '.accepted // false')"
    txid="$(echo "$send_result" | jq -r '.txid // empty')"

    if [ "$accepted" != "true" ] || [ -z "$txid" ]; then
        fail "Failed to create/send spend transaction in test mode: $(echo "$send_result" | jq -c '.')"
        return 1
    fi
    pass "Spend tx accepted into mempool: ${txid:0:16}..."

    mine_blocks 1 "$addr" >/dev/null
    after_raw="$(get_chainstate_commitment_raw)"

    if [ "$before_raw" != "$after_raw" ]; then
        pass "Commitment changed after spend confirmation"
    else
        fail "Commitment unchanged after spend confirmation"
        return 1
    fi

    assert_tip_header_matches_chainstate
}

test_multi_transaction_block() {
    print_test "TEST 5" "Multi-transaction block/root coherence"
    local addr accepted_count before_raw after_raw tip_hash tip_ntx
    accepted_count=0

    addr="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
    before_raw="$(get_chainstate_commitment_raw)"

    for _ in 1 2 3; do
        local dest send_result accepted
        dest="$(rpc_scalar "wallet.getnewaddress" "[]" '.address')"
        send_result="$(rpc_result "wallet.sendtoaddress" "[\"$dest\",0.5,\"\",\"\",true]")"
        accepted="$(echo "$send_result" | jq -r '.accepted // false')"
        if [ "$accepted" == "true" ]; then
            ((accepted_count++))
        fi
    done

    if [ "$accepted_count" -eq 0 ]; then
        skip "No transactions accepted into mempool for multi-tx test"
        return 0
    fi
    pass "Accepted $accepted_count transactions into mempool"

    mine_blocks 1 "$addr" >/dev/null
    after_raw="$(get_chainstate_commitment_raw)"
    tip_hash="$(rpc_scalar "getbestblockhash" "[]" '.')"
    tip_ntx="$(rpc_scalar "getblock" "[\"$tip_hash\"]" '.nTx')"

    if [ "$before_raw" != "$after_raw" ]; then
        pass "Commitment changed after multi-tx mining"
    else
        fail "Commitment unchanged after multi-tx mining"
        return 1
    fi

    if [ "$tip_ntx" -gt 1 ]; then
        pass "Block includes mempool transactions (nTx=$tip_ntx)"
    else
        fail "Expected nTx > 1 after mempool fill, got nTx=$tip_ntx"
        return 1
    fi

    assert_tip_header_matches_chainstate
}

test_proof_verification() {
    print_test "TEST 6" "UTXO proof shape/invariant checks"
    local utxo txid vout proof position proof_size siblings_len num_leaves

    utxo="$(rpc_result "wallet.listunspent" "[]" | jq -c '.[0] // empty')"
    if [ -z "$utxo" ]; then
        skip "No UTXO available for proof check"
        return 0
    fi

    txid="$(echo "$utxo" | jq -r '.txid')"
    vout="$(echo "$utxo" | jq -r '.vout')"
    proof="$(rpc_result "blockchain.getutxoproof" "[\"$txid\",$vout]")"

    position="$(echo "$proof" | jq -r '.position')"
    proof_size="$(echo "$proof" | jq -r '.proof_size')"
    siblings_len="$(echo "$proof" | jq -r '.siblings | length')"
    num_leaves="$(echo "$proof" | jq -r '.num_leaves')"

    if [ "$proof_size" -eq "$siblings_len" ] && [ "$proof_size" -ge 0 ]; then
        pass "Proof shape valid (proof_size=$proof_size siblings=$siblings_len)"
    else
        fail "Proof shape invalid (proof_size=$proof_size siblings=$siblings_len)"
        return 1
    fi

    if [ "$position" -ge 0 ] && [ "$position" -lt "$num_leaves" ]; then
        pass "Proof position in valid range ($position < $num_leaves)"
    else
        fail "Proof position out of range: position=$position num_leaves=$num_leaves"
        return 1
    fi
}

test_persistence() {
    print_test "TEST 7" "Restart persistence (tip/leaves/commitment)"
    local h1 tip1 c1 leaves1 h2 tip2 c2 leaves2

    h1="$(rpc_scalar "getblockcount" "[]" '.')"
    tip1="$(rpc_scalar "getbestblockhash" "[]" '.')"
    c1="$(get_chainstate_commitment_raw)"
    leaves1="$(rpc_scalar "blockchain.getutreexostats" "[]" '.num_leaves')"

    stop_daemon
    "$DINEROD" \
        --regtest \
        --datadir="$DATADIR" \
        --rpcport="$PORT_RPC" \
        --port="$PORT_P2P" \
        --debug \
        > "$DATADIR/daemon_restart.log" 2>&1 &
    DAEMON_PID=$!
    wait_for_daemon

    h2="$(rpc_scalar "getblockcount" "[]" '.')"
    tip2="$(rpc_scalar "getbestblockhash" "[]" '.')"
    c2="$(get_chainstate_commitment_raw)"
    leaves2="$(rpc_scalar "blockchain.getutreexostats" "[]" '.num_leaves')"

    if [ "$h1" -eq "$h2" ]; then pass "Height persisted ($h2)"; else fail "Height mismatch ($h1 vs $h2)"; return 1; fi
    if [ "$tip1" == "$tip2" ]; then pass "Tip hash persisted"; else fail "Tip mismatch"; return 1; fi
    if [ "$c1" == "$c2" ]; then pass "Accumulator commitment persisted"; else fail "Commitment mismatch"; return 1; fi
    if [ "$leaves1" -eq "$leaves2" ]; then pass "Leaf count persisted ($leaves2)"; else fail "Leaf mismatch ($leaves1 vs $leaves2)"; return 1; fi

    assert_tip_header_matches_chainstate
}

main() {
    print_header "UTREEXO CONSENSUS VALIDATION SUITE"
    echo -e "${YELLOW}Regtest runtime checks for commitment coherence and persistence.${NC}"

    if [ ! -x "$DINEROD" ]; then
        echo -e "${RED}ERROR:${NC} dinerod not found at $DINEROD"
        exit 1
    fi
    if ! command -v jq >/dev/null 2>&1; then
        echo -e "${RED}ERROR:${NC} jq is required"
        exit 1
    fi

    start_daemon

    test_genesis_to_block1 || true
    test_block1_to_block2 || true
    test_reorg || true
    test_spend_transaction || true
    test_multi_transaction_block || true
    test_proof_verification || true
    test_persistence || true

    print_header "SUMMARY"
    echo -e "${GREEN}PASSED:${NC}  $TESTS_PASSED"
    echo -e "${RED}FAILED:${NC}  $TESTS_FAILED"
    echo -e "${YELLOW}SKIPPED:${NC} $TESTS_SKIPPED"

    if [ "$TESTS_FAILED" -eq 0 ]; then
        echo -e "${GREEN}Utreexo consensus suite PASSED.${NC}"
        exit 0
    fi

    echo -e "${RED}Utreexo consensus suite FAILED.${NC}"
    exit 1
}

main "$@"
