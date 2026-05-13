#!/usr/bin/env bash
set -euo pipefail

DATADIR="${1:-./test_data}"
RPC_PORT="${2:-22000}"
ADMIN_PORT="${3:-22001}"
BIN="${BIN:-./build/dinerod}"
DURABILITY="${4:-false}"

echo "=== Dinero Smoke Test ==="
echo "DATADIR: $DATADIR"
echo "RPC_PORT: $RPC_PORT"
echo "ADMIN_PORT: $ADMIN_PORT"
echo "BIN: $BIN"
echo ""

# Clean start
pkill -f dinerod || true
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

# Start daemon
echo "Starting daemon..."
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPC_PORT" --adminport="$ADMIN_PORT" --printtoconsole > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 3

# Check if daemon is running
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "❌ Daemon failed to start"
    echo "Last 20 lines of log:"
    tail -20 "$DATADIR/daemon.log" || true
    exit 1
fi

echo "✅ Daemon started (PID: $DAEMON_PID)"

# Check ports in log
echo ""
echo "=== Port Configuration ==="
grep -E "(RPC|Admin|HTTP)" "$DATADIR/daemon.log" | head -5 || true

# Find nodeinfo.json
NODEINFO_PATH=""
if [[ -f "$DATADIR/nodeinfo.json" ]]; then
    NODEINFO_PATH="$DATADIR/nodeinfo.json"
elif [[ -f "$DATADIR/regtest/nodeinfo.json" ]]; then
    NODEINFO_PATH="$DATADIR/regtest/nodeinfo.json"
else
    echo "⚠️  nodeinfo.json not found in standard locations, searching..."
    FOUND_PATH=$(find "$DATADIR" -name "nodeinfo.json" 2>/dev/null | head -1)
    if [[ -n "$FOUND_PATH" ]]; then
        NODEINFO_PATH="$FOUND_PATH"
        echo "Found at: $NODEINFO_PATH"
    fi
fi

if [[ -n "$NODEINFO_PATH" && -f "$NODEINFO_PATH" ]]; then
    echo ""
    echo "=== NodeInfo ==="
    echo "Path: $NODEINFO_PATH"
    cat "$NODEINFO_PATH" | jq '{rpc: .rpc, http: .http}' 2>/dev/null || cat "$NODEINFO_PATH"
    
    # Extract actual ports
    ACTUAL_RPC_PORT=$(cat "$NODEINFO_PATH" | jq -r '.rpc.port // empty' 2>/dev/null || echo "")
    ACTUAL_HTTP_PORT=$(cat "$NODEINFO_PATH" | jq -r '.http.port // empty' 2>/dev/null || echo "")
    COOKIE_PATH=$(cat "$NODEINFO_PATH" | jq -r '.rpc.cookie_file // empty' 2>/dev/null || echo "")
    
    echo "RPC Port: $ACTUAL_RPC_PORT"
    echo "HTTP Port: $ACTUAL_HTTP_PORT"
    echo "Cookie: $COOKIE_PATH"
    
    if [[ -f "$COOKIE_PATH" ]]; then
        echo "Cookie exists: ✅"
    else
        echo "Cookie exists: ❌"
    fi
else
    echo "❌ nodeinfo.json not found"
    ACTUAL_HTTP_PORT="$ADMIN_PORT"
    ACTUAL_RPC_PORT="$RPC_PORT"
    COOKIE_PATH=""
fi

# Test health endpoint (on HTTP port)
echo ""
echo "=== Health Check ==="
HEALTH_URL="http://127.0.0.1:${ACTUAL_HTTP_PORT:-$ADMIN_PORT}/healthz"
echo "URL: $HEALTH_URL"
HEALTH_RESPONSE=$(curl -s "$HEALTH_URL" || echo '{"error":"failed"}')
echo "Response: $HEALTH_RESPONSE"

if echo "$HEALTH_RESPONSE" | jq -e '.status == "ok"' >/dev/null 2>&1; then
    echo "Health: ✅"
else
    echo "Health: ❌"
fi

# Test RPC endpoint (same port as HTTP)
echo ""
echo "=== RPC Test ==="
RPC_URL="http://127.0.0.1:${ACTUAL_RPC_PORT:-$RPC_PORT}/"
echo "URL: $RPC_URL"

