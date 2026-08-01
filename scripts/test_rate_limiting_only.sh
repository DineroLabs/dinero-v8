#!/usr/bin/env bash
set -euo pipefail

# Quick Rate Limiting Test
DATADIR="./test-data/rate-limit"
PORT="20999"

echo "⚡ TESTING RATE LIMITING ONLY"
echo "============================"

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
  --data-binary '{"jsonrpc":"2.0","id":"create","method":"wallet.create","params":{"name":"test"}}' \
  http://127.0.0.1:$PORT/ > /dev/null

curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"load","method":"wallet.load","params":{"name":"test"}}' \
  http://127.0.0.1:$PORT/ > /dev/null

echo "Testing 5 failed attempts followed by rate limiting..."

for i in {1..5}; do
    echo -n "Attempt $i: "
    response=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
      --data-binary '{"jsonrpc":"2.0","id":"test'$i'","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"VvIZ8/3ZpqRzLbJzT9P8Ow==","cipher":"aes-256-gcm","iv":"AZiU8/3ZpqRzLbJz","ct":"HZiU8/3ZpqRzLbJzT9P8OwHZiU8/3ZpqRzLbJzT9P8Ow","tag":"HZiU8/3ZpqRzLbJzT9P8Ow==","passphrase":"wrong_'$i'"}}' \
      http://127.0.0.1:$PORT/)
    
    error=$(echo "$response" | jq -r '.error.message? // .error // .result.error.message? // .result.error // "none"')
    echo "$error"
done

echo -n "Attempt 6 (should be rate limited): "
response=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":"test6","method":"wallet.importencryptedkey","params":{"enc":"pbkdf2-hmac-sha256","iter":100000,"salt":"VvIZ8/3ZpqRzLbJzT9P8Ow==","cipher":"aes-256-gcm","iv":"AZiU8/3ZpqRzLbJz","ct":"HZiU8/3ZpqRzLbJzT9P8OwHZiU8/3ZpqRzLbJzT9P8Ow","tag":"HZiU8/3ZpqRzLbJzT9P8Ow==","passphrase":"wrong_6"}}' \
  http://127.0.0.1:$PORT/)

error=$(echo "$response" | jq -r '.error.message? // .error // .result.error.message? // .result.error // "none"')
echo "$error"

if [[ "$error" == "RATE_LIMITED" ]]; then
    echo "✅ Rate limiting working correctly!"
else
    echo "❌ Rate limiting failed - expected RATE_LIMITED, got: $error"
fi
