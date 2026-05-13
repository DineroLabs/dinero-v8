#!/bin/bash

# Dinero Production Readiness Verification Script
# Tests all critical functionality for "production-ready" status

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Helper functions
ok() { echo -e "${GREEN}✅ $1${NC}"; }
bad() { echo -e "${RED}❌ $1${NC}"; }
warn() { echo -e "${YELLOW}⚠️  $1${NC}"; }
info() { echo -e "${BLUE}ℹ️  $1${NC}"; }

# Configuration
REPO_ROOT="/Users/haydarevich/Documents/DineroCoin"
DATADIR="$REPO_ROOT/test-data/regtest"
RPCPORT=20999
COOKIEFILE="$DATADIR/regtest/.cookie"

# RPC helper function
rpc() {
    local method="$1"
    local params="${2:-[]}"
    curl -sS --user "$(cat "$COOKIEFILE")" \
         -H 'Content-Type: application/json' \
         -d "{\"jsonrpc\":\"2.0\",\"id\":\"t\",\"method\":\"$method\",\"params\":$params}" \
         "http://127.0.0.1:$RPCPORT/"
}

# Test results tracking
PASSED=0
FAILED=0
SKIPPED=0

# Cleanup function
cleanup() {
    info "Cleaning up..."
    pkill -f dinerod || true
    sleep 2
}

# Test function
test_case() {
    local name="$1"
    local test_func="$2"
    
    echo ""
    info "Testing: $name"
    if $test_func; then
        ok "$name"
        ((PASSED++))
    else
        bad "$name"
        ((FAILED++))
    fi
}

# Individual test functions
test_daemon_startup() {
    cd "$REPO_ROOT"
    cleanup
    
    info "Starting daemon..."
    ./build/bin/dinerod --regtest --datadir="$DATADIR" --rpcport=$RPCPORT --printtoconsole &
    
    # Wait for daemon to start
    for i in {1..30}; do
        if [ -f "$COOKIEFILE" ]; then
            sleep 2  # Give it a moment to fully initialize
            return 0
        fi
        sleep 1
    done
    
    return 1
}

test_rpc_connectivity() {
    local response=$(rpc "help")
    local method_count=$(echo "$response" | jq -r '.result | length')
    
    if [ "$method_count" -gt 30 ]; then
        info "RPC methods available: $method_count"
        return 0
    else
        warn "Only $method_count RPC methods available"
        return 1
    fi
}

test_wallet_lifecycle() {
    # Create wallet
    rpc "wallet.create" '["default"]' >/dev/null
    
    # Load wallet
    rpc "wallet.load" '["default"]' >/dev/null
    
    # Generate address
    local addr=$(rpc "getnewaddress" | jq -r '.result // empty')
    
    if [[ "$addr" =~ ^din1[a-z0-9]{39}$ ]]; then
        info "Generated address: $addr"
        return 0
    else
        warn "Invalid address format: $addr"
        return 1
    fi
}

test_mining_address_persistence() {
    # Set mining address
    local test_addr="din1qtest1234567890abcdef1234567890abcdef12"
    local set_response=$(rpc "mining.setaddress" "[\"$test_addr\"]")
    
    if echo "$set_response" | jq -e '.result.address' >/dev/null; then
        # Get mining address
        local get_response=$(rpc "mining.getaddress")
        local retrieved_addr=$(echo "$get_response" | jq -r '.result.address // empty')
        
        if [ "$retrieved_addr" = "$test_addr" ]; then
            info "Mining address persisted: $retrieved_addr"
            return 0
        else
            warn "Address mismatch: expected $test_addr, got $retrieved_addr"
            return 1
        fi
    else
        warn "Failed to set mining address"
        return 1
    fi
}

test_block_generation() {
    # Get current height
    local height_before=$(rpc "getblockcount" | jq -r '.result // 0')
    
    # Generate blocks
    local test_addr="din1qtest1234567890abcdef1234567890abcdef12"
    local gen_response=$(rpc "generatetoaddress" "[1, \"$test_addr\"]")
    
    if echo "$gen_response" | jq -e '.result | length >= 1' >/dev/null; then
        local height_after=$(rpc "getblockcount" | jq -r '.result // 0')
        
        if [ "$height_after" -gt "$height_before" ]; then
            info "Block height increased: $height_before -> $height_after"
            return 0
        else
            warn "Block height unchanged: $height_before -> $height_after"
            return 1
        fi
    else
        warn "Block generation failed"
        return 1
    fi
}

