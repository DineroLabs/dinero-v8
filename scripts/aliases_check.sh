#!/usr/bin/env bash
set -euo pipefail

# RPC Alias Verification Script
# Tests that all alias mappings work correctly with the canonicalization system

D="${DINERO_DATADIR:-$HOME/.dinero}"
AUTH=$(cat "$D/.cookie" 2>/dev/null || echo "__cookie__:placeholder")
RPC=$(jq -r '.rpc.url' "$D/nodeinfo.json" 2>/dev/null || echo "http://127.0.0.1:20998")

call() { 
    curl -s --user "$AUTH" -H 'content-type: application/json' --data "$1" "$2" 2>/dev/null || echo '{"error": {"message": "Connection failed"}}'
}

echo "=== RPC Canonicalization Test Suite ==="
echo "RPC URL: $RPC"
echo "Auth: ${AUTH%%:*}:***"
echo

echo "== 1. Canonical method on base URL =="
call '{"jsonrpc":"2.0","id":1,"method":"wallet.info","params":[]}' "$RPC" | jq -r '.result // .error.message'
echo

echo "== 2. Legacy alias on base URL (should canonicalize) =="
call '{"jsonrpc":"2.0","id":2,"method":"getwalletinfo","params":[]}' "$RPC" | jq -r '.result // .error.message'
echo

echo "== 3. Legacy method on wallet-scoped URL =="
call '{"jsonrpc":"2.0","id":3,"method":"getnewaddress","params":["alias-test"]}' "$RPC/wallet/main" | jq -r '.result // .error.message'
echo

echo "== 4. Blockchain method (canonical) =="
call '{"jsonrpc":"2.0","id":4,"method":"blockchain.getblockcount","params":[]}' "$RPC" | jq -r '.result // .error.message'
echo

echo "== 5. Blockchain method (legacy alias) =="
call '{"jsonrpc":"2.0","id":5,"method":"getblockcount","params":[]}' "$RPC" | jq -r '.result // .error.message'
echo

echo "== 6. RPC introspection - capabilities =="
call '{"jsonrpc":"2.0","id":6,"method":"rpc.capabilities","params":[]}' "$RPC" | jq -r '.result.style // .error.message'
echo

echo "== 7. RPC introspection - list methods =="
call '{"jsonrpc":"2.0","id":7,"method":"rpc.listmethods","params":[]}' "$RPC" | jq -r '(.result | length) // .error.message' | head -1
echo

echo "== 8. Unknown method (should fail gracefully) =="
call '{"jsonrpc":"2.0","id":8,"method":"nonexistent","params":[]}' "$RPC" | jq -r '.error.message // "Unexpected success"'
echo

echo "== 9. Batch request with mixed canonical/legacy =="
BATCH='[
  {"jsonrpc":"2.0","id":"b1","method":"wallet.info","params":[]},
  {"jsonrpc":"2.0","id":"b2","method":"getblockcount","params":[]},
  {"jsonrpc":"2.0","id":"b3","method":"blockchain.getbestblockhash","params":[]}
]'
call "$BATCH" "$RPC" | jq -r 'length // .error.message'
echo

echo "=== Test Complete ==="
