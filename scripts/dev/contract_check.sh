#!/usr/bin/env bash
set -euo pipefail
DATADIR="${1:-./test_contract}"
RPCPORT="${2:-21000}"
BIN="${BIN:-./dinerod}"

rm -rf "$DATADIR"
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run.log" 2>&1 &
PID=$!
sleep 2

COOKIE_FILE="$DATADIR/regtest/.cookie"
AUTH="$(cat "$COOKIE_FILE")"
URL="http://127.0.0.1:$RPCPORT/"
rpc () { curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" -d "$1"; }

echo "== Contract checks (RPC envelope validation) =="

# Check if RPC returns standardized envelope
echo "Testing status method for schema compliance..."
STATUS_RESP=$(rpc '{"jsonrpc":"2.0","id":"s","method":"status","params":[]}')
echo "Status response: $STATUS_RESP"

# Check for din.rpc.v1 schema
if echo "$STATUS_RESP" | jq -e 'select(.result.rpc_schema == "din.rpc.v1")' >/dev/null 2>&1; then
  echo "✅ schema ok"
else
  echo "❌ schema missing or incorrect"
fi

# Test getblockchaininfo for proper envelope
BLOCKCHAIN_RESP=$(rpc '{"jsonrpc":"2.0","id":"bc","method":"getblockchaininfo","params":[]}')
echo "Blockchain info response structure:"
echo "$BLOCKCHAIN_RESP" | jq '{jsonrpc, id, result: (.result | keys), error}'

# Test error handling envelope
ERROR_RESP=$(rpc '{"jsonrpc":"2.0","id":"err","method":"nonexistent_method","params":[]}')
echo "Error response structure:"
echo "$ERROR_RESP" | jq '{jsonrpc, id, result, error}'

# Validate JSON-RPC 2.0 compliance
if echo "$BLOCKCHAIN_RESP" | jq -e '.jsonrpc == "2.0"' >/dev/null 2>&1; then
  echo "✅ JSON-RPC 2.0 compliance ok"
else
  echo "❌ JSON-RPC 2.0 compliance failed"
fi

# Stop
kill -TERM $PID; wait $PID 2>/dev/null || true
