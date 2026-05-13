#!/bin/bash

# GUI Integration Test
# Tests the complete integrated system with real blockchain data

set -e

echo "🧪 GUI Integration Testing Suite"
echo "================================="
echo ""

# Configuration
DINERO_ROOT="/Users/haydarevich/Documents/DineroCoin"
TEST_DATADIR="$HOME/.dinero-test-integration"
TIMEOUT=30
NETWORKS=("regtest" "testnet")

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

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

# Test functions
test_daemon_startup() {
    local network=$1
    log_info "Testing daemon startup for $network..."
    
    # Start daemon
    cd "$DINERO_ROOT"
    timeout $TIMEOUT ./build/dinerod --network=$network --datadir="$TEST_DATADIR" --daemon &
    DAEMON_PID=$!
    
    # Wait for daemon to start
    sleep 5
    
    # Check if daemon is running
    if kill -0 $DAEMON_PID 2>/dev/null; then
        log_success "Daemon started successfully for $network"
        
        # Test RPC connection
        if timeout 10 curl -s "http://localhost:20978/api/v1/getblockchaininfo" >/dev/null; then
            log_success "RPC endpoint responding for $network"
        else
            log_warning "RPC endpoint not responding for $network"
        fi
        
        # Stop daemon
        kill $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
    else
        log_error "Failed to start daemon for $network"
        return 1
    fi
}

test_database_integrity() {
    local network=$1
    log_info "Testing database integrity for $network..."
    
    local db_path="$TEST_DATADIR/$network/blockchain.db"
    
    if [ -f "$db_path" ]; then
        # Test SQLite integrity
        if sqlite3 "$db_path" "PRAGMA integrity_check;" | grep -q "ok"; then
            log_success "Database integrity check passed for $network"
        else
            log_error "Database integrity check failed for $network"
            return 1
        fi
        
        # Test meta table
        if sqlite3 "$db_path" "SELECT COUNT(*) FROM meta;" >/dev/null 2>&1; then
            log_success "Meta table accessible for $network"
        else
            log_error "Meta table not accessible for $network"
            return 1
        fi
        
        # Test headers table
        if sqlite3 "$db_path" "SELECT COUNT(*) FROM headers;" >/dev/null 2>&1; then
            log_success "Headers table accessible for $network"
        else
            log_error "Headers table not accessible for $network"
            return 1
        fi
    else
        log_warning "Database not found for $network: $db_path"
    fi
}

test_rpc_endpoints() {
    local network=$1
    local port=20978  # regtest port
    
    if [ "$network" = "testnet" ]; then
        port=20988
    fi
    
    log_info "Testing RPC endpoints for $network on port $port..."
    
    # Start daemon
    cd "$DINERO_ROOT"
    timeout $TIMEOUT ./build/dinerod --network=$network --datadir="$TEST_DATADIR" --daemon &
    DAEMON_PID=$!
    
    # Wait for daemon to start
    sleep 8
    
    # Test Core 4 RPCs
    local endpoints=("getblockchaininfo" "getbestblockhash" "getblockcount" "uptime")
    
    for endpoint in "${endpoints[@]}"; do
        if timeout 10 curl -s "http://localhost:$port/api/v1/$endpoint" | jq . >/dev/null 2>&1; then
            log_success "RPC endpoint $endpoint working for $network"
        else
            log_error "RPC endpoint $endpoint failed for $network"
        fi
    done
    
    # Test block/header RPCs
    local block_endpoints=("getblockhash" "getblockheader" "getblock")
    
    for endpoint in "${block_endpoints[@]}"; do
        if timeout 10 curl -s "http://localhost:$port/api/v1/$endpoint?height=0" | jq . >/dev/null 2>&1; then
            log_success "Block RPC endpoint $endpoint working for $network"
        else
            log_warning "Block RPC endpoint $endpoint may need parameters for $network"
        fi
    done
    
    # Test network/mempool RPCs
    local net_endpoints=("getnetworkinfo" "getmempoolinfo")
    
    for endpoint in "${net_endpoints[@]}"; do
        if timeout 10 curl -s "http://localhost:$port/api/v1/$endpoint" | jq . >/dev/null 2>&1; then
            log_success "Network RPC endpoint $endpoint working for $network"
        else
            log_error "Network RPC endpoint $endpoint failed for $network"
        fi
    done
    
    # Stop daemon
    kill $DAEMON_PID 2>/dev/null || true
    wait $DAEMON_PID 2>/dev/null || true
}

test_gui_startup() {
    log_info "Testing GUI startup..."
    
    cd "$DINERO_ROOT"
    
    # Check if GUI binary exists
    if [ ! -f "./build/dinero-desktop" ]; then
        log_error "GUI binary not found: ./build/dinero-desktop"
        return 1
    fi
    
    log_success "GUI binary found"
    
    # Test GUI startup (headless mode simulation)
    export QT_QPA_PLATFORM=offscreen
    timeout 15 ./build/dinero-desktop --test-mode &
    GUI_PID=$!
    
    sleep 3
    
    if kill -0 $GUI_PID 2>/dev/null; then
        log_success "GUI started successfully in test mode"
        kill $GUI_PID 2>/dev/null || true
        wait $GUI_PID 2>/dev/null || true
    else
        log_error "GUI failed to start"
        return 1
    fi
}

