#!/bin/bash
# test_metrics_build_info.sh - Test that build info is exposed in getmetrics

set -e

DAEMON="./build/dinerod"
RPC_PORT=20998
MAX_WAIT=15
DATA_DIR="./data-test-metrics"  # Use separate test data directory

echo "🧪 Testing getmetrics build info exposure..."
echo ""

# Kill any existing daemon
pkill -f dinerod || true
sleep 2

# Clean test data directory
rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"

# Start daemon in background
echo "🚀 Starting daemon..."
$DAEMON -regtest -datadir="$DATA_DIR" -rpcport=$RPC_PORT -wsport=21000 -port=20999 -printtoconsole > /tmp/dinerod_metrics_test.log 2>&1 &
DAEMON_PID=$!

echo "   PID: $DAEMON_PID"
echo "   Waiting for RPC server to be ready..."

# Wait for RPC server to be ready
for i in $(seq 1 $MAX_WAIT); do
    if curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
        --data-binary '{"jsonrpc":"1.0","id":"test","method":"getblockchaininfo","params":[]}' \
        -H 'content-type: text/plain;' > /dev/null 2>&1; then
        echo "✅ RPC server ready!"
        break
    fi
    if [ $i -eq $MAX_WAIT ]; then
        echo "❌ RPC server not ready after ${MAX_WAIT}s"
        echo "   Check logs: tail -f /tmp/dinerod_metrics_test.log"
        kill $DAEMON_PID 2>/dev/null || true
        exit 1
    fi
    sleep 1
done

# Wait for cookie file to be created
echo "   Waiting for cookie file..."
COOKIE_FILE=""
for i in $(seq 1 5); do
    # Check root level first (for regtest/mainnet)
    if [ -f "$DATA_DIR/.cookie" ]; then
        COOKIE_FILE="$DATA_DIR/.cookie"
        break
    elif [ -f "$DATA_DIR/regtest/.cookie" ]; then
        COOKIE_FILE="$DATA_DIR/regtest/.cookie"
        break
    elif [ -f "$DATA_DIR/mainnet/.cookie" ]; then
        COOKIE_FILE="$DATA_DIR/mainnet/.cookie"
        break
    fi
    sleep 1
done

if [ -z "$COOKIE_FILE" ] || [ ! -f "$COOKIE_FILE" ]; then
    echo "⚠️  Cookie file not found, trying without auth..."
else
    COOKIE_VALUE=$(cat "$COOKIE_FILE" 2>/dev/null || echo "")
    if [ -n "$COOKIE_VALUE" ]; then
        echo "✅ Cookie file found"
    else
        echo "⚠️  Cookie file empty, trying without auth..."
    fi
fi

sleep 1

# Test getmetrics endpoint
echo ""
echo "📊 Testing getmetrics endpoint..."

# Use cookie auth if available
if [ -n "$COOKIE_FILE" ] && [ -f "$COOKIE_FILE" ]; then
    COOKIE_VALUE=$(cat "$COOKIE_FILE" 2>/dev/null | tr -d '\n\r' || echo "")
    if [ -n "$COOKIE_VALUE" ]; then
        echo "   Using cookie authentication..."
        METRICS_RESPONSE=$(curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
            --user "$COOKIE_VALUE" \
            --data-binary '{"jsonrpc":"1.0","id":"test","method":"getmetrics","params":[]}' \
            -H 'content-type: text/plain;')
    else
        echo "⚠️  Cookie file empty, trying without auth..."
        METRICS_RESPONSE=$(curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
            --data-binary '{"jsonrpc":"1.0","id":"test","method":"getmetrics","params":[]}' \
            -H 'content-type: text/plain;')
    fi
else
    echo "⚠️  No cookie file, trying without auth..."
    METRICS_RESPONSE=$(curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
        --data-binary '{"jsonrpc":"1.0","id":"test","method":"getmetrics","params":[]}' \
        -H 'content-type: text/plain;')
fi

if [ -z "$METRICS_RESPONSE" ]; then
    echo "❌ No response from getmetrics"
    kill $DAEMON_PID 2>/dev/null || true
    exit 1
fi

# Check if it's an error response (error field exists and is not null)
HAS_ERROR=$(echo "$METRICS_RESPONSE" | python3 -c "import sys, json; d=json.load(sys.stdin); err=d.get('error'); print('1' if err is not None and err != {} and err != [] else '0')" 2>/dev/null || echo "0")
if [ "$HAS_ERROR" = "1" ]; then
    echo "❌ Error response:"
    echo "$METRICS_RESPONSE" | python3 -m json.tool 2>/dev/null || echo "$METRICS_RESPONSE"
    kill $DAEMON_PID 2>/dev/null || true
    exit 1
