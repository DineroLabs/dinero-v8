#!/usr/bin/env bash
set -euo pipefail

DATADIR="./test-data/debug-responses"
PORT="20999"

echo "🔍 Debugging actual responses..."

# Clean start
pkill -f dinerod >/dev/null 2>&1 || true
rm -rf "$DATADIR" && mkdir -p "$DATADIR"

# Start daemon
./build/dinerod --regtest --datadir="$DATADIR" --printtoconsole > "$DATADIR/daemon.log" 2>&1 &
DAEMON_PID=$!
trap 'kill $DAEMON_PID >/dev/null 2>&1 || true' EXIT
sleep 2

AUTH=$(cat "$DATADIR/regtest/.cookie")

# Create wallet
curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"create","method":"wallet.create","params":{"name":"debug"}}' \
  http://127.0.0.1:$PORT/ >/dev/null

curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"load","method":"wallet.load","params":{"name":"debug"}}' \
  http://127.0.0.1:$PORT/ >/dev/null

echo "Testing specific cases to see actual responses..."
echo ""

# Test 1: Short hex
echo "1. Short hex response:"
SHORT_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"short","method":"wallet.importprivatekey","params":{"privkey":"0123456789abcdef"}}' \
  http://127.0.0.1:$PORT/)
echo "$SHORT_RESULT" | jq .
echo ""

# Test 2: Invalid hex characters
echo "2. Invalid hex characters response:"
INVALID_HEX_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"invalid_hex","method":"wallet.importprivatekey","params":{"privkey":"0123456789abcdefGHIJKLMNOPQRSTUVWXYZ0123456789abcdef0123456789abcdef"}}' \
  http://127.0.0.1:$PORT/)
echo "$INVALID_HEX_RESULT" | jq .
echo ""

# Test 3: Empty key
echo "3. Empty key response:"
EMPTY_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"empty","method":"wallet.importprivatekey","params":{"privkey":""}}' \
  http://127.0.0.1:$PORT/)
echo "$EMPTY_RESULT" | jq .
echo ""

# Test 4: Missing parameter
echo "4. Missing parameter response:"
MISSING_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"missing","method":"wallet.importprivatekey","params":{"label":"Missing Key"}}' \
  http://127.0.0.1:$PORT/)
echo "$MISSING_RESULT" | jq .
echo ""
