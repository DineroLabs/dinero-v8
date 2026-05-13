#!/usr/bin/env bash
set -euo pipefail

# RPC Placeholder Fixes Test
# Tests that all RPC methods return real data instead of placeholders

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
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC} $1"; }
log_error() { echo -e "${RED}[FAIL]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARN]${NC} $1"; }

rpc() {
    curl -sS --user "$COOKIE" -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$PORT/"
}

setup() {
    log_info "Setting up RPC placeholder test..."
    pkill -f "$BIN" || true
    sleep 2
    
    "$BIN" -regtest -datadir="$DATADIR" -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 -rpcport=$PORT -printtoconsole=1 > /dev/null 2>&1 &
    DAEMON_PID=$!
    sleep 5
    
    COOKIE=$(cat "$DATADIR/regtest/.cookie")
    log_info "Daemon ready for RPC testing"
}

cleanup() {
    if [[ -n "$DAEMON_PID" ]]; then
        kill $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
    fi
}

test_getnewaddress() {
    log_info "=== Testing getnewaddress ==="
    
    # Test multiple address generation
    local addresses=()
    for i in {1..5}; do
        local addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
        addresses+=("$addr")
        
        # Check if address is real (not placeholder)
        if [[ "$addr" =~ ^din1q[a-z0-9]{39,43}$ ]]; then
            log_success "getnewaddress: Generated valid address $i: $addr"
        else
            log_error "getnewaddress: Invalid address format: $addr"
            return 1
        fi
    done
    
    # Check that addresses are unique
    local unique_count=$(printf '%s\n' "${addresses[@]}" | sort -u | wc -l)
    if [[ "$unique_count" -eq 5 ]]; then
        log_success "getnewaddress: All 5 addresses are unique"
    else
        log_error "getnewaddress: Addresses are not unique ($unique_count unique out of 5)"
        return 1
    fi
    
    return 0
}

test_getbalance() {
    log_info "=== Testing getbalance ==="
    
    local balance_response=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getbalance"}')
    local balance_din=$(echo "$balance_response" | jq -r '.result.balance_din // "null"')
    local balance_una=$(echo "$balance_response" | jq -r '.result.balance_una // "null"')
    
    # Check if balance structure is real
    if [[ "$balance_din" != "null" && "$balance_una" != "null" ]]; then
        log_success "getbalance: Returns real balance structure (DIN: $balance_din, UNA: $balance_una)"
    else
        log_error "getbalance: Missing balance fields"
        return 1
    fi
    
    # Check if balance is numeric
    if [[ "$balance_din" =~ ^[0-9]+\.?[0-9]*$ ]]; then
        log_success "getbalance: Balance DIN is numeric"
    else
        log_error "getbalance: Balance DIN is not numeric: $balance_din"
        return 1
    fi
    
    return 0
}

test_listunspent() {
    log_info "=== Testing listunspent ==="
    
    local unspent_response=$(rpc '{"jsonrpc":"2.0","id":"test","method":"listunspent"}')
    local unspent_count=$(echo "$unspent_response" | jq -r '.result | length')
    
    if [[ "$unspent_count" -ge 0 ]]; then
        log_success "listunspent: Returns array with $unspent_count UTXOs"
    else
        log_error "listunspent: Invalid response format"
        return 1
    fi
    
    # If there are UTXOs, check their structure
    if [[ "$unspent_count" -gt 0 ]]; then
        local first_utxo=$(echo "$unspent_response" | jq -r '.result[0]')
        local has_address=$(echo "$first_utxo" | jq -r 'has("address")')
        local has_amount=$(echo "$first_utxo" | jq -r 'has("amount")')
        
        if [[ "$has_address" == "true" && "$has_amount" == "true" ]]; then
            log_success "listunspent: UTXO has proper structure (address, amount)"
        else
            log_error "listunspent: UTXO missing required fields"
            return 1
        fi
    else
        log_warning "listunspent: No UTXOs found (may be expected for new wallet)"
    fi
    
    return 0
}

