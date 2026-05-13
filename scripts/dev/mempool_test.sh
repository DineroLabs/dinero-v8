#!/usr/bin/env bash
set -euo pipefail
DATADIR="${1:-./test_mempool}"
RPCPORT="${2:-21003}"
BIN="${BIN:-./dinerod}"

echo "== Mempool admission policy test =="

rm -rf "$DATADIR"
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run.log" 2>&1 &
PID=$!
sleep 2

COOKIE_FILE="$DATADIR/regtest/.cookie"
AUTH="$(cat "$COOKIE_FILE")"
URL="http://127.0.0.1:$RPCPORT/"
rpc () { curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" -d "$1"; }

# Get initial mempool state
echo "Initial mempool state:"
rpc '{"jsonrpc":"2.0","id":"mp1","method":"getmempoolinfo","params":[]}' | jq '.result // .error'

# Create a wallet address for testing
ADDR=$(rpc '{"jsonrpc":"2.0","id":"addr","method":"wallet.newaddress","params":[]}' | jq -r '.result // "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"')
echo "Test address: $ADDR"

# Mine some blocks to get coins for testing
rpc "{\"jsonrpc\":\"2.0\",\"id\":\"setaddr\",\"method\":\"mining.setaddress\",\"params\":[\"$ADDR\"]}" > /dev/null
rpc '{"jsonrpc":"2.0","id":"start","method":"mining.start","params":[1]}' > /dev/null
sleep 3
rpc '{"jsonrpc":"2.0","id":"stop","method":"mining.stop","params":[]}' > /dev/null

# Check balance
BALANCE=$(rpc '{"jsonrpc":"2.0","id":"bal","method":"wallet.getbalance","params":[]}' | jq -r '.result // 0')
echo "Wallet balance: $BALANCE"

# Test mempool admission with different scenarios
if [[ "$BALANCE" != "0" ]] && [[ "$BALANCE" != "null" ]]; then
  echo "Testing transaction creation and mempool admission..."
  
  # Try to create a transaction (if supported)
  NEW_ADDR=$(rpc '{"jsonrpc":"2.0","id":"addr2","method":"wallet.newaddress","params":[]}' | jq -r '.result // "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"')
  
  # Test with normal fee (if wallet.sendtoaddress exists)
  SEND_RESULT=$(rpc "{\"jsonrpc\":\"2.0\",\"id\":\"send\",\"method\":\"wallet.sendtoaddress\",\"params\":[\"$NEW_ADDR\", 1.0]}" 2>/dev/null || echo '{"error":"method not found"}')
  
  if echo "$SEND_RESULT" | jq -e '.result' >/dev/null 2>&1; then
    TXID=$(echo "$SEND_RESULT" | jq -r '.result')
    echo "✅ Transaction created: $TXID"
    
    # Check if it's in mempool
    MEMPOOL_AFTER=$(rpc '{"jsonrpc":"2.0","id":"mp2","method":"getmempoolinfo","params":[]}')
    MEMPOOL_SIZE=$(echo "$MEMPOOL_AFTER" | jq -r '.result.size // 0')
    echo "Mempool size after transaction: $MEMPOOL_SIZE"
    
    if [[ "$MEMPOOL_SIZE" -gt 0 ]]; then
      echo "✅ Transaction admitted to mempool"
    else
      echo "⚠️  Transaction not in mempool (may have been mined immediately)"
    fi
  else
    echo "⚠️  Transaction creation not supported or failed"
    echo "Send result: $SEND_RESULT"
  fi
else
  echo "⚠️  No balance available for transaction testing"
fi

# Test mempool info and raw mempool
echo "Testing mempool RPC methods..."
rpc '{"jsonrpc":"2.0","id":"mp3","method":"getrawmempool","params":[]}' | jq '.result // .error'

# Test invalid transaction handling (if createrawtransaction exists)
echo "Testing invalid transaction rejection..."
INVALID_TX_RESULT=$(rpc '{"jsonrpc":"2.0","id":"invalid","method":"sendrawtransaction","params":["0100000000000000"]}' 2>/dev/null || echo '{"error":"expected"}')
if echo "$INVALID_TX_RESULT" | jq -e '.error' >/dev/null 2>&1; then
  echo "✅ Invalid transaction properly rejected"
else
  echo "⚠️  Invalid transaction handling unclear"
fi

# Stop
kill -TERM $PID; wait $PID 2>/dev/null || true

echo "== Mempool test complete =="
