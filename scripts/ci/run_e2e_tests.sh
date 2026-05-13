#!/bin/bash
set -e

# CI Integration Script for DineroCoin E2E RPC Tests
# This script starts the daemon, runs comprehensive Python E2E tests, and cleans up

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEST_DIR="$PROJECT_ROOT/tests/e2e"
BUILD_DIR="$PROJECT_ROOT/build"

# Configuration
RPC_PORT="${RPC_PORT:-20996}"
WS_PORT="${WS_PORT:-18881}"
DAEMON_TIMEOUT=30
TEST_TIMEOUT=120

echo "🚀 Starting DineroCoin E2E RPC Test Suite"
echo "Project root: $PROJECT_ROOT"
echo "Build directory: $BUILD_DIR"

# Check if daemon binary exists
DAEMON_BIN="$BUILD_DIR/bin/dinerod"
if [ ! -f "$DAEMON_BIN" ]; then
    echo "❌ Error: dinerod binary not found at $DAEMON_BIN"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

# Check if Python test exists
TEST_SCRIPT="$TEST_DIR/test_wallet_mining_rpc.py"
if [ ! -f "$TEST_SCRIPT" ]; then
    echo "❌ Error: E2E test script not found at $TEST_SCRIPT"
    exit 1
fi

# Create test data directory
TEST_DATA_DIR="$PROJECT_ROOT/test-data-ci"
mkdir -p "$TEST_DATA_DIR"

# Cleanup function
cleanup() {
    echo "🧹 Cleaning up..."
    if [ ! -z "$DAEMON_PID" ]; then
        echo "Stopping daemon (PID: $DAEMON_PID)"
        kill $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
    fi
    
    # Clean up test data
    if [ -d "$TEST_DATA_DIR" ]; then
        rm -rf "$TEST_DATA_DIR"
    fi
}

# Set trap for cleanup
trap cleanup EXIT INT TERM

# Start daemon in regtest mode
echo "🔧 Starting daemon in regtest mode..."
cd "$PROJECT_ROOT"

"$DAEMON_BIN" \
    --regtest \
    --datadir="$TEST_DATA_DIR" \
    --rpcport=$RPC_PORT \
    --wsport=$WS_PORT \
    --rpcallowip=127.0.0.1 \
    --daemon \
    --autowallet=test_wallet &

DAEMON_PID=$!
echo "Daemon started with PID: $DAEMON_PID"

# Wait for daemon to be ready
echo "⏳ Waiting for daemon to be ready..."
READY=false
for i in $(seq 1 $DAEMON_TIMEOUT); do
    if curl -s --connect-timeout 1 "http://127.0.0.1:$RPC_PORT/" >/dev/null 2>&1; then
        READY=true
        break
    fi
    echo "  Attempt $i/$DAEMON_TIMEOUT..."
    sleep 1
done

if [ "$READY" = false ]; then
    echo "❌ Error: Daemon failed to start within $DAEMON_TIMEOUT seconds"
    exit 1
fi

# Verify RPC health
echo "🔍 Checking RPC health..."
HEALTH_RESPONSE=$(curl -s -X POST \
    -H "Content-Type: application/json" \
    -d '{"method":"rpc.health","params":[],"id":1}' \
    "http://127.0.0.1:$RPC_PORT/" || echo "")

if echo "$HEALTH_RESPONSE" | grep -q '"status":"healthy"'; then
    echo "✅ RPC health check passed"
else
    echo "❌ RPC health check failed"
    echo "Response: $HEALTH_RESPONSE"
    exit 1
fi

# Run Python E2E tests
echo "🧪 Running Python E2E tests..."
cd "$TEST_DIR"

# Set environment variables for the test
export RPC_PORT=$RPC_PORT
export RPC_HOST="127.0.0.1"
export TEST_DATA_DIR="$TEST_DATA_DIR"

# Run the test with timeout
timeout $TEST_TIMEOUT python3 "$TEST_SCRIPT" || {
    echo "❌ E2E tests failed or timed out"
    exit 1
}

echo "✅ All E2E tests passed successfully!"
echo "🎉 CI integration complete"
