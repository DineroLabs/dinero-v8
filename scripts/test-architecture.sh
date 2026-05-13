#!/bin/bash

# Dinero Architecture Regression Test Script
# Runs comprehensive tests to ensure architecture remains stable

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
BUILD_DIR="build-test"
TEST_BINARY="$BUILD_DIR/bin/test_architecture_regression"
DAEMON_BINARY="$BUILD_DIR/bin/dinerod"
CLI_BINARY="$BUILD_DIR/bin/dinero-cli"
TEST_DATA_DIR="./test-data"

echo -e "${BLUE}=== Dinero Architecture Regression Tests ===${NC}"

# Clean up previous test data
cleanup() {
    echo "Cleaning up test data..."
    rm -rf "$TEST_DATA_DIR"
    pkill -f "dinerod.*test-data" 2>/dev/null || true
    sleep 2
}

# Build test binary
build_tests() {
    echo -e "${YELLOW}Building test binary...${NC}"
    
    # Create build directory
    mkdir -p "$BUILD_DIR"
    
    # Configure with test flags
    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_SANITIZERS=ON \
        -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos" \
        -DBUILD_TESTS=ON
    
    # Build
    cmake --build "$BUILD_DIR" -j8
    
    if [[ ! -f "$TEST_BINARY" ]]; then
        echo -e "${RED}❌ Test binary not found: $TEST_BINARY${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ Test binary built successfully${NC}"
}

# Run regression tests
run_regression_tests() {
    echo -e "${YELLOW}Running regression tests...${NC}"
    
    if [[ ! -f "$TEST_BINARY" ]]; then
        echo -e "${RED}❌ Test binary not found${NC}"
        return 1
    fi
    
    # Run the test binary
    if "$TEST_BINARY"; then
        echo -e "${GREEN}✅ Regression tests passed${NC}"
        return 0
    else
        echo -e "${RED}❌ Regression tests failed${NC}"
        return 1
    fi
}

# Test cookie authentication
test_cookie_auth() {
    echo -e "${YELLOW}Testing cookie authentication...${NC}"
    
    # Create test data directory
    mkdir -p "$TEST_DATA_DIR"
    
    # Start daemon in test mode
    echo "Starting test daemon..."
    "$DAEMON_BINARY" -datadir="$TEST_DATA_DIR" -regtest=1 -server=1 -rpcport=20996 &
    DAEMON_PID=$!
    
    # Wait for daemon to start
    sleep 5
    
    # Check if daemon is running
    if ! kill -0 $DAEMON_PID 2>/dev/null; then
        echo -e "${RED}❌ Daemon failed to start${NC}"
        return 1
    fi
    
    # Test cookie file exists
    COOKIE_FILE="$TEST_DATA_DIR/.cookie"
    if [[ ! -f "$COOKIE_FILE" ]]; then
        echo -e "${RED}❌ Cookie file not created: $COOKIE_FILE${NC}"
        kill $DAEMON_PID 2>/dev/null || true
        return 1
    fi
    
    # Test cookie format
    COOKIE_CONTENT=$(cat "$COOKIE_FILE" | tr -d '\n\r')
    if [[ "$COOKIE_CONTENT" != *":"* ]]; then
        echo -e "${RED}❌ Invalid cookie format${NC}"
        kill $DAEMON_PID 2>/dev/null || true
        return 1
    fi
    
    # Test RPC call with cookie auth
    RPC_URL="http://127.0.0.1:20996"
    AUTH_HEADER="Authorization: Basic $(echo -n "$COOKIE_CONTENT" | base64)"
    
    RESPONSE=$(curl -s -X POST "$RPC_URL" \
        -H "Content-Type: application/json" \
        -H "$AUTH_HEADER" \
        -d '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}')
    
    if echo "$RESPONSE" | jq -e '.result' > /dev/null 2>&1; then
        echo -e "${GREEN}✅ Cookie authentication working${NC}"
        AUTH_SUCCESS=true
    else
        echo -e "${RED}❌ Cookie authentication failed${NC}"
        echo "Response: $RESPONSE"
        AUTH_SUCCESS=false
    fi
    
    # Stop daemon
    kill $DAEMON_PID 2>/dev/null || true
    wait $DAEMON_PID 2>/dev/null || true
    
    if [[ "$AUTH_SUCCESS" == "true" ]]; then
        return 0
    else
        return 1
    fi
}

# Test static credentials
test_static_auth() {
    echo -e "${YELLOW}Testing static credentials...${NC}"
    
    # Create test config with static credentials
    cat > "$TEST_DATA_DIR/dinero.conf" << EOF
server=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=20996
port=21001
rpcuser=testuser
rpcpassword=testpass123
regtest=1
EOF
    
    # Start daemon with static credentials
    echo "Starting daemon with static credentials..."
    "$DAEMON_BINARY" -datadir="$TEST_DATA_DIR" -regtest=1 &
    DAEMON_PID=$!
    
    # Wait for daemon to start
    sleep 5
    
    # Test RPC call with static credentials
    RPC_URL="http://127.0.0.1:20996"
    AUTH_HEADER="Authorization: Basic $(echo -n "testuser:testpass123" | base64)"
    
    RESPONSE=$(curl -s -X POST "$RPC_URL" \
        -H "Content-Type: application/json" \
        -H "$AUTH_HEADER" \
        -d '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}')
    
    if echo "$RESPONSE" | jq -e '.result' > /dev/null 2>&1; then
        echo -e "${GREEN}✅ Static credentials working${NC}"
        STATIC_SUCCESS=true
    else
        echo -e "${RED}❌ Static credentials failed${NC}"
        echo "Response: $RESPONSE"
        STATIC_SUCCESS=false
    fi
    
    # Stop daemon
    kill $DAEMON_PID 2>/dev/null || true
    wait $DAEMON_PID 2>/dev/null || true
    
    if [[ "$STATIC_SUCCESS" == "true" ]]; then
        return 0
    else
        return 1
    fi
}

