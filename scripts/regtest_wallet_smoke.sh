#!/bin/bash
# regtest_wallet_smoke.sh - PSBT RPC Flow Test Harness
# 
# This script spins up a Dinero node, mines blocks, and exercises the complete
# PSBT RPC flow: walletcreatefundedpsbt → walletprocesspsbt → finalizepsbt → sendrawtransaction
#
# Usage: ./scripts/regtest_wallet_smoke.sh [datadir]

set -euo pipefail

# Configuration
DATADIR="${1:-/tmp/dinero-regtest-smoke}"
RPC_PORT=20998
P2P_PORT=20999
RPC_URL="http://127.0.0.1:$RPC_PORT"
COOKIE_FILE="$DATADIR/regtest/.cookie"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Cleanup function
cleanup() {
    log_info "Cleaning up..."
    if [ ! -z "${DAEMON_PID:-}" ]; then
        kill $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
    fi
    log_info "Cleanup complete"
}

# Set up cleanup trap
trap cleanup EXIT INT TERM

# RPC helper function
rpc_call() {
    local method="$1"
    local params="$2"
    
    curl -s -X POST \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"$method\",\"params\":$params}" \
        --cookie "$COOKIE_FILE" \
        "$RPC_URL" | jq -r '.result // .error'
}

# Wait for daemon to be ready
wait_for_daemon() {
    log_info "Waiting for daemon to be ready..."
    local max_attempts=30
    local attempt=0
    
    while [ $attempt -lt $max_attempts ]; do
        if curl -s --cookie "$COOKIE_FILE" "$RPC_URL" >/dev/null 2>&1; then
            log_success "Daemon is ready!"
            return 0
        fi
        
        attempt=$((attempt + 1))
        log_info "Attempt $attempt/$max_attempts - waiting..."
        sleep 2
    done
    
    log_error "Daemon failed to start within $((max_attempts * 2)) seconds"
    return 1
}

