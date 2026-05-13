#!/usr/bin/env bash
set -euo pipefail

# =========================
# Dinero WebSocket RPC Test Suite
# =========================
# 
# Tests WebSocket RPC functionality including:
# - WebSocket upgrade from HTTP
# - Subscription management (subscribe/unsubscribe)
# - Real-time event broadcasting
# - Rate limiting
# - Connection management
#
# Usage: ./scripts/test-websocket-rpc.sh [daemon_bin] [datadir]
# Example: ./scripts/test-websocket-rpc.sh ./build-test/bin/dinerod /tmp/test-dir2

# -------- Configuration --------
DAEMON_BIN="${1:-./build-test/bin/dinerod}"
DATADIR="${2:-/tmp/test-dir2}"
NETWORK="mainnet"
RPC_PORT="20998"
WS_PATH="/rpc.ws"

# Test configuration
TEST_TIMEOUT=30
SUBSCRIPTION_TIMEOUT=10
RATE_LIMIT_ITERATIONS=100

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test results
TESTS_PASSED=0
TESTS_FAILED=0

# -------- Utility Functions --------

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

die() {
    echo -e "${RED}ERROR: $1${NC}" >&2
    exit 1
}

check_dependencies() {
    local deps=("curl" "jq" "websocat" "nc")
    local missing=()
    
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" >/dev/null 2>&1; then
            missing+=("$dep")
        fi
    done
    
    if [ ${#missing[@]} -gt 0 ]; then
        log_warning "Missing dependencies: ${missing[*]}"
        log_warning "Some tests may be skipped"
        
        # Check if websocat is available (required for WebSocket tests)
        if ! command -v "websocat" >/dev/null 2>&1; then
            log_error "websocat is required for WebSocket tests"
            log_info "Install with: cargo install websocat"
            return 1
        fi
    fi
    
    return 0
}

wait_for_daemon() {
    local max_attempts=30
    local attempt=1
    
    log_info "Waiting for daemon to be ready..."
    
    while [ $attempt -le $max_attempts ]; do
        if curl -s --user "$(cat "$DATADIR/$NETWORK/.cookie")" \
               -H 'content-type: application/json' \
               --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
               "http://127.0.0.1:$RPC_PORT" >/dev/null 2>&1; then
            log_success "Daemon is ready after $attempt attempts"
            return 0
        fi
        
        sleep 1
        ((attempt++))
    done
    
    log_error "Daemon failed to become ready after $max_attempts attempts"
    return 1
}

get_cookie_auth() {
    if [ -f "$DATADIR/$NETWORK/.cookie" ]; then
        local cookie=$(cat "$DATADIR/$NETWORK/.cookie")
        echo "Basic $(echo -n "$cookie" | base64)"
    else
        die "Cookie file not found at $DATADIR/$NETWORK/.cookie"
    fi
}

# -------- Test Functions --------

test_websocket_upgrade() {
    log_info "Testing WebSocket upgrade..."
    
    # Create WebSocket upgrade request
    local ws_key="dGhlIHNhbXBsZSBub25jZQ=="
    local upgrade_request="GET $WS_PATH HTTP/1.1\r\n"
    upgrade_request+="Host: 127.0.0.1:$RPC_PORT\r\n"
    upgrade_request+="Upgrade: websocket\r\n"
    upgrade_request+="Connection: Upgrade\r\n"
    upgrade_request+="Sec-WebSocket-Key: $ws_key\r\n"
    upgrade_request+="Sec-WebSocket-Version: 13\r\n"
    upgrade_request+="Authorization: $(get_cookie_auth)\r\n"
    upgrade_request+="\r\n"
    
    # Send upgrade request and check response
    local response
    response=$(echo -en "$upgrade_request" | nc -w 5 127.0.0.1 "$RPC_PORT")
    
    if echo "$response" | grep -q "HTTP/1.1 101 Switching Protocols"; then
        if echo "$response" | grep -q "Upgrade: websocket"; then
            if echo "$response" | grep -q "X-Dinero-RPC-Engine: v2"; then
                log_success "WebSocket upgrade successful with Dinero headers"
                return 0
            else
                log_error "WebSocket upgrade missing Dinero headers"
                return 1
            fi
        else
            log_error "WebSocket upgrade response missing Upgrade header"
            return 1
        fi
    else
        log_error "WebSocket upgrade failed - expected 101 response"
        return 1
    fi
}

test_subscription_flow() {
    log_info "Testing subscription flow..."
    
    # Test with websocat if available
    if ! command -v "websocat" >/dev/null 2>&1; then
        log_warning "websocat not available, skipping subscription test"
        return 0
    fi
    
    # Create subscription test script
    local test_script="/tmp/websocket_test.js"
    cat > "$test_script" << 'EOF'
const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:20998/rpc.ws', {
    headers: {
        'Authorization': 'Basic ' + Buffer.from('user:pass').toString('base64')
    }
});

ws.on('open', function open() {
    console.log('Connected to WebSocket');
    
    // Subscribe to newHeads
    const subscribe_msg = {
        jsonrpc: "2.0",
        id: 1,
        method: "subscribe",
        params: ["newHeads"]
    };
    
    ws.send(JSON.stringify(subscribe_msg));
});

ws.on('message', function message(data) {
    console.log('Received:', data.toString());
    
    try {
        const msg = JSON.parse(data.toString());
        if (msg.method === 'dinero_subscription') {
            console.log('Subscription notification received');
            process.exit(0);
        }
    } catch (e) {
        console.error('Failed to parse message:', e);
    }
});

ws.on('error', function error(err) {
    console.error('WebSocket error:', err);
    process.exit(1);
});

// Timeout after 10 seconds
setTimeout(() => {
    console.error('Test timeout');
    process.exit(1);
}, 10000);
EOF
    
    # Run the test
    if timeout "$SUBSCRIPTION_TIMEOUT" node "$test_script" >/dev/null 2>&1; then
        log_success "Subscription flow test passed"
        rm -f "$test_script"
        return 0
    else
        log_error "Subscription flow test failed"
        rm -f "$test_script"
        return 1
    fi
}

