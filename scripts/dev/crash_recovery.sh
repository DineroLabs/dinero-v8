#!/usr/bin/env bash
set -euo pipefail
DATADIR="${1:-./test_crash}"
RPCPORT="${2:-21002}"
BIN="${BIN:-./dinerod}"

echo "== Crash/Recovery durability test =="

rm -rf "$DATADIR"
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run.log" 2>&1 &
PID=$!
sleep 2

COOKIE_FILE="$DATADIR/regtest/.cookie"
AUTH="$(cat "$COOKIE_FILE")"
URL="http://127.0.0.1:$RPCPORT/"
rpc () { curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" -d "$1"; }

# Start mining to create some activity
echo "Starting mining to create blockchain activity..."
ADDR=$(rpc '{"jsonrpc":"2.0","id":"addr","method":"wallet.newaddress","params":[]}' | jq -r '.result // "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"')
rpc "{\"jsonrpc\":\"2.0\",\"id\":\"setaddr\",\"method\":\"mining.setaddress\",\"params\":[\"$ADDR\"]}" > /dev/null
rpc '{"jsonrpc":"2.0","id":"start","method":"mining.start","params":[1]}' > /dev/null

# Let it mine for a bit
sleep 3

# Get state before crash
PRE_CRASH_INFO=$(rpc '{"jsonrpc":"2.0","id":"pre","method":"getblockchaininfo","params":[]}')
PRE_CRASH_HEIGHT=$(echo "$PRE_CRASH_INFO" | jq -r '.result.blocks')
PRE_CRASH_HASH=$(echo "$PRE_CRASH_INFO" | jq -r '.result.bestblockhash')

echo "Pre-crash state: height=$PRE_CRASH_HEIGHT, hash=$PRE_CRASH_HASH"

# Simulate crash with kill -9
echo "Simulating crash with kill -9..."
kill -9 $PID
wait $PID 2>/dev/null || true
sleep 1

# Restart and check recovery
echo "Restarting after crash..."
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/recovery.log" 2>&1 &
PID=$!
sleep 3

# Check if it recovered properly
POST_CRASH_INFO=$(rpc '{"jsonrpc":"2.0","id":"post","method":"getblockchaininfo","params":[]}')
POST_CRASH_HEIGHT=$(echo "$POST_CRASH_INFO" | jq -r '.result.blocks')
POST_CRASH_HASH=$(echo "$POST_CRASH_INFO" | jq -r '.result.bestblockhash')

echo "Post-crash state: height=$POST_CRASH_HEIGHT, hash=$POST_CRASH_HASH"

# Check for corruption indicators in logs
if grep -q "corruption\|corrupt\|invalid\|damaged" "$DATADIR/recovery.log"; then
  echo "❌ Corruption detected in recovery logs"
  grep -i "corruption\|corrupt\|invalid\|damaged" "$DATADIR/recovery.log"
else
  echo "✅ No corruption detected"
fi

# Verify blockchain integrity
if [[ "$POST_CRASH_HEIGHT" -ge "$PRE_CRASH_HEIGHT" ]]; then
  echo "✅ Height preserved or advanced: $PRE_CRASH_HEIGHT -> $POST_CRASH_HEIGHT"
else
  echo "❌ Height regressed: $PRE_CRASH_HEIGHT -> $POST_CRASH_HEIGHT"
fi

# Test continued operation after recovery
echo "Testing continued operation after recovery..."
rpc '{"jsonrpc":"2.0","id":"test","method":"mining.start","params":[1]}' > /dev/null
sleep 2
FINAL_INFO=$(rpc '{"jsonrpc":"2.0","id":"final","method":"getblockchaininfo","params":[]}')
FINAL_HEIGHT=$(echo "$FINAL_INFO" | jq -r '.result.blocks')

if [[ "$FINAL_HEIGHT" -gt "$POST_CRASH_HEIGHT" ]]; then
  echo "✅ Continued operation successful: $POST_CRASH_HEIGHT -> $FINAL_HEIGHT"
else
  echo "⚠️  No new blocks after recovery (may be normal)"
fi

# Stop cleanly
rpc '{"jsonrpc":"2.0","id":"stop","method":"mining.stop","params":[]}' > /dev/null || true
kill -TERM $PID; wait $PID 2>/dev/null || true

echo "== Crash recovery test complete =="