fi

# Extract metrics text (it's in a JSON field)
METRICS_TEXT=$(echo "$METRICS_RESPONSE" | python3 -c "import sys, json; d=json.load(sys.stdin); print(d.get('result', {}).get('metrics', ''))" 2>/dev/null || echo "")

if [ -z "$METRICS_TEXT" ]; then
    echo "⚠️  Could not parse metrics JSON, showing raw response:"
    echo "$METRICS_RESPONSE" | head -20
    kill $DAEMON_PID 2>/dev/null || true
    exit 1
fi

echo ""
echo "✅ Metrics retrieved successfully!"
echo ""

# Check for build info
echo "🔍 Checking for build info metrics..."
echo ""

FOUND_BUILD_INFO=0
FOUND_CONSENSUS_INFO=0
FOUND_VERSION_INFO=0

if echo "$METRICS_TEXT" | grep -q "dinero_build_info"; then
    echo "✅ dinero_build_info found:"
    echo "$METRICS_TEXT" | grep "dinero_build_info" | head -1
    FOUND_BUILD_INFO=1
    echo ""
else
    echo "❌ dinero_build_info NOT found in metrics"
    echo ""
fi

if echo "$METRICS_TEXT" | grep -q "dinero_consensus_info"; then
    echo "✅ dinero_consensus_info found:"
    echo "$METRICS_TEXT" | grep "dinero_consensus_info" | head -1
    FOUND_CONSENSUS_INFO=1
    echo ""
else
    echo "❌ dinero_consensus_info NOT found in metrics"
    echo ""
fi

if echo "$METRICS_TEXT" | grep -q "dinero_version_info"; then
    echo "✅ dinero_version_info found:"
    echo "$METRICS_TEXT" | grep "dinero_version_info" | head -1
    FOUND_VERSION_INFO=1
    echo ""
else
    echo "❌ dinero_version_info NOT found in metrics"
    echo ""
fi

# Show full build info line
echo "📋 Full dinero_build_info metric:"
BUILD_INFO_LINE=$(echo "$METRICS_TEXT" | grep "dinero_build_info" || echo "")
if [ -n "$BUILD_INFO_LINE" ]; then
    echo "$BUILD_INFO_LINE"
    echo ""
    # Extract individual fields
    echo "   Breakdown:"
    echo "$BUILD_INFO_LINE" | grep -oP 'commit="\K[^"]+' | sed 's/^/     commit: /' || true
    echo "$BUILD_INFO_LINE" | grep -oP 'version="\K[^"]+' | sed 's/^/     version: /' || true
    echo "$BUILD_INFO_LINE" | grep -oP 'checksum="\K[^"]+' | sed 's/^/     checksum: /' || true
else
    echo "   (not found)"
fi
echo ""

# Summary
echo "═══════════════════════════════════════════════════════"
echo "📊 Test Summary:"
echo "═══════════════════════════════════════════════════════"
if [ $FOUND_BUILD_INFO -eq 1 ] && [ $FOUND_CONSENSUS_INFO -eq 1 ] && [ $FOUND_VERSION_INFO -eq 1 ]; then
    echo "✅ All metrics found - Test PASSED!"
    EXIT_CODE=0
else
    echo "❌ Some metrics missing - Test FAILED"
    echo "   Build info: $([ $FOUND_BUILD_INFO -eq 1 ] && echo '✅' || echo '❌')"
    echo "   Consensus info: $([ $FOUND_CONSENSUS_INFO -eq 1 ] && echo '✅' || echo '❌')"
    echo "   Version info: $([ $FOUND_VERSION_INFO -eq 1 ] && echo '✅' || echo '❌')"
    EXIT_CODE=1
fi
echo ""

# Cleanup
echo "🧹 Stopping daemon..."
kill $DAEMON_PID 2>/dev/null || true
sleep 1
pkill -f dinerod || true

exit $EXIT_CODE

echo "🧪 Testing getmetrics build info exposure..."
echo ""

# Kill any existing daemon
pkill -f dinerod || true
sleep 2

# Clean test data directory
rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"