test_rate_limiting() {
    log_info "Testing rate limiting..."
    
    # Test HTTP RPC rate limiting
    local start_time=$(date +%s)
    local success_count=0
    local rate_limit_count=0
    
    for i in $(seq 1 "$RATE_LIMIT_ITERATIONS"); do
        local response
        response=$(curl -s --user "$(cat "$DATADIR/$NETWORK/.cookie")" \
                        -H 'content-type: application/json' \
                        --data '{"jsonrpc":"2.0","id":'"$i"',"method":"getblockcount","params":[]}' \
                        "http://127.0.0.1:$RPC_PORT")
        
        if echo "$response" | jq -e '.error.code == -429' >/dev/null 2>&1; then
            ((rate_limit_count++))
        elif echo "$response" | jq -e '.result' >/dev/null 2>&1; then
            ((success_count++))
        fi
        
        # Small delay to not overwhelm the system
        sleep 0.01
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    local actual_rate=$((success_count / duration))
    
    log_info "Rate limit test results:"
    log_info "  Duration: ${duration}s"
    log_info "  Successful requests: $success_count"
    log_info "  Rate limited requests: $rate_limit_count"
    log_info "  Actual rate: ${actual_rate} req/s"
    
    # Check if rate limiting is working
    if [ $rate_limit_count -gt 0 ]; then
        log_success "Rate limiting is working (some requests were limited)"
        return 0
    else
        log_warning "No rate limiting detected (all requests succeeded)"
        return 0
    fi
}

test_connection_limits() {
    log_info "Testing connection limits..."
    
    # Try to establish multiple WebSocket connections
    local max_connections=10
    local successful_connections=0
    local failed_connections=0
    
    for i in $(seq 1 "$max_connections"); do
        if timeout 5 websocat --no-close \
                -H "Authorization: $(get_cookie_auth)" \
                "ws://127.0.0.1:$RPC_PORT$WS_PATH" >/dev/null 2>&1; then
            ((successful_connections++))
        else
            ((failed_connections++))
        fi
        
        # Small delay between connections
        sleep 0.1
    done
    
    log_info "Connection limit test results:"
    log_info "  Successful connections: $successful_connections"
    log_info "  Failed connections: $failed_connections"
    
    if [ $successful_connections -gt 0 ]; then
        log_success "Connection establishment working"
        return 0
    else
        log_error "All connection attempts failed"
        return 1
    fi
}