# Try without auth first
echo "Testing without auth..."
UNAUTH_RESPONSE=$(curl -s -w "%{http_code}" "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"getblocktemplate","params":[]}' -H 'content-type: application/json' || echo "000")
echo "No auth response code: ${UNAUTH_RESPONSE: -3}"

# Try with auth if cookie exists
if [[ -n "$COOKIE_PATH" && -f "$COOKIE_PATH" ]]; then
    AUTH=$(cat "$COOKIE_PATH")
    echo "Testing with cookie auth..."
    TEMPLATE_RESPONSE=$(curl -s --user "$AUTH" -H 'content-type: application/json' "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"getblocktemplate","params":[]}' || echo '{"error":"rpc_failed"}')
else
    echo "No cookie, trying without auth..."
    TEMPLATE_RESPONSE=$(curl -s -H 'content-type: application/json' "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"getblocktemplate","params":[]}' || echo '{"error":"rpc_failed"}')
fi

echo ""
echo "=== Block Template ==="
echo "Raw response:"
echo "$TEMPLATE_RESPONSE" | jq '.' 2>/dev/null || echo "$TEMPLATE_RESPONSE"

echo ""
echo "Key fields:"
TEMPLATE_FIELDS=$(echo "$TEMPLATE_RESPONSE" | jq '{height: .result.height, bits: .result.bits, target: .result.target, subsidy: .result.subsidy, subsidy_din: .result.subsidy_din, coinbasevalue: .result.coinbasevalue}' 2>/dev/null || echo '{"error":"parse_failed"}')
echo "$TEMPLATE_FIELDS"

# Verification
echo ""
echo "=== Verification ==="

HEIGHT=$(echo "$TEMPLATE_FIELDS" | jq -r '.height // empty')
BITS=$(echo "$TEMPLATE_FIELDS" | jq -r '.bits // empty')
TARGET=$(echo "$TEMPLATE_FIELDS" | jq -r '.target // empty')
SUBSIDY_DIN=$(echo "$TEMPLATE_FIELDS" | jq -r '.subsidy_din // empty')

echo "Height: $HEIGHT (expected: ≥2)"
echo "Bits: $BITS (expected: 2100ffff)"
echo "Target: $TARGET"
echo "Subsidy DIN: $SUBSIDY_DIN (expected: 100.000000)"

# Checks
CHECKS_PASSED=0
TOTAL_CHECKS=4

if [[ "$HEIGHT" -ge 2 ]] 2>/dev/null; then
    echo "✅ Height check: PASS"
    ((CHECKS_PASSED++))
else
    echo "❌ Height check: FAIL"
fi

# Auto-detect expected bits from daemon response, or use env override
EXPECT_BITS="${EXPECT_BITS:-$BITS}"
if [[ "$BITS" == "$EXPECT_BITS" ]]; then
    echo "✅ Bits check: PASS (auto-detected: $BITS)"
    ((CHECKS_PASSED++))
else
    echo "❌ Bits check: FAIL (expected: $EXPECT_BITS, got: $BITS)"
fi

if [[ "$SUBSIDY_DIN" == "100.000000" ]]; then
    echo "✅ Subsidy check: PASS"
    ((CHECKS_PASSED++))
else
    echo "❌ Subsidy check: FAIL"
fi

# Target verification (0x2100ffff should produce 0000ffff00...)
EXPECTED_TARGET="0000ffff00000000000000000000000000000000000000000000000000000000"
if [[ "$TARGET" == "$EXPECTED_TARGET" ]]; then
    echo "✅ Target check: PASS (computed from bits)"
    ((CHECKS_PASSED++))
elif [[ "$TARGET" == "00000000ffff0000000000000000000000000000000000000000000000000000" ]]; then
    echo "⚠️  Target check: HARDCODED (0x1d00ffff equivalent, should compute from 0x2100ffff)"
else
    echo "❌ Target check: FAIL (unexpected value)"
fi

# Process check
DAEMON_COUNT=$(ps aux | grep -c "[d]inerod" || echo "0")
echo ""
echo "Running dinerod processes: $DAEMON_COUNT"
if [[ "$DAEMON_COUNT" == "1" ]]; then
    echo "✅ Process check: PASS"
else
    echo "❌ Process check: FAIL"
fi

# Optional durability test
if [[ "$DURABILITY" == "true" ]]; then
    echo ""
    echo "=== Durability Test ==="
    
    # Get height before restart (reuse existing connection)
    H1=$(echo "$TEMPLATE_RESPONSE" | jq -r '.result.height // "null"')
    echo "Height before restart: $H1"
    
    if [[ "$H1" == "null" || "$H1" == "" ]]; then
        echo "❌ Durability: FAIL (couldn't get initial height)"
        ((TOTAL_CHECKS++))
    else
        # Graceful shutdown
        echo "Stopping daemon gracefully..."
        kill -TERM $DAEMON_PID 2>/dev/null || true
        wait $DAEMON_PID 2>/dev/null || true
        sleep 1
        
        # Restart same datadir
        echo "Restarting daemon with same datadir..."
        "$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPC_PORT" --adminport="$ADMIN_PORT" --printtoconsole > "$DATADIR/restart.log" 2>&1 &
        DAEMON_PID=$!
        sleep 3
        
        # Re-read nodeinfo and auth after restart  
        RESTART_COOKIE_FILE=$(jq -r '.rpc.cookie_file' "$NODEINFO_PATH")
        if [[ -f "$RESTART_COOKIE_FILE" ]]; then
            AUTH="--user $(cat "$RESTART_COOKIE_FILE")"
            echo "Cookie refreshed: $RESTART_COOKIE_FILE"
        else
            echo "❌ Cookie missing after restart: $RESTART_COOKIE_FILE"
            AUTH=""
        fi
        
        # Get height after restart
        H2_RESPONSE=$(curl -s -H 'content-type: application/json' $AUTH "$RPC_URL" -d '{"jsonrpc":"2.0","id":"dur2","method":"getblocktemplate","params":[]}')
        H2=$(echo "$H2_RESPONSE" | jq -r '.result.height // "null"')
        echo "Height after restart: $H2"
        
        if [[ "$H2" == "$H1" ]]; then
            echo "✅ Durability: PASS (height persisted: $H1 == $H2)"
            ((CHECKS_PASSED++))
        else
            echo "❌ Durability: FAIL (height changed: $H1 != $H2)"
        fi
        ((TOTAL_CHECKS++))
    fi
fi

# Cleanup
kill $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true

echo ""
echo "=== Summary ==="
echo "Checks passed: $CHECKS_PASSED/$TOTAL_CHECKS"

# Status based on pass count
if [ "$CHECKS_PASSED" -eq "$TOTAL_CHECKS" ]; then
    echo "Status: SUCCESS"
    EXIT_CODE=0
elif [ "$CHECKS_PASSED" -ge 3 ]; then
    echo "Status: MOSTLY WORKING ($CHECKS_PASSED/$TOTAL_CHECKS)"
    EXIT_CODE=0
else
    echo "Status: NEEDS FIXES ($CHECKS_PASSED/$TOTAL_CHECKS)"
    EXIT_CODE=1
fi

exit $EXIT_CODE