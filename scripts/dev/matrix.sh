#!/usr/bin/env bash
set -euo pipefail
set -x

echo "== Test Matrix: Backend and Network Combinations =="

# Backends and networks to test
BACKENDS=("sqlite" "rocksdb")
NETWORKS=("regtest" "testnet")
BASE_PORT=21010

cleanup() {
  echo "Cleaning up any running processes..."
  pkill -f "dinerod.*--rpcport=21[0-9][0-9][0-9]" || true
  sleep 1
}

trap cleanup EXIT

BIN="${BIN:-./dinerod}"

for BACKEND in "${BACKENDS[@]}"; do
  for NETWORK in "${NETWORKS[@]}"; do
    PORT=$((BASE_PORT++))
    DBDIR="test_matrix_${BACKEND}_${NETWORK}"
    
    echo "Testing: $BACKEND + $NETWORK on port $PORT"
    
    rm -rf "$DBDIR"
    
    # Start daemon with backend-specific flags if available
    BACKEND_FLAGS=""
    if [[ "$BACKEND" == "sqlite" ]]; then
      BACKEND_FLAGS="--db=sqlite"
    elif [[ "$BACKEND" == "rocksdb" ]]; then
      BACKEND_FLAGS="--db=rocksdb"
    fi
    
    NETWORK_FLAG="--$NETWORK"
    
    "$BIN" --datadir="$DBDIR" $NETWORK_FLAG $BACKEND_FLAGS --rpcport=$PORT --printtoconsole > "$DBDIR/run.log" 2>&1 &
    PID=$!
    sleep 3
    
    # Test basic RPC functionality
    COOKIE_FILE="$DBDIR/$NETWORK/.cookie"
    if [[ ! -f "$COOKIE_FILE" ]]; then
      echo "❌ $BACKEND/$NETWORK => FAIL (no cookie)"
      kill -TERM $PID 2>/dev/null || true; wait $PID 2>/dev/null || true
      continue
    fi
    
    AUTH="$(cat "$COOKIE_FILE")"
    URL="http://127.0.0.1:$PORT/"
    
    OK=1
    BLOCKS=$(curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" \
      -d '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' | jq -r '.result.blocks // empty') || OK=0
    
    if [[ $OK == 1 ]] && [[ -n "$BLOCKS" ]] && [[ "$BLOCKS" != "null" ]]; then
      echo "✅ $BACKEND/$NETWORK => PASS (blocks: $BLOCKS)"
      
      # Test a few more RPCs for completeness
      curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" \
        -d '{"jsonrpc":"2.0","id":2,"method":"getbestblockhash","params":[]}' | jq -r '.result // "ERROR"' > /dev/null
      
      curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" \
        -d '{"jsonrpc":"2.0","id":3,"method":"help","params":[]}' | jq -r '.result // "ERROR"' > /dev/null
        
    else
      echo "❌ $BACKEND/$NETWORK => FAIL (RPC error)"
    fi
    
    kill -TERM $PID 2>/dev/null || true; wait $PID 2>/dev/null || true
    sleep 1
  done
done

echo "== Matrix test complete =="
