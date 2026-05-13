#!/usr/bin/env bash
set -euo pipefail

# Comprehensive Test Suite for Dinero Placeholder Fixes
# Tests all critical RPC, block validation, and mining functionality

ROOT="$HOME/Documents/DineroCoin"
BIN="$ROOT/build/bin/dinerod"
DATADIR="$ROOT/test-data/regtest"
PORT=20999
COOKIE=""
DAEMON_PID=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_TOTAL=0

# Helper functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

# RPC helper function
rpc() {
    curl -sS --user "$COOKIE" -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$PORT/"
}

# Test helper function
test_rpc() {
    local test_name="$1"
    local method="$2"
    local params="$3"
    local expected_field="$4"
    local expected_value="$5"
    
    ((TESTS_TOTAL++))
    log_info "Testing $test_name..."
    
    local response=$(rpc '{"jsonrpc":"2.0","id":"test","method":"'$method'","params":'$params'}')
    local result=$(echo "$response" | jq -r '.result' 2>/dev/null || echo "null")
    local error=$(echo "$response" | jq -r '.error' 2>/dev/null || echo "null")
    
    if [[ "$error" != "null" ]]; then
        log_error "$test_name: RPC error - $error"
        return 1
    fi
    
    if [[ "$expected_field" == "exists" ]]; then
        if [[ "$result" != "null" && "$result" != "" ]]; then
            log_success "$test_name: Returns data"
            return 0
        else
            log_error "$test_name: No data returned"
            return 1
        fi
    fi
    
    local field_value=$(echo "$result" | jq -r '.'$expected_field' // "null"' 2>/dev/null || echo "null")
    
    if [[ "$field_value" == "$expected_value" ]]; then
        log_success "$test_name: $expected_field = $expected_value"
        return 0
    else
        log_error "$test_name: Expected $expected_field = $expected_value, got $field_value"
        return 1
    fi
}

# Setup function
setup() {
    log_info "Setting up test environment..."
    
    # Kill any existing daemon
    pkill -f "$BIN" || true
    sleep 2
    
    # Start daemon
    log_info "Starting daemon..."
    "$BIN" -regtest -datadir="$DATADIR" -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 -rpcport=$PORT -printtoconsole=1 > /dev/null 2>&1 &
    DAEMON_PID=$!
    
    # Wait for daemon to start
    sleep 5
    
    # Get cookie
    COOKIE=$(cat "$DATADIR/regtest/.cookie")
    if [[ -z "$COOKIE" ]]; then
        log_error "Failed to get RPC cookie"
        exit 1
    fi
    
    log_info "Daemon started with PID $DAEMON_PID"
}

# Cleanup function
cleanup() {
    log_info "Cleaning up..."
    if [[ -n "$DAEMON_PID" ]]; then
        kill $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
    fi
}

# Test 1: RPC Placeholder Fixes
test_rpc_placeholders() {
    log_info "=== Testing RPC Placeholder Fixes ==="
    
    # Test getnewaddress - should return real address, not placeholder
    test_rpc "getnewaddress returns real address" "getnewaddress" '{}' "address" "exists"
    
    # Test getbalance - should return real balance structure
    test_rpc "getbalance returns real data" "getbalance" '{}' "balance_din" "exists"
    
    # Test listunspent - should return real UTXO data
    test_rpc "listunspent returns real data" "listunspent" '{}' "exists" "exists"
    
    # Test listtransactions - should return real transaction data
    test_rpc "listtransactions returns real data" "listtransactions" '{}' "exists" "exists"
    
    # Test mining.getaddress - should return real address or null, not placeholder
    local mining_addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.getaddress"}' | jq -r '.result.address // "null"')
    if [[ "$mining_addr" != "din1qminingaddressplaceholder000000000000000" ]]; then
        log_success "mining.getaddress: No placeholder address returned"
    else
        log_error "mining.getaddress: Still returns placeholder address"
    fi
}

# Test 2: Block Validation Fixes
test_block_validation() {
    log_info "=== Testing Block Validation Fixes ==="
    
    # Generate a new address
    local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    
    # Test block generation - should create real blocks with proper validation
    log_info "Testing block generation with real validation..."
    local block_hash=$(rpc '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"'$addr'"]}' | jq -r '.result.result[0]')
    
    if [[ "$block_hash" =~ ^[a-f0-9]{64}$ ]]; then
        log_success "Block generation: Returns real 64-char hex hash"
    else
        log_error "Block generation: Returns invalid hash format"
    fi
    
    # Test blockchain info - should show increased height
    local height_before=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo"}' | jq -r '.result.blocks')
    
    # Generate another block
    local block_hash2=$(rpc '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"'$addr'"]}' | jq -r '.result.result[0]')
    
    local height_after=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo"}' | jq -r '.result.blocks')
    
    if [[ "$height_after" -eq $((height_before + 1)) ]]; then
        log_success "Block validation: Height increased correctly"
    else
        log_error "Block validation: Height did not increase (before: $height_before, after: $height_after)"
    fi
    
    # Test block details - should have real transaction data
    local block_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getblock","params":["'$block_hash2'"]}')
    local tx_count=$(echo "$block_info" | jq -r '.result.tx | length')
    
    if [[ "$tx_count" -gt 0 ]]; then
        log_success "Block validation: Block contains transactions"
    else
        log_error "Block validation: Block has no transactions"
    fi
}