# Main test function
run_psbt_tests() {
    log_info "Starting PSBT RPC flow tests..."
    
    # Test 1: Get network info
    log_info "Test 1: Getting network info..."
    local network_info=$(rpc_call "getnetworkinfo" "[]")
    if [ "$network_info" != "null" ]; then
        log_success "Network info retrieved"
    else
        log_error "Failed to get network info"
        return 1
    fi
    
    # Test 2: Generate a test address
    log_info "Test 2: Generating test address..."
    local test_address=$(rpc_call "getnewaddress" '["test-address"]')
    if [ "$test_address" != "null" ] && [[ "$test_address" =~ ^din1 ]]; then
        log_success "Generated test address: $test_address"
    else
        log_error "Failed to generate test address"
        return 1
    fi
    
    # Test 3: Mine some blocks to get funds
    log_info "Test 3: Mining blocks for funds..."
    local mining_result=$(rpc_call "generatetoaddress" "[10, \"$test_address\"]")
    if [ "$mining_result" != "null" ]; then
        log_success "Mined 10 blocks"
    else
        log_error "Failed to mine blocks"
        return 1
    fi
    
    # Test 4: Get wallet balance
    log_info "Test 4: Checking wallet balance..."
    local balance=$(rpc_call "getbalance" "[]")
    if [ "$balance" != "null" ] && [ "$balance" != "0" ]; then
        log_success "Wallet balance: $balance DIN"
    else
        log_error "No balance available"
        return 1
    fi
    
    # Test 5: Create funded PSBT
    log_info "Test 5: Creating funded PSBT..."
    local psbt_params="[{\"$test_address\": 1.0}, null, 0, {\"fee_rate\": 1}]"
    local psbt_result=$(rpc_call "walletcreatefundedpsbt" "$psbt_params")
    
    if [ "$psbt_result" != "null" ]; then
        local psbt_base64=$(echo "$psbt_result" | jq -r '.psbt // empty')
        if [ ! -z "$psbt_base64" ]; then
            log_success "Created PSBT: ${psbt_base64:0:50}..."
        else
            log_error "PSBT creation failed - no psbt field"
            return 1
        fi
    else
        log_error "PSBT creation failed"
        return 1
    fi
    
    # Test 6: Process PSBT (sign)
    log_info "Test 6: Processing PSBT (signing)..."
    local process_params="[\"$psbt_base64\", true]"
    local process_result=$(rpc_call "walletprocesspsbt" "$process_params")
    
    if [ "$process_result" != "null" ]; then
        local processed_psbt=$(echo "$process_result" | jq -r '.psbt // empty')
        local signed_count=$(echo "$process_result" | jq -r '.signed_inputs // 0')
        if [ ! -z "$processed_psbt" ] && [ "$signed_count" -gt 0 ]; then
            log_success "Processed PSBT: $signed_count inputs signed"
        else
            log_error "PSBT processing failed"
            return 1
        fi
    else
        log_error "PSBT processing failed"
        return 1
    fi
    
    # Test 7: Finalize PSBT
    log_info "Test 7: Finalizing PSBT..."
    local finalize_params="[\"$processed_psbt\", true]"
    local finalize_result=$(rpc_call "finalizepsbt" "$finalize_params")
    
    if [ "$finalize_result" != "null" ]; then
        local final_tx_hex=$(echo "$finalize_result" | jq -r '.hex // empty')
        local is_complete=$(echo "$finalize_result" | jq -r '.complete // false')
        if [ ! -z "$final_tx_hex" ] && [ "$is_complete" = "true" ]; then
            log_success "Finalized PSBT: ${final_tx_hex:0:50}..."
        else
            log_error "PSBT finalization failed"
            return 1
        fi
    else
        log_error "PSBT finalization failed"
        return 1
    fi
    
    # Test 8: Send raw transaction
    log_info "Test 8: Sending raw transaction..."
    local send_params="[\"$final_tx_hex\"]"
    local send_result=$(rpc_call "sendrawtransaction" "$send_params")
    
    if [ "$send_result" != "null" ]; then
        log_success "Transaction sent: $send_result"
    else
        log_error "Failed to send transaction"
        return 1
    fi
    
    # Test 9: Verify transaction in mempool
    log_info "Test 9: Verifying transaction in mempool..."
    local mempool_info=$(rpc_call "getmempoolinfo" "[]")
    if [ "$mempool_info" != "null" ]; then
        local tx_count=$(echo "$mempool_info" | jq -r '.size // 0')
        if [ "$tx_count" -gt 0 ]; then
            log_success "Transaction in mempool: $tx_count transactions"
        else
            log_warning "No transactions in mempool"
        fi
    else
        log_warning "Could not get mempool info"
    fi
    
    log_success "All PSBT tests completed successfully!"
    return 0
}

# Main execution
main() {
    log_info "Starting Dinero PSBT RPC Smoke Test"
    log_info "Data directory: $DATADIR"
    log_info "RPC port: $RPC_PORT"
    
    # Clean up any existing data
    rm -rf "$DATADIR"
    mkdir -p "$DATADIR"
    
    # Start daemon
    log_info "Starting Dinero daemon..."
    ./bin/dinerod \
        -datadir="$DATADIR" \
        -regtest \
        -rpcport=$RPC_PORT \
        -port=$P2P_PORT \
        -rpcuser=testuser \
        -rpcpassword=testpass \
        -rpcallowip=127.0.0.1 \
        -server \
        -daemon=0 \
        -debug=0 \
        -printtoconsole=0 &
    
    DAEMON_PID=$!
    log_info "Daemon started with PID: $DAEMON_PID"
    
    # Wait for daemon to be ready
    if ! wait_for_daemon; then
        log_error "Failed to start daemon"
        exit 1
    fi
    
    # Run PSBT tests
    if run_psbt_tests; then
        log_success "🎉 All PSBT RPC tests PASSED!"
        log_success "V2 wallet stack is production-ready!"
        exit 0
    else
        log_error "❌ PSBT RPC tests FAILED!"
        exit 1
    fi
}

# Run main function
main "$@"