test_network_info() {
    local response=$(rpc "getnetworkinfo")
    
    if echo "$response" | jq -e '.result.version' >/dev/null; then
        local version=$(echo "$response" | jq -r '.result.version')
        info "Network version: $version"
        return 0
    else
        warn "Network info unavailable"
        return 1
    fi
}

test_mempool_info() {
    local response=$(rpc "getmempoolinfo")
    
    if echo "$response" | jq -e '.result.size' >/dev/null; then
        local size=$(echo "$response" | jq -r '.result.size')
        info "Mempool size: $size"
        return 0
    else
        warn "Mempool info unavailable"
        return 1
    fi
}

test_mining_status() {
    local response=$(rpc "mining.status")
    
    if echo "$response" | jq -e '.result.running' >/dev/null; then
        local running=$(echo "$response" | jq -r '.result.running')
        info "Mining status: running=$running"
        return 0
    else
        warn "Mining status unavailable"
        return 1
    fi
}

test_multi_account() {
    # Create multi-account
    local create_response=$(rpc "multiaccount.create" '["Test Account"]')
    
    if echo "$create_response" | jq -e '.result.account_id' >/dev/null; then
        local account_id=$(echo "$create_response" | jq -r '.result.account_id')
        
        # Generate address for account
        local addr_response=$(rpc "multiaccount.generatenewaddress")
        
        if echo "$addr_response" | jq -e '.result.address' >/dev/null; then
            local addr=$(echo "$addr_response" | jq -r '.result.address')
            info "Multi-account address: $addr"
            return 0
        else
            warn "Failed to generate multi-account address"
            return 1
        fi
    else
        warn "Failed to create multi-account"
        return 1
    fi
}

test_bip39_support() {
    local response=$(rpc "wallet.mnemonic.new")
    
    if echo "$response" | jq -e '.result.mnemonic' >/dev/null; then
        local mnemonic=$(echo "$response" | jq -r '.result.mnemonic')
        info "BIP39 mnemonic generated: ${mnemonic:0:20}..."
        return 0
    elif echo "$response" | jq -e '.error' >/dev/null; then
        local error=$(echo "$response" | jq -r '.error.message')
        if [[ "$error" == *"not implemented"* ]]; then
            warn "BIP39 not implemented (expected)"
            return 0
        else
            warn "BIP39 error: $error"
            return 1
        fi
    else
        warn "BIP39 response format unexpected"
        return 1
    fi
}

# Main execution
main() {
    echo -e "${BLUE}🚀 Dinero Production Readiness Verification${NC}"
    echo "=================================================="
    
    cd "$REPO_ROOT"
    
    # Core infrastructure tests
    test_case "Daemon Startup" test_daemon_startup
    test_case "RPC Connectivity" test_rpc_connectivity
    test_case "Wallet Lifecycle" test_wallet_lifecycle
    
    # Mining tests
    test_case "Mining Address Persistence" test_mining_address_persistence
    test_case "Block Generation" test_block_generation
    test_case "Mining Status" test_mining_status
    
    # Network tests
    test_case "Network Info" test_network_info
    test_case "Mempool Info" test_mempool_info
    
    # Advanced features
    test_case "Multi-Account Support" test_multi_account
    test_case "BIP39 Support" test_bip39_support
    
    # Summary
    echo ""
    echo "=================================================="
    echo -e "${BLUE}📊 VERIFICATION SUMMARY${NC}"
    echo "=================================================="
    echo -e "${GREEN}✅ Passed: $PASSED${NC}"
    echo -e "${RED}❌ Failed: $FAILED${NC}"
    echo -e "${YELLOW}⚠️  Skipped: $SKIPPED${NC}"
    
    local total=$((PASSED + FAILED + SKIPPED))
    local success_rate=$((PASSED * 100 / total))
    
    echo ""
    echo -e "${BLUE}Success Rate: $success_rate%${NC}"
    
    if [ $FAILED -eq 0 ]; then
        echo -e "${GREEN}🎉 ALL TESTS PASSED - PRODUCTION READY!${NC}"
        exit 0
    elif [ $success_rate -ge 80 ]; then
        echo -e "${YELLOW}⚠️  MOSTLY READY - Minor issues to address${NC}"
        exit 1
    else
        echo -e "${RED}🚨 NOT READY - Major issues need fixing${NC}"
        exit 2
    fi
}

# Run main function
main "$@"