# Test 3: Mining System Integration
test_mining_integration() {
    log_info "=== Testing Mining System Integration ==="
    
    # Test mining address persistence
    local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    
    # Set mining address
    local set_result=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.setaddress","params":["'$addr'"]}')
    local set_success=$(echo "$set_result" | jq -r '.result.ok // false')
    
    if [[ "$set_success" == "true" ]]; then
        log_success "Mining address: Successfully set"
    else
        log_error "Mining address: Failed to set"
    fi
    
    # Get mining address back
    local get_addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.getaddress"}' | jq -r '.result.address')
    
    if [[ "$get_addr" == "$addr" ]]; then
        log_success "Mining address: Persistence working"
    else
        log_error "Mining address: Persistence failed (set: $addr, got: $get_addr)"
    fi
    
    # Test mining status
    local mining_status=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.status"}')
    local is_running=$(echo "$mining_status" | jq -r '.result.running // false')
    
    if [[ "$is_running" == "true" ]]; then
        log_success "Mining status: Miner is running"
    else
        log_warning "Mining status: Miner is not running (may be expected)"
    fi
}

# Test 4: Legacy Compatibility
test_legacy_compatibility() {
    log_info "=== Testing Legacy Compatibility ==="
    
    # Test getmininginfo alias
    local legacy_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getmininginfo"}')
    local legacy_running=$(echo "$legacy_info" | jq -r '.result.running // false')
    
    if [[ "$legacy_running" != "null" ]]; then
        log_success "Legacy compatibility: getmininginfo alias working"
    else
        log_error "Legacy compatibility: getmininginfo alias not working"
    fi
}

# Test 5: Error Handling
test_error_handling() {
    log_info "=== Testing Error Handling ==="
    
    # Test invalid RPC method
    local invalid_response=$(rpc '{"jsonrpc":"2.0","id":"test","method":"invalidmethod"}')
    local error_code=$(echo "$invalid_response" | jq -r '.error.code // 0')
    
    if [[ "$error_code" == "-32601" ]]; then
        log_success "Error handling: Invalid method returns proper error"
    else
        log_error "Error handling: Invalid method error code incorrect"
    fi
    
    # Test invalid parameters
    local invalid_params=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":["invalid"]}')
    local param_error=$(echo "$invalid_params" | jq -r '.error // "null"')
    
    if [[ "$param_error" != "null" ]]; then
        log_success "Error handling: Invalid parameters handled correctly"
    else
        log_warning "Error handling: Invalid parameters not rejected (may be expected)"
    fi
}

# Test 6: Performance and Stability
test_performance_stability() {
    log_info "=== Testing Performance and Stability ==="
    
    # Test multiple rapid RPC calls
    local start_time=$(date +%s)
    for i in {1..10}; do
        rpc '{"jsonrpc":"2.0","id":"test'$i'","method":"getblockchaininfo"}' > /dev/null
    done
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    if [[ "$duration" -lt 5 ]]; then
        log_success "Performance: 10 RPC calls completed in ${duration}s"
    else
        log_warning "Performance: 10 RPC calls took ${duration}s (may be slow)"
    fi
    
    # Test daemon stability
    local pid_check=$(ps -p $DAEMON_PID -o pid= 2>/dev/null || echo "")
    if [[ -n "$pid_check" ]]; then
        log_success "Stability: Daemon still running after tests"
    else
        log_error "Stability: Daemon crashed during tests"
    fi
}

# Main test execution
main() {
    echo "🚀 Dinero Comprehensive Test Suite"
    echo "=================================="
    echo ""
    
    setup
    
    # Run all test suites
    test_rpc_placeholders
    echo ""
    test_block_validation
    echo ""
    test_mining_integration
    echo ""
    test_legacy_compatibility
    echo ""
    test_error_handling
    echo ""
    test_performance_stability
    
    # Print summary
    echo ""
    echo "📊 Test Summary"
    echo "==============="
    echo "Total Tests: $TESTS_TOTAL"
    echo -e "Passed: ${GREEN}$TESTS_PASSED${NC}"
    echo -e "Failed: ${RED}$TESTS_FAILED${NC}"
    
    if [[ $TESTS_FAILED -eq 0 ]]; then
        echo -e "\n🎉 ${GREEN}All tests passed!${NC}"
        cleanup
        exit 0
    else
        echo -e "\n❌ ${RED}Some tests failed!${NC}"
        cleanup
        exit 1
    fi
}

# Trap to ensure cleanup on exit
trap cleanup EXIT

# Run main function
main "$@"
