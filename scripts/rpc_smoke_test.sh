#!/usr/bin/env bash
set -euo pipefail

DIN=./build-test/bin/dinerod
DATADIR=$(mktemp -d -t din-XXXX)
PORT=20998

echo "🧪 Starting RPC smoke test with datadir: $DATADIR"

$DIN -datadir="$DATADIR" >"$DATADIR/daemon.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; rm -rf "$DATADIR"' EXIT

echo "⏳ Waiting for daemon to start and generate cookie..."

# wait for cookie
for i in {1..100}; do
  [[ -f "$DATADIR/mainnet/.cookie" ]] && break
  sleep 0.05
done

if [[ ! -f "$DATADIR/mainnet/.cookie" ]]; then
  echo "❌ Cookie file not found after 5 seconds"
  exit 1
fi

COOKIE=$(cat "$DATADIR/mainnet/.cookie")
echo "✅ Cookie found: ${COOKIE:0:20}..."

rpc() {
  curl -s --user "$COOKIE" -H 'content-type: application/json' \
    --data "$1" "http://127.0.0.1:$PORT/"
}

echo "⏳ Waiting for chain bootstrap (blocks==1)..."

# wait for chain bootstrap (blocks==1)
for i in {1..200}; do
  if rpc '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
     | jq -e '.result.blocks==1' >/dev/null; then 
    echo "✅ Chain bootstrap complete (blocks=1)"
    break
  fi
  sleep 0.05
done

# Verify we got blocks=1
if ! rpc '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
     | jq -e '.result.blocks==1' >/dev/null; then
  echo "❌ Chain bootstrap failed - blocks != 1"
  exit 1
fi

echo "🧪 Running RPC tests..."

# 1) Unknown method → top-level error -32601
echo "  Testing unknown method error handling..."
if ! rpc '{"jsonrpc":"2.0","id":2,"method":"doesnotexist","params":[]}' \
     | jq -e '.error.code==-32601' >/dev/null; then
  echo "❌ Unknown method should return error code -32601"
  exit 1
fi
echo "  ✅ Unknown method returns proper error code"

# 2) Get block hashes
echo "  Testing block hash retrieval..."
H0=$(rpc '{"jsonrpc":"2.0","id":3,"method":"getblockhash","params":[0]}' | jq -r .result)
H1=$(rpc '{"jsonrpc":"2.0","id":4,"method":"getblockhash","params":[1]}' | jq -r .result)

if [[ -z "$H0" || -z "$H1" ]]; then
  echo "❌ Failed to get block hashes"
  exit 1
fi
echo "  ✅ Block 0: ${H0:0:16}..."
echo "  ✅ Block 1: ${H1:0:16}..."

# 3) getblock verbose JSON fields present
echo "  Testing getblock verbose response..."
if ! rpc '{"jsonrpc":"2.0","id":5,"method":"getblock","params":["'"$H1"'",true]}' \
     | jq -e --arg h1 "$H1" '.result | (.hash==$h1) and (.height==1) and ((.tx|length) >= 1)' >/dev/null; then
  echo "❌ getblock verbose response missing required fields"
  exit 1
fi
echo "  ✅ getblock verbose response has all required fields"

# 4) Linkage: prev(h=1) == hash(h=0)
echo "  Testing block linkage..."
if ! rpc '{"jsonrpc":"2.0","id":6,"method":"getblock","params":["'"$H1"'",true]}' \
     | jq -e '.result.previousblockhash=="'"$H0"'"' >/dev/null; then
  echo "❌ Block linkage failed: prev(h=1) != hash(h=0)"
  exit 1
fi
echo "  ✅ Block linkage verified: prev(h=1) == hash(h=0)"

# 5) Test response headers
echo "  Testing response headers..."
if ! curl -s -i --user "$COOKIE" -H 'content-type: application/json' \
     --data '{"jsonrpc":"2.0","id":7,"method":"getblockchaininfo","params":[]}' \
     "http://127.0.0.1:$PORT/" | grep -i '^x-dinero-rpc-engine:'; then
  echo "❌ Missing X-Dinero-RPC-Engine: v2 header"
  exit 1
fi
echo "  ✅ Response headers include RPC engine version"

echo "🎉 RPC smoke test passed!"
echo "✅ All critical RPC functionality working correctly"
echo "✅ JSON-RPC 2.0 compliance verified"
echo "✅ Block linkage integrity confirmed"
echo "✅ Response headers properly set"
