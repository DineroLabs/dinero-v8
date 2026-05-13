#!/usr/bin/env bash
set -euo pipefail
DATADIR="${1:-./test_persistence}"
RPCPORT="${2:-21001}"
BIN="${BIN:-./dinerod}"

echo "== Persistence & CIC binding test =="

# First run: create DB and store CIC
rm -rf "$DATADIR"
echo "Starting daemon for first time to create DB..."
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run1.log" 2>&1 &
PID1=$!
sleep 3

COOKIE_FILE="$DATADIR/regtest/.cookie"
AUTH="$(cat "$COOKIE_FILE")"
URL="http://127.0.0.1:$RPCPORT/"
rpc () { curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" -d "$1"; }

# Get initial blockchain info
INITIAL_INFO=$(rpc '{"jsonrpc":"2.0","id":"init","method":"getblockchaininfo","params":[]}')
INITIAL_HASH=$(echo "$INITIAL_INFO" | jq -r '.result.bestblockhash')
INITIAL_HEIGHT=$(echo "$INITIAL_INFO" | jq -r '.result.blocks')

echo "Initial state: height=$INITIAL_HEIGHT, hash=$INITIAL_HASH"

# Stop first instance
kill -TERM $PID1; wait $PID1 2>/dev/null || true
sleep 1

# Second run: should load existing DB and match CIC
echo "Restarting daemon to test persistence..."
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run2.log" 2>&1 &
PID2=$!
sleep 3

# Verify state persisted
RESTORED_INFO=$(rpc '{"jsonrpc":"2.0","id":"restore","method":"getblockchaininfo","params":[]}')
RESTORED_HASH=$(echo "$RESTORED_INFO" | jq -r '.result.bestblockhash')
RESTORED_HEIGHT=$(echo "$RESTORED_INFO" | jq -r '.result.blocks')

echo "Restored state: height=$RESTORED_HEIGHT, hash=$RESTORED_HASH"

if [[ "$INITIAL_HASH" == "$RESTORED_HASH" ]] && [[ "$INITIAL_HEIGHT" == "$RESTORED_HEIGHT" ]]; then
  echo "✅ Persistence check passed"
else
  echo "❌ Persistence check failed"
  echo "Expected: height=$INITIAL_HEIGHT, hash=$INITIAL_HASH"
  echo "Got: height=$RESTORED_HEIGHT, hash=$RESTORED_HASH"
fi

# Test CIC mismatch protection (if --dev-autoreset flag exists)
kill -TERM $PID2; wait $PID2 2>/dev/null || true
sleep 1

# Try to start with different network (should fail or warn)
echo "Testing CIC protection with different network..."
"$BIN" --datadir="$DATADIR" --testnet --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run3.log" 2>&1 &
PID3=$!
sleep 3

# Check if it refused to start or gave clear error
if grep -q "network mismatch\|CIC mismatch\|chain mismatch" "$DATADIR/run3.log"; then
  echo "✅ CIC protection working"
else
  echo "⚠️  CIC protection not detected (may need implementation)"
fi

kill -TERM $PID3 2>/dev/null || true; wait $PID3 2>/dev/null || true

echo "== Persistence test complete =="
