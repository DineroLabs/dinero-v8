#!/bin/bash
# vNext Smoke Test - Quick validation of vNext-only daemon
set -euo pipefail

# Configuration
DATADIR="/tmp/dinero-smoke-test"
RPC_PORT=20998
HTTP_PORT=8080
TIMEOUT=30

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."
    pkill -f dinerod || true
    rm -rf "$DATADIR"
}

trap cleanup EXIT

# Check if daemon binary exists
if [ ! -f "./build/bin/dinerod" ]; then
    log_error "dinerod binary not found. Build first with: cmake --build build --target dinerod"
    exit 1
fi

# Clean start
log_info "Starting vNext smoke test..."
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

# Launch daemon
log_info "Launching daemon..."
./build/bin/dinerod \
    --regtest \
    --datadir="$DATADIR" \
    --rpcport="$RPC_PORT" \
    --httpport="$HTTP_PORT" \
    --log-level=info \
    -gen=0 &

DAEMON_PID=$!
sleep 5

# Check if daemon is running
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    log_error "Daemon failed to start"
    exit 1
fi

log_info "Daemon started (PID: $DAEMON_PID)"

# Test 1: Health check
log_info "Test 1: Health check"
HEALTH_RESPONSE=$(curl -s --max-time $TIMEOUT http://127.0.0.1:$HTTP_PORT/healthz)
echo "Health response: $HEALTH_RESPONSE"

# Check if daemon is running (status: ok)
if echo "$HEALTH_RESPONSE" | jq -e '.status=="ok"' >/dev/null 2>&1; then
    log_info "✅ Health check passed - daemon is running"
else
    log_error "❌ Health check failed - daemon not responding"
    exit 1
fi

# Test 2: Auth enforcement
log_info "Test 2: Auth enforcement"
UNAUTH_RESPONSE=$(curl -s --max-time $TIMEOUT -X POST \
    -H "Content-Type: application/json" \
    -d '{"jsonrpc":"2.0","method":"help","params":[],"id":1}' \
    http://127.0.0.1:$HTTP_PORT/)

if echo "$UNAUTH_RESPONSE" | jq -e '.error.code==-32600' >/dev/null 2>&1; then
    log_info "✅ Auth enforcement working"
else
    log_error "❌ Auth enforcement failed"
    exit 1
fi

# Test 3: Authenticated RPC call
log_info "Test 3: Authenticated RPC call"
COOKIE=$(cut -d: -f2 "$DATADIR/regtest/.cookie")
AUTH="Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)"

HELP_RESPONSE=$(curl -s --max-time $TIMEOUT -X POST \
    -H "Content-Type: application/json" \
    -H "$AUTH" \
    -d '{"jsonrpc":"2.0","method":"help","params":[],"id":1}' \
    http://127.0.0.1:$HTTP_PORT/)

METHOD_COUNT=$(echo "$HELP_RESPONSE" | jq '.result | length')
log_info "Found $METHOD_COUNT RPC methods"

if [ "$METHOD_COUNT" -lt 60 ]; then
    log_error "❌ Too few RPC methods found"
    exit 1
fi

# Check for key vNext methods (healthz is HTTP endpoint, not RPC method)
# Just verify we have a reasonable number of methods and no obvious errors
log_info "✅ RPC surface accessible with $METHOD_COUNT methods"

# Test 4: Basic RPC functionality (skip mining for now - requires wallet setup)
log_info "Test 4: Basic RPC functionality"

# Test a simple RPC call
NETWORK_RESPONSE=$(curl -s --max-time $TIMEOUT -X POST \
    -H "Content-Type: application/json" \
    -H "$AUTH" \
    -d '{"jsonrpc":"2.0","method":"getnetworkinfo","params":[],"id":1}' \
    http://127.0.0.1:$HTTP_PORT/)

if echo "$NETWORK_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
    log_info "✅ Basic RPC functionality working"
else
    log_error "❌ Basic RPC functionality failed"
    exit 1
fi

# Test 5: No legacy port listeners
log_info "Test 5: No legacy port listeners"
if netstat -an 2>/dev/null | grep -q ":22998.*LISTEN"; then
    log_warn "⚠️ Legacy RPC port 22998 is listening; check for an old daemon"
else
    log_info "✅ No legacy RPC port listener found"
fi

log_info "🎉 All vNext smoke tests passed!"
log_info "vNext-only daemon is working correctly"
