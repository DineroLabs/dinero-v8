#!/bin/bash
# Phase 1 vNext Integration Smoke Tests
# Tests HTTP server, health/metrics endpoints, structured logging, and nodeinfo parser

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_DATA_DIR="$PROJECT_ROOT/test_data_phase1"
DAEMON_PID=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "${GREEN}[SMOKE]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

cleanup() {
    if [[ -n "$DAEMON_PID" ]]; then
        log "Stopping daemon (PID: $DAEMON_PID)"
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    
    if [[ -d "$TEST_DATA_DIR" ]]; then
        log "Cleaning up test data directory"
        rm -rf "$TEST_DATA_DIR"
    fi
}

trap cleanup EXIT

# Setup test environment
setup_test_env() {
    log "Setting up test environment"
    
    # Create test data directory
    mkdir -p "$TEST_DATA_DIR"
    
    # Create test nodeinfo.json
    cat > "$TEST_DATA_DIR/nodeinfo.json" << EOF
{
  "schema": "din.nodeinfo.v1",
  "network": "regtest",
  "data_dir": "$TEST_DATA_DIR",
  "http": {
    "bind_address": "127.0.0.1",
    "port": 18880,
    "enabled": true,
    "threads": 2,
    "health_endpoint": true,
    "metrics_endpoint": true,
    "max_request_size": 1048576
  },
  "rpc": {
    "bind_address": "127.0.0.1",
    "port": 20996,
    "enabled": true
  },
  "logging": {
    "level": "INFO",
    "structured": true,
    "console_output": true,
    "file_output": true,
    "log_file": "$TEST_DATA_DIR/daemon.log"
  },
  "mining": {
    "enabled": false
  },
  "p2p": {
    "enabled": false
  }
}
EOF
}

# Start daemon for testing
start_daemon() {
    log "Starting daemon for smoke tests"
    
    cd "$PROJECT_ROOT"
    
    # Start daemon in background
    ./dinerod -regtest -datadir="$TEST_DATA_DIR" -rpcport=20996 > "$TEST_DATA_DIR/daemon_output.log" 2>&1 &
    DAEMON_PID=$!
    
    log "Daemon started with PID: $DAEMON_PID"
    
    # Wait for daemon to be ready
    local max_attempts=30
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        if curl -s -f "http://127.0.0.1:18880/healthz" > /dev/null 2>&1; then
            log "Daemon is ready"
            return 0
        fi
        
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            error "Daemon process died during startup"
            cat "$TEST_DATA_DIR/daemon_output.log"
            return 1
        fi
        
        sleep 1
        ((attempt++))
    done
    
    error "Daemon failed to start within $max_attempts seconds"
    cat "$TEST_DATA_DIR/daemon_output.log"
    return 1
}

# Test health endpoint
test_health_endpoint() {
    log "Testing health endpoint"
    
    local response=$(curl -s -w "%{http_code}" "http://127.0.0.1:18880/healthz")
    local status_code="${response: -3}"
    local body="${response%???}"
    
    if [[ "$status_code" != "200" ]]; then
        error "Health endpoint returned status $status_code"
        return 1
    fi
    
    # Check JSON structure
    if ! echo "$body" | jq -e '.status == "ok"' > /dev/null 2>&1; then
        error "Health endpoint response missing 'status: ok'"
        echo "Response: $body"
        return 1
    fi
    
    if ! echo "$body" | jq -e '.version == "din.daemon.v1"' > /dev/null 2>&1; then
        error "Health endpoint response missing correct version"
        echo "Response: $body"
        return 1
    fi
    
    log "✅ Health endpoint test passed"
}

# Test metrics endpoint
test_metrics_endpoint() {
    log "Testing metrics endpoint"
    
    local response=$(curl -s -w "%{http_code}" "http://127.0.0.1:18880/metrics")
    local status_code="${response: -3}"
    local body="${response%???}"
    
    if [[ "$status_code" != "200" ]]; then
        error "Metrics endpoint returned status $status_code"
        return 1
    fi
    
    # Check Prometheus format
    if ! echo "$body" | grep -q "# HELP dinero_daemon_uptime_seconds"; then
        error "Metrics endpoint missing uptime metric"
        return 1
    fi
    
    if ! echo "$body" | grep -q "# TYPE dinero_rpc_requests_total counter"; then
        error "Metrics endpoint missing RPC requests metric"
        return 1
    fi
    
    log "✅ Metrics endpoint test passed"
}

# Test RPC endpoint
test_rpc_endpoint() {
    log "Testing RPC endpoint"
    
    # Test valid JSON-RPC request
    local rpc_request='{"jsonrpc":"2.0","method":"help","params":[],"id":1}'
    local response=$(curl -s -w "%{http_code}" -X POST \
        -H "Content-Type: application/json" \
        -d "$rpc_request" \
        "http://127.0.0.1:18880/")
    
    local status_code="${response: -3}"
    local body="${response%???}"
    
    if [[ "$status_code" != "200" ]]; then
        error "RPC endpoint returned status $status_code"
        echo "Response: $body"
        return 1
    fi
    
    # Check JSON-RPC response structure
    if ! echo "$body" | jq -e '.jsonrpc == "2.0"' > /dev/null 2>&1; then
        error "RPC response missing jsonrpc field"
        echo "Response: $body"
        return 1
    fi
    
    if ! echo "$body" | jq -e '.id == 1' > /dev/null 2>&1; then
        error "RPC response missing correct id"
        echo "Response: $body"
        return 1
    fi
    
    log "✅ RPC endpoint test passed"
}

