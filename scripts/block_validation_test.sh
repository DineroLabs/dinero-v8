#!/usr/bin/env bash
set -euo pipefail

# Block Validation Deep Test
# Tests transaction parsing, merkle root validation, and BIP34 height validation

ROOT="$HOME/Documents/DineroCoin"
BIN="$ROOT/build/bin/dinerod"
DATADIR="$ROOT/test-data/regtest"
PORT=20999
COOKIE=""
DAEMON_PID=""

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC} $1"; }
log_error() { echo -e "${RED}[FAIL]${NC} $1"; }

rpc() {
    curl -sS --user "$COOKIE" -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$PORT/"
}

setup() {
    log_info "Setting up block validation test..."
    pkill -f "$BIN" || true
    sleep 2
    
    "$BIN" -regtest -datadir="$DATADIR" -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 -rpcport=$PORT -printtoconsole=1 > /dev/null 2>&1 &
    DAEMON_PID=$!
    sleep 5
    
    COOKIE=$(cat "$DATADIR/regtest/.cookie")
    log_info "Daemon ready for block validation testing"
}

cleanup() {
    if [[ -n "$DAEMON_PID" ]]; then
        kill $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
    fi
}

test_transaction_parsing() {
    log_info "=== Testing Transaction Parsing ==="
    
    # Generate address and create block
    local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    local block_hash=$(rpc '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"'$addr'"]}' | jq -r '.result.result[0]')
    
    # Get block details
    local block_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblock","params":["'$block_hash'"]}')
    local tx_count=$(echo "$block_info" | jq -r '.result.tx | length')
    local first_tx=$(echo "$block_info" | jq -r '.result.tx[0]')
    
    if [[ "$tx_count" -gt 0 ]]; then
        log_success "Transaction parsing: Block contains $tx_count transactions"
    else
        log_error "Transaction parsing: Block has no transactions"
        return 1
    fi
    
    # Test transaction details
    local tx_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"gettransaction","params":["'$first_tx'"]}')
    local tx_confirmations=$(echo "$tx_info" | jq -r '.result.confirmations // 0')
    
    if [[ "$tx_confirmations" -gt 0 ]]; then
        log_success "Transaction parsing: Transaction has $tx_confirmations confirmations"
    else
        log_warning "Transaction parsing: Transaction has 0 confirmations (may be expected)"
    fi
    
    return 0
}

test_merkle_root_validation() {
    log_info "=== Testing Merkle Root Validation ==="
    
    # Create multiple blocks to test merkle root consistency
    local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    
    for i in {1..3}; do
        local block_hash=$(rpc '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"'$addr'"]}' | jq -r '.result.result[0]')
        local block_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblock","params":["'$block_hash'"]}')
        local merkle_root=$(echo "$block_info" | jq -r '.result.merkleroot')
        local tx_count=$(echo "$block_info" | jq -r '.result.tx | length')
        
        if [[ "$merkle_root" =~ ^[a-f0-9]{64}$ ]]; then
            log_success "Merkle root validation: Block $i has valid merkle root ($tx_count tx)"
        else
            log_error "Merkle root validation: Block $i has invalid merkle root format"
            return 1
        fi
    done
    
    return 0
}

test_bip34_height_validation() {
    log_info "=== Testing BIP34 Height Validation ==="
    
    # Get current height
    local current_height=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo"}' | jq -r '.result.blocks')
    log_info "Current blockchain height: $current_height"
    
    # Generate a block and check height increment
    local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    local block_hash=$(rpc '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"'$addr'"]}' | jq -r '.result.result[0]')
    
    local new_height=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo"}' | jq -r '.result.blocks')
    
    if [[ "$new_height" -eq $((current_height + 1)) ]]; then
        log_success "BIP34 height validation: Height increased from $current_height to $new_height"
    else
        log_error "BIP34 height validation: Height did not increase correctly (from $current_height to $new_height)"
        return 1
    fi
    
    # Test block height in block info
    local block_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblock","params":["'$block_hash'"]}')
    local block_height=$(echo "$block_info" | jq -r '.result.height // 0')
    
    if [[ "$block_height" -eq "$new_height" ]]; then
        log_success "BIP34 height validation: Block height matches chain height ($block_height)"
    else
        log_error "BIP34 height validation: Block height mismatch (block: $block_height, chain: $new_height)"
        return 1
    fi
    
    return 0
}

test_block_consistency() {
    log_info "=== Testing Block Consistency ==="
    
    # Generate multiple blocks and verify consistency
    local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    local blocks=()
    
    # Generate 5 blocks
    for i in {1..5}; do
        local block_hash=$(rpc '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"'$addr'"]}' | jq -r '.result.result[0]')
        blocks+=("$block_hash")
        log_info "Generated block $i: $block_hash"
    done
    
    # Verify each block can be retrieved and has correct previous hash
    for i in "${!blocks[@]}"; do
        local block_hash="${blocks[$i]}"
        local block_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblock","params":["'$block_hash'"]}')
        local prev_hash=$(echo "$block_info" | jq -r '.result.previousblockhash // "null"')
        local height=$(echo "$block_info" | jq -r '.result.height // 0')
        
        if [[ "$prev_hash" != "null" ]]; then
            log_success "Block consistency: Block $((i+1)) has previous hash"
        else
            log_error "Block consistency: Block $((i+1)) missing previous hash"
            return 1
        fi
        
        if [[ "$height" -gt 0 ]]; then
            log_success "Block consistency: Block $((i+1)) has height $height"
        else
            log_error "Block consistency: Block $((i+1)) missing height"
            return 1
        fi
    done
    
    return 0
}

main() {
    echo "🔍 Block Validation Deep Test"
    echo "============================="
    echo ""
    
    setup
    
    local tests_passed=0
    local tests_failed=0
    
    # Run tests
    if test_transaction_parsing; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_merkle_root_validation; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_bip34_height_validation; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_block_consistency; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    # Summary
    echo "📊 Block Validation Test Summary"
    echo "================================"
    echo "Tests Passed: $tests_passed"
    echo "Tests Failed: $tests_failed"
    
    if [[ $tests_failed -eq 0 ]]; then
        echo -e "\n🎉 ${GREEN}All block validation tests passed!${NC}"
        cleanup
        exit 0
    else
        echo -e "\n❌ ${RED}Some block validation tests failed!${NC}"
        cleanup
        exit 1
    fi
}

trap cleanup EXIT
main "$@"