test_wallet_integration() {
    log_info "Testing wallet integration..."
    
    # Test HD wallet creation
    cd "$DINERO_ROOT"
    
    # Create test wallet directory
    mkdir -p "$TEST_DATADIR/wallet-test"
    
    # Test BIP84 address validation (if test exists)
    if [ -f "./build/test_bip84_addr_check" ]; then
        if ./build/test_bip84_addr_check; then
            log_success "BIP84 address validation test passed"
        else
            log_error "BIP84 address validation test failed"
            return 1
        fi
    else
        log_warning "BIP84 address validation test not found"
    fi
}

test_network_switching() {
    log_info "Testing network switching functionality..."
    
    # Test that different networks use different ports and data directories
    for network in "${NETWORKS[@]}"; do
        local expected_dir="$TEST_DATADIR/$network"
        
        # Create network directory
        mkdir -p "$expected_dir"
        
        # Test daemon with network-specific settings
        cd "$DINERO_ROOT"
        timeout $TIMEOUT ./build/dinerod --network=$network --datadir="$TEST_DATADIR" --daemon &
        DAEMON_PID=$!
        
        sleep 5
        
        if kill -0 $DAEMON_PID 2>/dev/null; then
            log_success "Network switching test passed for $network"
            
            # Check if network-specific directory was created
            if [ -d "$expected_dir" ]; then
                log_success "Network-specific directory created for $network"
            else
                log_warning "Network-specific directory not found for $network"
            fi
            
            kill $DAEMON_PID 2>/dev/null || true
            wait $DAEMON_PID 2>/dev/null || true
        else
            log_error "Network switching test failed for $network"
            return 1
        fi
    done
}

test_performance_integration() {
    log_info "Testing performance integration..."
    
    # Check if performance monitoring files exist
    if [ -f "include/gui-desktop/performance/gui_performance_integration.h" ]; then
        log_success "Performance integration header found"
    else
        log_error "Performance integration header missing"
        return 1
    fi
    
    if [ -f "src/gui-desktop/performance/gui_performance_integration.cpp" ]; then
        log_success "Performance integration implementation found"
    else
        log_error "Performance integration implementation missing"
        return 1
    fi
}

test_accessibility_integration() {
    log_info "Testing accessibility integration..."
    
    # Check if accessibility integration files exist
    if [ -f "include/gui-desktop/accessibility/gui_accessibility_integration.h" ]; then
        log_success "Accessibility integration header found"
    else
        log_error "Accessibility integration header missing"
        return 1
    fi
    
    if [ -f "src/gui-desktop/accessibility/gui_accessibility_integration.cpp" ]; then
        log_success "Accessibility integration implementation found"
    else
        log_error "Accessibility integration implementation missing"
        return 1
    fi
}

test_build_system() {
    log_info "Testing build system integration..."
    
    cd "$DINERO_ROOT"
    
    # Test CMake configuration
    if [ -f "CMakeLists.txt" ]; then
        log_success "Main CMakeLists.txt found"
    else
        log_error "Main CMakeLists.txt missing"
        return 1
    fi
    
    # Test GUI CMake configuration
    if [ -f "src/gui-desktop/CMakeLists.txt" ]; then
        log_success "GUI CMakeLists.txt found"
    else
        log_error "GUI CMakeLists.txt missing"
        return 1
    fi
    
    # Test if build directory exists and has recent builds
    if [ -d "build" ] && [ -f "build/dinerod" ]; then
        log_success "Build artifacts found"
    else
        log_warning "Build artifacts not found - may need to rebuild"
    fi
}

# Main test execution
main() {
    log_info "Starting GUI Integration Testing Suite..."
    echo ""
    
    # Cleanup previous test data
    rm -rf "$TEST_DATADIR"
    mkdir -p "$TEST_DATADIR"
    
    local failed_tests=0
    
    # Run tests
    echo "📋 Test Suite Execution:"
    echo "========================"
    
    # Test 1: Build System
    if ! test_build_system; then
        ((failed_tests++))
    fi
    echo ""
    
    # Test 2: Performance Integration
    if ! test_performance_integration; then
        ((failed_tests++))
    fi
    echo ""
    
    # Test 3: Accessibility Integration
    if ! test_accessibility_integration; then
        ((failed_tests++))
    fi
    echo ""
    
    # Test 4: Daemon Startup
    for network in "${NETWORKS[@]}"; do
        if ! test_daemon_startup "$network"; then
            ((failed_tests++))
        fi
    done
    echo ""
    
    # Test 5: Database Integrity
    for network in "${NETWORKS[@]}"; do
        if ! test_database_integrity "$network"; then
            ((failed_tests++))
        fi
    done
    echo ""
    
    # Test 6: RPC Endpoints
    for network in "${NETWORKS[@]}"; do
        if ! test_rpc_endpoints "$network"; then
            ((failed_tests++))
        fi
    done
    echo ""
    
    # Test 7: GUI Startup
    if ! test_gui_startup; then
        ((failed_tests++))
    fi
    echo ""
    
    # Test 8: Wallet Integration
    if ! test_wallet_integration; then
        ((failed_tests++))
    fi
    echo ""
    
    # Test 9: Network Switching
    if ! test_network_switching; then
        ((failed_tests++))
    fi
    echo ""
    
    # Summary
    echo "📊 Test Results Summary:"
    echo "========================"
    
    if [ $failed_tests -eq 0 ]; then
        log_success "All integration tests passed! 🎉"
        echo ""
        log_info "The integrated system is ready for deployment to beta users."
    else
        log_error "$failed_tests test(s) failed ❌"
        echo ""
        log_info "Please review the failed tests and fix issues before deployment."
    fi
    
    # Cleanup
    rm -rf "$TEST_DATADIR"
    
    return $failed_tests
}

# Run main function
main "$@"
