#!/usr/bin/env bash
set -euo pipefail

DATADIR="./test-data/wif-debug"
PORT="20999"

echo "🔍 Debugging WIF validation..."

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

# Test simple cases
echo "Testing various inputs..."

# Test 1: Valid hex (should work)
echo "1. Testing hex key..."
HEX_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"hex","method":"wallet.importprivatekey","params":{"privkey":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}}' \
  http://127.0.0.1:$PORT/)
echo "$HEX_RESULT" | jq '.result'

# Test 2: Invalid format (should fail with specific error)
echo "2. Testing invalid format..."
INVALID_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"invalid","method":"wallet.importprivatekey","params":{"privkey":"invalid"}}' \
  http://127.0.0.1:$PORT/)
echo "$INVALID_RESULT" | jq '.result'

# Test 3: Short hex (should fail)
echo "3. Testing short hex..."
SHORT_RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"short","method":"wallet.importprivatekey","params":{"privkey":"0123456789abcdef"}}' \
  http://127.0.0.1:$PORT/)
echo "$SHORT_RESULT" | jq '.result'
