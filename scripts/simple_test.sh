#!/usr/bin/env bash
set -euo pipefail

DATADIR="./test-data/simple"
PORT="20999"

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
  --data-binary '{"jsonrpc":"2.0","id":"create","method":"wallet.create","params":{"name":"simple"}}' \
  http://127.0.0.1:$PORT/ >/dev/null

curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"load","method":"wallet.load","params":{"name":"simple"}}' \
  http://127.0.0.1:$PORT/ >/dev/null

echo "Simple test of one invalid case..."

# Test invalid hex
RESULT=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"test","method":"wallet.importprivatekey","params":{"privkey":"short"}}' \
  http://127.0.0.1:$PORT/)

echo "Raw result:"
echo "$RESULT"
echo ""

echo "Success field:"
echo "$RESULT" | jq -r '.result.success'
echo ""

echo "Testing with grep:"
if echo "$RESULT" | jq -r '.result.success' | grep -q "false"; then
    echo "✅ Correctly detected false"
else
    echo "❌ Failed to detect false"
fi