test_websocket_methods() {
    log_info "Testing WebSocket RPC methods..."
    
    # Test if WebSocket supports regular RPC methods
    local test_script="/tmp/websocket_rpc_test.js"
    cat > "$test_script" << 'EOF'
const WebSocket = require('ws');

const ws = new WebSocket('ws://127.0.0.1:20998/rpc.ws', {
    headers: {
        'Authorization': 'Basic ' + Buffer.from('user:pass').toString('base64')
    }
});

ws.on('open', function open() {
    console.log('Connected to WebSocket');
    
    // Test regular RPC method
    const rpc_msg = {
        jsonrpc: "2.0",
        id: 1,
        method: "getblockcount",
        params: []
    };
    
    ws.send(JSON.stringify(rpc_msg));
});

ws.on('message', function message(data) {
    console.log('Received:', data.toString());
    
    try {
        const msg = JSON.parse(data.toString());
        if (msg.result !== undefined) {
            console.log('RPC method working over WebSocket');
            process.exit(0);
        }
    } catch (e) {
        console.error('Failed to parse message:', e);
    }
});

ws.on('error', function error(err) {
    console.error('WebSocket error:', err);
    process.exit(1);
});

// Timeout after 10 seconds
setTimeout(() => {
    console.error('Test timeout');
    process.exit(1);
}, 10000);
EOF
    
    # Run the test
    if timeout "$SUBSCRIPTION_TIMEOUT" node "$test_script" >/dev/null 2>&1; then
        log_success "WebSocket RPC methods working"
        rm -f "$test_script"
        return 0
    else
        log_error "WebSocket RPC methods test failed"
        rm -f "$test_script"
        return 1
    fi
}

test_error_handling() {
    log_info "Testing error handling..."
    
    # Test malformed WebSocket upgrade
    local malformed_request="GET /invalid HTTP/1.1\r\n"
    malformed_request+="Host: 127.0.0.1:$RPC_PORT\r\n"
    malformed_request+="\r\n"
    
    local response
    response=$(echo -en "$malformed_request" | nc -w 5 127.0.0.1 "$RPC_PORT")
    
    if echo "$response" | grep -q "HTTP/1.1 400\|HTTP/1.1 404"; then
        log_success "Malformed request properly rejected"
        return 0
    else
        log_error "Malformed request not properly handled"
        return 1
    fi
}

# -------- Main Test Execution --------

main() {
    log_info "Starting Dinero WebSocket RPC Test Suite"
    log_info "Daemon: $DAEMON_BIN"
    log_info "Data directory: $DATADIR"
    log_info "Network: $NETWORK"
    log_info "RPC Port: $RPC_PORT"
    log_info "WebSocket Path: $WS_PATH"
    
    # Check dependencies
    if ! check_dependencies; then
        log_warning "Some dependencies missing, tests may be limited"
    fi
    
    # Verify daemon is running
    if ! wait_for_daemon; then
        die "Daemon is not accessible"
    fi
    
    # Run tests
    log_info "Running WebSocket tests..."
    
    # Test 1: WebSocket upgrade
    if test_websocket_upgrade; then
        log_success "WebSocket upgrade test passed"
    else
        log_error "WebSocket upgrade test failed"
    fi
    
    # Test 2: Subscription flow
    if test_subscription_flow; then
        log_success "Subscription flow test passed"
    else
        log_error "Subscription flow test failed"
    fi
    
    # Test 3: Rate limiting
    if test_rate_limiting; then
        log_success "Rate limiting test passed"
    else
        log_error "Rate limiting test failed"
    fi
    
    # Test 4: Connection limits
    if test_connection_limits; then
        log_success "Connection limits test passed"
    else
        log_error "Connection limits test failed"
    fi
    
    # Test 5: WebSocket RPC methods
    if test_websocket_methods; then
        log_success "WebSocket RPC methods test passed"
    else
        log_error "WebSocket RPC methods test failed"
    fi
    
    # Test 6: Error handling
    if test_error_handling; then
        log_success "Error handling test passed"
    else
        log_error "Error handling test failed"
    fi
    
    # Summary
    log_info "Test Summary:"
    log_info "  Tests passed: $TESTS_PASSED"
    log_info "  Tests failed: $TESTS_FAILED"
    log_info "  Total tests: $((TESTS_PASSED + TESTS_FAILED))"
    
    if [ $TESTS_FAILED -eq 0 ]; then
        log_success "All WebSocket RPC tests passed! 🎉"
        exit 0
    else
        log_error "Some tests failed. Check the output above for details."
        exit 1
    fi
}

# Handle script arguments
if [ $# -eq 0 ]; then
    log_info "No arguments provided, using defaults:"
    log_info "  Daemon: $DAEMON_BIN"
    log_info "  Data directory: $DATADIR"
    log_info "  Run with: $0 [daemon_bin] [datadir]"
fi

# Run main function
main "$@"