# Test invalid JSON handling
test_invalid_json() {
    log "Testing invalid JSON handling"
    
    local response=$(curl -s -w "%{http_code}" -X POST \
        -H "Content-Type: application/json" \
        -d "invalid json" \
        "http://127.0.0.1:18880/")
    
    local status_code="${response: -3}"
    local body="${response%???}"
    
    if [[ "$status_code" != "400" ]]; then
        error "Invalid JSON should return 400, got $status_code"
        return 1
    fi
    
    if ! echo "$body" | jq -e '.error' > /dev/null 2>&1; then
        error "Invalid JSON response should contain error field"
        echo "Response: $body"
        return 1
    fi
    
    log "✅ Invalid JSON handling test passed"
}

# Test 404 handling
test_404_handling() {
    log "Testing 404 handling"
    
    local response=$(curl -s -w "%{http_code}" "http://127.0.0.1:18880/nonexistent")
    local status_code="${response: -3}"
    
    if [[ "$status_code" != "404" ]]; then
        error "Nonexistent endpoint should return 404, got $status_code"
        return 1
    fi
    
    log "✅ 404 handling test passed"
}

# Test structured logging
test_structured_logging() {
    log "Testing structured logging"
    
    local log_file="$TEST_DATA_DIR/daemon.log"
    
    if [[ ! -f "$log_file" ]]; then
        error "Structured log file not found: $log_file"
        return 1
    fi
    
    # Check for JSON log entries
    if ! grep -q '"timestamp"' "$log_file"; then
        error "Structured log missing timestamp field"
        return 1
    fi
    
    if ! grep -q '"level"' "$log_file"; then
        error "Structured log missing level field"
        return 1
    fi
    
    if ! grep -q '"component"' "$log_file"; then
        error "Structured log missing component field"
        return 1
    fi
    
    log "✅ Structured logging test passed"
}

# Test nodeinfo.json parsing
test_nodeinfo_parsing() {
    log "Testing nodeinfo.json parsing"
    
    local nodeinfo_file="$TEST_DATA_DIR/nodeinfo.json"
    
    if [[ ! -f "$nodeinfo_file" ]]; then
        error "nodeinfo.json file not found"
        return 1
    fi
    
    # Validate JSON structure
    if ! jq -e '.schema == "din.nodeinfo.v1"' "$nodeinfo_file" > /dev/null 2>&1; then
        error "nodeinfo.json missing correct schema"
        return 1
    fi
    
    if ! jq -e '.http.port == 18880' "$nodeinfo_file" > /dev/null 2>&1; then
        error "nodeinfo.json HTTP port not configured correctly"
        return 1
    fi
    
    log "✅ nodeinfo.json parsing test passed"
}

# Test trace ID headers
test_trace_headers() {
    log "Testing trace ID headers"
    
    local headers=$(curl -s -I "http://127.0.0.1:18880/healthz")
    
    if ! echo "$headers" | grep -q "X-Trace-ID:"; then
        error "Missing X-Trace-ID header"
        echo "Headers: $headers"
        return 1
    fi
    
    log "✅ Trace ID headers test passed"
}

# Main test execution
main() {
    log "Starting Phase 1 vNext Integration Smoke Tests"
    
    # Check dependencies
    if ! command -v curl &> /dev/null; then
        error "curl is required for smoke tests"
        exit 1
    fi
    
    if ! command -v jq &> /dev/null; then
        error "jq is required for smoke tests"
        exit 1
    fi
    
    # Check if daemon binary exists
    if [[ ! -f "$PROJECT_ROOT/dinerod" ]]; then
        error "dinerod binary not found. Please build the project first."
        exit 1
    fi
    
    setup_test_env
    start_daemon
    
    # Run all tests
    local tests=(
        "test_health_endpoint"
        "test_metrics_endpoint"
        "test_rpc_endpoint"
        "test_invalid_json"
        "test_404_handling"
        "test_structured_logging"
        "test_nodeinfo_parsing"
        "test_trace_headers"
    )
    
    local passed=0
    local failed=0
    
    for test in "${tests[@]}"; do
        if $test; then
            ((passed++))
        else
            ((failed++))
        fi
    done
    
    log "Phase 1 Smoke Tests Complete"
    log "Passed: $passed, Failed: $failed"
    
    if [[ $failed -gt 0 ]]; then
        error "Some tests failed"
        exit 1
    else
        log "🎉 All Phase 1 smoke tests passed!"
        exit 0
    fi
}

main "$@"