test_listtransactions() {
    log_info "=== Testing listtransactions ==="
    
    local tx_response=$(rpc '{"jsonrpc":"2.0","id":"test","method":"listtransactions"}')
    local tx_count=$(echo "$tx_response" | jq -r '.result | length')
    
    if [[ "$tx_count" -ge 0 ]]; then
        log_success "listtransactions: Returns array with $tx_count transactions"
    else
        log_error "listtransactions: Invalid response format"
        return 1
    fi
    
    # If there are transactions, check their structure
    if [[ "$tx_count" -gt 0 ]]; then
        local first_tx=$(echo "$tx_response" | jq -r '.result[0]')
        local has_txid=$(echo "$first_tx" | jq -r 'has("txid")')
        local has_amount=$(echo "$first_tx" | jq -r 'has("amount")')
        
        if [[ "$has_txid" == "true" && "$has_amount" == "true" ]]; then
            log_success "listtransactions: Transaction has proper structure (txid, amount)"
        else
            log_error "listtransactions: Transaction missing required fields"
            return 1
        fi
    else
        log_warning "listtransactions: No transactions found (may be expected for new wallet)"
    fi
    
    return 0
}

test_mining_address() {
    log_info "=== Testing Mining Address ==="
    
    # Test getting mining address (should not be placeholder)
    local mining_addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.getaddress"}' | jq -r '.result.address // "null"')
    
    if [[ "$mining_addr" != "din1qminingaddressplaceholder000000000000000" ]]; then
        log_success "mining.getaddress: No placeholder address returned"
    else
        log_error "mining.getaddress: Still returns placeholder address"
        return 1
    fi
    
    # Test setting mining address
    local test_addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress"}' | jq -r '.result.address')
    local set_result=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.setaddress","params":["'$test_addr'"]}')
    local set_success=$(echo "$set_result" | jq -r '.result.ok // false')
    
    if [[ "$set_success" == "true" ]]; then
        log_success "mining.setaddress: Successfully set address"
    else
        log_error "mining.setaddress: Failed to set address"
        return 1
    fi
    
    # Test getting the set address back
    local get_addr=$(rpc '{"jsonrpc":"2.0","id":"test","method":"mining.getaddress"}' | jq -r '.result.address')
    
    if [[ "$get_addr" == "$test_addr" ]]; then
        log_success "mining.getaddress: Persistence working correctly"
    else
        log_error "mining.getaddress: Persistence failed (set: $test_addr, got: $get_addr)"
        return 1
    fi
    
    return 0
}

test_legacy_aliases() {
    log_info "=== Testing Legacy Aliases ==="
    
    # Test getmininginfo alias
    local legacy_info=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getmininginfo"}')
    local legacy_running=$(echo "$legacy_info" | jq -r '.result.running // "null"')
    
    if [[ "$legacy_running" != "null" ]]; then
        log_success "getmininginfo: Legacy alias working (running: $legacy_running)"
    else
        log_error "getmininginfo: Legacy alias not working"
        return 1
    fi
    
    return 0
}

test_error_handling() {
    log_info "=== Testing Error Handling ==="
    
    # Test invalid method
    local invalid_response=$(rpc '{"jsonrpc":"2.0","id":"test","method":"invalidmethod"}')
    local error_code=$(echo "$invalid_response" | jq -r '.error.code // 0')
    
    if [[ "$error_code" == "-32601" ]]; then
        log_success "Error handling: Invalid method returns proper error code"
    else
        log_error "Error handling: Invalid method error code incorrect: $error_code"
        return 1
    fi
    
    # Test invalid parameters
    local invalid_params=$(rpc '{"jsonrpc":"2.0","id":"test","method":"getnewaddress","params":["invalid"]}')
    local param_error=$(echo "$invalid_params" | jq -r '.error // "null"')
    
    if [[ "$param_error" != "null" ]]; then
        log_success "Error handling: Invalid parameters handled correctly"
    else
        log_warning "Error handling: Invalid parameters not rejected (may be expected)"
    fi
    
    return 0
}

main() {
    echo "🔧 RPC Placeholder Fixes Test"
    echo "============================="
    echo ""
    
    setup
    
    local tests_passed=0
    local tests_failed=0
    
    # Run tests
    if test_getnewaddress; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_getbalance; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_listunspent; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_listtransactions; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_mining_address; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_legacy_aliases; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    if test_error_handling; then
        ((tests_passed++))
    else
        ((tests_failed++))
    fi
    echo ""
    
    # Summary
    echo "📊 RPC Placeholder Test Summary"
    echo "==============================="
    echo "Tests Passed: $tests_passed"
    echo "Tests Failed: $tests_failed"
    
    if [[ $tests_failed -eq 0 ]]; then
        echo -e "\n🎉 ${GREEN}All RPC placeholder tests passed!${NC}"
        cleanup
        exit 0
    else
        echo -e "\n❌ ${RED}Some RPC placeholder tests failed!${NC}"
        cleanup
        exit 1
    fi
}

trap cleanup EXIT
main "$@"