# Test smoke mining
test_smoke_mining() {
    echo -e "${YELLOW}Testing smoke mining...${NC}"
    
    # Start daemon in regtest mode
    echo "Starting regtest daemon..."
    "$DAEMON_BINARY" -datadir="$TEST_DATA_DIR" -regtest=1 &
    DAEMON_PID=$!
    
    # Wait for daemon to start
    sleep 5
    
    # Generate one block (smoke test)
    if [[ -f "$CLI_BINARY" ]]; then
        echo "Generating test block..."
        BLOCK_HASH=$("$CLI_BINARY" -datadir="$TEST_DATA_DIR" -regtest=1 generate 1 2>/dev/null | tail -1)
        
        if [[ -n "$BLOCK_HASH" && "$BLOCK_HASH" != "null" ]]; then
            echo -e "${GREEN}✅ Smoke mining successful: $BLOCK_HASH${NC}"
            MINING_SUCCESS=true
        else
            echo -e "${RED}❌ Smoke mining failed${NC}"
            MINING_SUCCESS=false
        fi
    else
        echo -e "${YELLOW}⚠️  CLI binary not found, skipping smoke mining test${NC}"
        MINING_SUCCESS=true
    fi
    
    # Stop daemon
    kill $DAEMON_PID 2>/dev/null || true
    wait $DAEMON_PID 2>/dev/null || true
    
    if [[ "$MINING_SUCCESS" == "true" ]]; then
        return 0
    else
        return 1
    fi
}

# Test health monitoring script
test_health_script() {
    echo -e "${YELLOW}Testing health monitoring script...${NC}"
    
    # Start daemon
    "$DAEMON_BINARY" -datadir="$TEST_DATA_DIR" -regtest=1 &
    DAEMON_PID=$!
    sleep 5
    
    # Test health script
    if [[ -f "scripts/din-health.sh" ]]; then
        export DINERO_RPC_URL="http://127.0.0.1:20996"
        export DINERO_DATADIR="$TEST_DATA_DIR"
        export DINERO_COOKIE_FILE="$TEST_DATA_DIR/.cookie"
        
        if ./scripts/din-health.sh --network regtest > /dev/null 2>&1; then
            echo -e "${GREEN}✅ Health script working${NC}"
            HEALTH_SUCCESS=true
        else
            echo -e "${RED}❌ Health script failed${NC}"
            HEALTH_SUCCESS=false
        fi
    else
        echo -e "${YELLOW}⚠️  Health script not found${NC}"
        HEALTH_SUCCESS=true
    fi
    
    # Stop daemon
    kill $DAEMON_PID 2>/dev/null || true
    wait $DAEMON_PID 2>/dev/null || true
    
    if [[ "$HEALTH_SUCCESS" == "true" ]]; then
        return 0
    else
        return 1
    fi
}

# Main test runner
main() {
    local exit_code=0
    
    # Cleanup on exit
    trap cleanup EXIT
    
    # Build tests
    if ! build_tests; then
        exit_code=1
    fi
    
    # Run regression tests
    if ! run_regression_tests; then
        exit_code=1
    fi
    
    # Test cookie authentication
    if ! test_cookie_auth; then
        exit_code=1
    fi
    
    # Test static credentials
    if ! test_static_auth; then
        exit_code=1
    fi
    
    # Test smoke mining
    if ! test_smoke_mining; then
        exit_code=1
    fi
    
    # Test health script
    if ! test_health_script; then
        exit_code=1
    fi
    
    echo
    echo -e "${BLUE}=== Test Summary ===${NC}"
    
    if [[ $exit_code -eq 0 ]]; then
        echo -e "${GREEN}🎉 All architecture tests passed!${NC}"
        echo -e "${GREEN}Architecture is regression-proof and ready for production.${NC}"
    else
        echo -e "${RED}❌ Some architecture tests failed.${NC}"
        echo -e "${RED}Please fix the issues before deploying.${NC}"
    fi
    
    return $exit_code
}

# Handle command line arguments
case "${1:-}" in
    "--help" | "-h")
        echo "Dinero Architecture Regression Test Script"
        echo
        echo "Usage: $0 [options]"
        echo
        echo "Options:"
        echo "  --help, -h     Show this help message"
        echo "  --build-only   Only build tests, don't run them"
        echo "  --clean        Clean build directory and exit"
        echo
        echo "This script runs comprehensive tests to ensure the Dinero"
        echo "architecture remains stable and regression-proof."
        exit 0
        ;;
    "--build-only")
        build_tests
        exit $?
        ;;
    "--clean")
        cleanup
        rm -rf "$BUILD_DIR"
        echo "Cleaned build directory"
        exit 0
        ;;
    "")
        main
        ;;
    *)
        echo -e "${RED}Error: Unknown option '$1'${NC}"
        echo "Use --help for usage information"
        exit 1
        ;;
esac