# Start daemon in background
echo "🚀 Starting daemon..."
$DAEMON -regtest -dev -datadir="$DATA_DIR" -rpcport=$RPC_PORT -wsport=21000 -port=20999 -printtoconsole > /tmp/dinerod_metrics_test.log 2>&1 &
DAEMON_PID=$!

echo "   PID: $DAEMON_PID"
echo "   Waiting for RPC server to be ready..."

# Wait for RPC server to be ready
for i in $(seq 1 $MAX_WAIT); do
    if curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
        --data-binary '{"jsonrpc":"1.0","id":"test","method":"getblockchaininfo","params":[]}' \
        -H 'content-type: text/plain;' > /dev/null 2>&1; then
        echo "✅ RPC server ready!"
        break
    fi
    if [ $i -eq $MAX_WAIT ]; then
        echo "❌ RPC server not ready after ${MAX_WAIT}s"
        echo "   Check logs: tail -f /tmp/dinerod_metrics_test.log"
        kill $DAEMON_PID 2>/dev/null || true
        exit 1
    fi
    sleep 1
done

sleep 1

# Test getmetrics endpoint
echo ""
echo "📊 Testing getmetrics endpoint..."

# Wait for cookie file to be created (dev mode may still create it)
COOKIE_FILE="$DATA_DIR/regtest/.cookie"
if [ ! -f "$COOKIE_FILE" ]; then
    # Try mainnet location
    COOKIE_FILE="$DATA_DIR/mainnet/.cookie"
fi

# Use cookie auth if available, otherwise rely on dev mode
COOKIE_VALUE=""
if [ -f "$COOKIE_FILE" ]; then
    COOKIE_VALUE=$(cat "$COOKIE_FILE" 2>/dev/null | tr -d '\n\r' || echo "")
fi

# Build curl command with proper auth
if [ -n "$COOKIE_VALUE" ]; then
    METRICS_RESPONSE=$(curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
        --user "$COOKIE_VALUE" \
        --data-binary '{"jsonrpc":"1.0","id":"test","method":"getmetrics","params":[]}' \
        -H 'content-type: text/plain;')
else
    # No cookie - try without auth (dev mode)
    METRICS_RESPONSE=$(curl -s -X POST http://127.0.0.1:$RPC_PORT/ \
        --data-binary '{"jsonrpc":"1.0","id":"test","method":"getmetrics","params":[]}' \
        -H 'content-type: text/plain;')
fi

if [ -z "$METRICS_RESPONSE" ]; then
    echo "❌ No response from getmetrics"
    kill $DAEMON_PID 2>/dev/null || true
    exit 1
fi

# Extract metrics text (it's in a JSON field)
METRICS_TEXT=$(echo "$METRICS_RESPONSE" | python3 -c "import sys, json; print(json.load(sys.stdin)['result']['metrics'])" 2>/dev/null || echo "")

if [ -z "$METRICS_TEXT" ]; then
    echo "⚠️  Could not parse metrics JSON, showing raw response:"
    echo "$METRICS_RESPONSE" | head -10
    kill $DAEMON_PID 2>/dev/null || true
    exit 1
fi

echo ""
echo "✅ Metrics retrieved successfully!"
echo ""

# Check for build info
echo "🔍 Checking for build info metrics..."
echo ""

if echo "$METRICS_TEXT" | grep -q "dinero_build_info"; then
    echo "✅ dinero_build_info found:"
    echo "$METRICS_TEXT" | grep "dinero_build_info" | head -1
    echo ""
else
    echo "❌ dinero_build_info NOT found in metrics"
    echo ""
fi

if echo "$METRICS_TEXT" | grep -q "dinero_consensus_info"; then
    echo "✅ dinero_consensus_info found:"
    echo "$METRICS_TEXT" | grep "dinero_consensus_info" | head -1
    echo ""
else
    echo "❌ dinero_consensus_info NOT found in metrics"
    echo ""
fi

if echo "$METRICS_TEXT" | grep -q "dinero_version_info"; then
    echo "✅ dinero_version_info found:"
    echo "$METRICS_TEXT" | grep "dinero_version_info" | head -1
    echo ""
else
    echo "❌ dinero_version_info NOT found in metrics"
    echo ""
fi

# Show full build info line
echo "📋 Full dinero_build_info metric:"
echo "$METRICS_TEXT" | grep "dinero_build_info" || echo "   (not found)"
echo ""

# Cleanup
echo "🧹 Stopping daemon..."
kill $DAEMON_PID 2>/dev/null || true
sleep 1

echo ""
echo "✅ Test complete!"

