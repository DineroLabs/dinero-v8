#!/usr/bin/env bash
set -euo pipefail

# =========================
# Test Separated HTTP RPC + WebSocket Services
# =========================
# 
# Tests the clean separation of:
# - HTTP RPC on port 20998 (JSON-RPC 2.0)
# - WebSocket server on port 21001 (subscriptions)
#
# Usage: ./scripts/test-separated-services.sh

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test results
TESTS_PASSED=0
TESTS_FAILED=0

# Configuration
HTTP_RPC_PORT=20998
WS_PORT=21001
COOKIE_FILE="/private/tmp/test-dir2/.cookie"

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

# -------- Check Dependencies --------

check_dependencies() {
    local deps=("curl" "jq" "websocat")
    local missing=()
    
    for dep in "${deps[@]}"; do
        if ! command -v "$dep" >/dev/null 2>&1; then
            missing+=("$dep")
        fi
    done
    
    if [ ${#missing[@]} -gt 0 ]; then
        log_warning "Missing dependencies: ${missing[*]}"
        log_warning "Install websocat with: cargo install websocat"
        log_warning "Some tests may be skipped"
    fi
    
    return 0
}

# -------- Check Services --------

check_http_rpc() {
    log_info "Testing HTTP RPC on port $HTTP_RPC_PORT..."
    
    if ! curl -s --connect-timeout 5 "http://127.0.0.1:$HTTP_RPC_PORT/" >/dev/null 2>&1; then
        log_error "HTTP RPC server not responding on port $HTTP_RPC_PORT"
        return 1
    fi
    
    log_success "HTTP RPC server responding on port $HTTP_RPC_PORT"
    return 0
}

check_websocket() {
    log_info "Testing WebSocket server on port $WS_PORT..."
    
    if ! curl -s --connect-timeout 5 "http://127.0.0.1:$WS_PORT/" >/dev/null 2>&1; then
        log_error "WebSocket server not responding on port $WS_PORT"
        return 1
    fi
    
    log_success "WebSocket server responding on port $WS_PORT"
    return 0
}

# -------- Test HTTP RPC --------

test_http_rpc() {
    log_info "Testing HTTP RPC functionality..."
    
    if [ ! -f "$COOKIE_FILE" ]; then
        log_error "Cookie file not found: $COOKIE_FILE"
        return 1
    fi
    
    local AUTH
    AUTH="$(tr -d '\n' < "$COOKIE_FILE")"
    
    # Test getblockcount
    local response
    response=$(curl -s --basic --user "$AUTH" \
        -H 'content-type: application/json' \
        --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
        "http://127.0.0.1:$HTTP_RPC_PORT/")
    
    if echo "$response" | jq -e '.result' >/dev/null 2>&1; then
        log_success "HTTP RPC getblockcount working"
    else
        log_error "HTTP RPC getblockcount failed: $response"
        return 1
    fi
    
    # Test getblockchaininfo
    response=$(curl -s --basic --user "$AUTH" \
        -H 'content-type: application/json' \
        --data '{"jsonrpc":"2.0","id":2,"method":"getblockchaininfo","params":[]}' \
        "http://127.0.0.1:$HTTP_RPC_PORT/")
    
    if echo "$response" | jq -e '.result.chain' >/dev/null 2>&1; then
        log_success "HTTP RPC getblockchaininfo working"
    else
        log_error "HTTP RPC getblockchaininfo failed: $response"
        return 1
    fi
    
    return 0
}

# -------- Test WebSocket --------

test_websocket() {
    log_info "Testing WebSocket functionality..."
    
    if ! command -v websocat >/dev/null 2>&1; then
        log_warning "websocat not available, skipping WebSocket tests"
        return 0
    fi
    
    if [ ! -f "$COOKIE_FILE" ]; then
        log_error "Cookie file not found: $COOKIE_FILE"
        return 1
    fi
    
    local AUTH
    AUTH="$(tr -d '\n' < "$COOKIE_FILE")"
    local TOKEN
    TOKEN="$(printf '%s' "$AUTH" | base64)"
    
    # Test WebSocket connection and subscription
    local ws_output
    ws_output=$(timeout 10 websocat -H "Authorization: Basic $TOKEN" \
        "ws://127.0.0.1:$WS_PORT" \
        -1 \
        --text \
        --ping-interval 0 \
        --ping-timeout 0 \
        --close-on-eof \
        --no-close \
        < <(echo '{"op":"ping"}') 2>/dev/null || true)
    
    if echo "$ws_output" | grep -q '"op":"pong"'; then
        log_success "WebSocket ping/pong working"
    else
        log_error "WebSocket ping/pong failed: $ws_output"
        return 1
    fi
    
    return 0
}

# -------- Main Test Execution --------

main() {
    echo "=========================================="
    echo "Testing Separated HTTP RPC + WebSocket Services"
    echo "=========================================="
    echo "HTTP RPC Port: $HTTP_RPC_PORT"
    echo "WebSocket Port: $WS_PORT"
    echo "Cookie File: $COOKIE_FILE"
    echo ""
    
    # Check dependencies
    check_dependencies
    
    # Check if services are running
    if ! check_http_rpc; then
        log_error "HTTP RPC service check failed"
        ((TESTS_FAILED++))
    fi
    
    if ! check_websocket; then
        log_error "WebSocket service check failed"
        ((TESTS_FAILED++))
    fi
    
    # Test HTTP RPC functionality
    if test_http_rpc; then
        log_success "HTTP RPC functionality tests passed"
    else
        log_error "HTTP RPC functionality tests failed"
        ((TESTS_FAILED++))
    fi
    
    # Test WebSocket functionality
    if test_websocket; then
        log_success "WebSocket functionality tests passed"
    else
        log_error "WebSocket functionality tests failed"
        ((TESTS_FAILED++))
    fi
    
    # Test summary
    echo ""
    echo "=========================================="
    echo "Test Summary"
    echo "=========================================="
    echo "Tests passed: $TESTS_PASSED"
    echo "Tests failed: $TESTS_FAILED"
    echo "Total tests: $((TESTS_PASSED + TESTS_FAILED))"
    
    if [ $TESTS_FAILED -eq 0 ]; then
        echo ""
        log_success "All tests passed! Services are working correctly."
        echo ""
        echo "✅ HTTP RPC: http://127.0.0.1:$HTTP_RPC_PORT/"
        echo "✅ WebSocket: ws://127.0.0.1:$WS_PORT/"
        echo ""
        echo "Usage examples:"
        echo "  # HTTP RPC"
        echo "  curl --basic --user \"\$AUTH\" -H 'content-type: application/json' \\"
        echo "    --data '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockcount\",\"params\":[]}' \\"
        echo "    http://127.0.0.1:$HTTP_RPC_PORT/"
        echo ""
        echo "  # WebSocket"
        echo "  websocat -H \"Authorization: Basic \$TOKEN\" ws://127.0.0.1:$WS_PORT"
        echo "  # Then send: {\"op\":\"subscribe\",\"topic\":\"blocks\"}"
        exit 0
    else
        echo ""
        log_error "Some tests failed. Check the output above for details."
        exit 1
    fi
}

# Run main function
main "$@"
