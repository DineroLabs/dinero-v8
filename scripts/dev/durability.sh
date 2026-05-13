#!/usr/bin/env bash
# Durability test - verify blockchain state persists across restarts
set -euo pipefail

DATADIR="${1:-./test_durability}"
RPC_PORT="${2:-22000}"
ADMIN_PORT="${3:-22001}"
BIN="${BIN:-./build/dinerod}"

echo "=== Durability Test ==="
echo "DATADIR: $DATADIR"
echo "Testing blockchain persistence across daemon restarts..."
echo ""

# Clean slate
rm -rf "$DATADIR"
mkdir -p "$DATADIR"
pkill -f dinerod || true

# === FIRST START ===
echo "=== First Start ==="
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPC_PORT" --adminport="$ADMIN_PORT" --printtoconsole > "$DATADIR/run1.log" 2>&1 &
DAEMON_PID=$!
sleep 3

# Get initial state
NODEINFO="$DATADIR/nodeinfo.json"
if [[ ! -f "$NODEINFO" ]]; then
    echo "❌ nodeinfo.json not found"
    kill -TERM $DAEMON_PID 2>/dev/null || true
    exit 1
fi

ACTUAL_RPC_PORT=$(jq -r '.rpc.port' "$NODEINFO")
ACTUAL_HTTP_PORT=$(jq -r '.http.port' "$NODEINFO")
COOKIE_FILE=$(jq -r '.rpc.cookie_file' "$NODEINFO")

if [[ ! -f "$COOKIE_FILE" ]]; then
    echo "❌ Cookie file not found: $COOKIE_FILE"
    kill -TERM $DAEMON_PID 2>/dev/null || true
    exit 1
fi

AUTH="--user $(cat "$COOKIE_FILE")"
RPC_URL="http://127.0.0.1:$ACTUAL_RPC_PORT/"

# Get first template
TEMPLATE1=$(curl -s -H 'content-type: application/json' $AUTH "$RPC_URL" -d '{"jsonrpc":"2.0","id":1,"method":"getblocktemplate","params":[]}')
HEIGHT1=$(echo "$TEMPLATE1" | jq -r '.result.height // "null"')
BITS1=$(echo "$TEMPLATE1" | jq -r '.result.bits // "null"')
SUBSIDY1=$(echo "$TEMPLATE1" | jq -r '.result.subsidy_din // "null"')

echo "First start - Height: $HEIGHT1, Bits: $BITS1, Subsidy: $SUBSIDY1"

if [[ "$HEIGHT1" == "null" || "$HEIGHT1" == "" ]]; then
    echo "❌ Failed to get height from first start"
    kill -TERM $DAEMON_PID 2>/dev/null || true
    exit 1
fi

# Stop daemon
echo "Stopping daemon..."
kill -TERM $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true
sleep 1

# === SECOND START (RESTART) ===
echo ""
echo "=== Restart (Same Datadir) ==="
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPC_PORT" --adminport="$ADMIN_PORT" --printtoconsole > "$DATADIR/run2.log" 2>&1 &
DAEMON_PID=$!
sleep 3

# Get restarted state
TEMPLATE2=$(curl -s -H 'content-type: application/json' $AUTH "$RPC_URL" -d '{"jsonrpc":"2.0","id":2,"method":"getblocktemplate","params":[]}')
HEIGHT2=$(echo "$TEMPLATE2" | jq -r '.result.height // "null"')
BITS2=$(echo "$TEMPLATE2" | jq -r '.result.bits // "null"')
SUBSIDY2=$(echo "$TEMPLATE2" | jq -r '.result.subsidy_din // "null"')

echo "After restart - Height: $HEIGHT2, Bits: $BITS2, Subsidy: $SUBSIDY2"

# === VERIFICATION ===
echo ""
echo "=== Verification ==="

EXIT_CODE=0

# Height should be unchanged
if [[ "$HEIGHT1" == "$HEIGHT2" ]]; then
    echo "✅ Height persistence: PASS ($HEIGHT1 == $HEIGHT2)"
else
    echo "❌ Height persistence: FAIL ($HEIGHT1 != $HEIGHT2)"
    EXIT_CODE=1
fi

# Bits should be unchanged
if [[ "$BITS1" == "$BITS2" ]]; then
    echo "✅ Bits consistency: PASS ($BITS1 == $BITS2)"
else
    echo "❌ Bits consistency: FAIL ($BITS1 != $BITS2)"
    EXIT_CODE=1
fi

# Subsidy should be unchanged
if [[ "$SUBSIDY1" == "$SUBSIDY2" ]]; then
    echo "✅ Subsidy consistency: PASS ($SUBSIDY1 == $SUBSIDY2)"
else
    echo "❌ Subsidy consistency: FAIL ($SUBSIDY1 != $SUBSIDY2)"
    EXIT_CODE=1
fi

# Height should be >= 2 (genesis + premine)
if [[ "$HEIGHT2" -ge 2 ]]; then
    echo "✅ Height sanity: PASS (height >= 2)"
else
    echo "❌ Height sanity: FAIL (height < 2)"
    EXIT_CODE=1
fi

# Cleanup
kill -TERM $DAEMON_PID 2>/dev/null || true
wait $DAEMON_PID 2>/dev/null || true

echo ""
echo "=== Durability Test Complete ==="
if [[ $EXIT_CODE -eq 0 ]]; then
    echo "🎉 All durability checks PASSED"
else
    echo "💥 Some durability checks FAILED"
fi

exit $EXIT_CODE
