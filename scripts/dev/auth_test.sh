#!/usr/bin/env bash
# Authentication test - verify RPC requires auth by default and allows --dev-rpc-open
set -euo pipefail

DATADIR="${1:-./test_auth}"
RPC_PORT="${2:-22000}"
ADMIN_PORT="${3:-22001}"
BIN="${BIN:-./build/dinerod}"

echo "=== Authentication Test ==="
echo ""

# Clean slate
rm -rf "$DATADIR"
mkdir -p "$DATADIR"
pkill -f dinerod || true

# === TEST 1: Default (Auth Required) ===
echo "=== Test 1: Auth Required (Default) ==="
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPC_PORT" --adminport="$ADMIN_PORT" --printtoconsole > "$DATADIR/run1.log" 2>&1 &
DAEMON_PID=$!
sleep 3

NODEINFO="$DATADIR/nodeinfo.json"
if [[ ! -f "$NODEINFO" ]]; then
    echo "❌ nodeinfo.json not found"
    kill -TERM $DAEMON_PID 2>/dev/null || true
    exit 1
fi

ACTUAL_RPC_PORT=$(jq -r '.rpc.port' "$NODEINFO")
COOKIE_FILE=$(jq -r '.rpc.cookie_file' "$NODEINFO")
RPC_URL="http://127.0.0.1:$ACTUAL_RPC_PORT/"

# Test without auth (should get 401)
NO_AUTH_CODE=$(curl -s -o /dev/null -w '%{http_code}' -H 'content-type: application/json' \
  "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"help","params":[]}' || echo "000")

echo "Without auth: HTTP $NO_AUTH_CODE"

# Test with auth (should get 200)
if [[ -f "$COOKIE_FILE" ]]; then
    AUTH="--user $(cat "$COOKIE_FILE")"
    WITH_AUTH_CODE=$(curl -s -o /dev/null -w '%{http_code}' -H 'content-type: application/json' \
      $AUTH "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"help","params":[]}' || echo "000")
    echo "With auth: HTTP $WITH_AUTH_CODE"
else
    echo "❌ Cookie file not found: $COOKIE_FILE"
    WITH_AUTH_CODE="000"
fi

# Stop daemon
kill -TERM $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true
sleep 1

# === TEST 2: Dev Mode (Auth Disabled) ===
echo ""
echo "=== Test 2: Dev Mode (--dev-rpc-open) ==="
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPC_PORT" --adminport="$ADMIN_PORT" --dev-rpc-open --printtoconsole > "$DATADIR/run2.log" 2>&1 &
DAEMON_PID=$!
sleep 3

# Test without auth in dev mode (should get 200)
DEV_NO_AUTH_CODE=$(curl -s -o /dev/null -w '%{http_code}' -H 'content-type: application/json' \
  "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"help","params":[]}' || echo "000")

echo "Dev mode without auth: HTTP $DEV_NO_AUTH_CODE"

# Stop daemon
kill -TERM $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true

# === VERIFICATION ===
echo ""
echo "=== Verification ==="

EXIT_CODE=0

# Default mode should require auth
if [[ "$NO_AUTH_CODE" == "401" ]]; then
    echo "✅ Default mode auth enforcement: PASS (401 without auth)"
else
    echo "❌ Default mode auth enforcement: FAIL (expected 401, got $NO_AUTH_CODE)"
    EXIT_CODE=1
fi

if [[ "$WITH_AUTH_CODE" == "200" ]]; then
    echo "✅ Default mode with auth: PASS (200 with cookie)"
else
    echo "❌ Default mode with auth: FAIL (expected 200, got $WITH_AUTH_CODE)"
    EXIT_CODE=1
fi

# Dev mode should allow no auth
if [[ "$DEV_NO_AUTH_CODE" == "200" ]]; then
    echo "✅ Dev mode no auth: PASS (200 without auth)"
else
    echo "❌ Dev mode no auth: FAIL (expected 200, got $DEV_NO_AUTH_CODE)"
    EXIT_CODE=1
fi

echo ""
echo "=== Authentication Test Complete ==="
if [[ $EXIT_CODE -eq 0 ]]; then
    echo "🎉 All authentication checks PASSED"
else
    echo "💥 Some authentication checks FAILED"
fi

exit $EXIT_CODE
