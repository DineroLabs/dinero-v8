#!/usr/bin/env bash
# RC smoke test script for DineroCoin wallet functionality
set -euo pipefail

D="${DINERO_DATADIR:-$HOME/.dinero}"
AUTH=$(cat "$D/.cookie")
WAL="http://127.0.0.1:20998/wallet/main"

jq_ok(){ command -v jq >/dev/null || { echo "jq is required"; exit 1; }; }
jq_ok

echo "=== DineroCoin RC Smoke Test ==="

# Test 1: Basic connectivity
echo "1. Testing basic RPC connectivity..."
curl -s --user "$AUTH" -H 'content-type: application/json' \
 --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' "$WAL" | jq .

# Test 2: Address generation
echo "2. Testing address generation..."
ADDR=$(curl -s --user "$AUTH" -H 'content-type: application/json' \
 --data '{"jsonrpc":"2.0","id":2,"method":"getnewaddress","params":["rc"]}' "$WAL" | jq -r '.result // .result.address')
test -n "$ADDR" || { echo "ERROR: no address generated"; exit 1; }
echo "Generated address: $ADDR"

# Test 3: Address validation
echo "3. Testing address validation..."
curl -s --user "$AUTH" -H 'content-type: application/json' \
 --data "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"validateaddress\",\"params\":[\"$ADDR\"]}" "$WAL" | jq .

# Test 4: Wallet info
echo "4. Testing wallet info..."
curl -s --user "$AUTH" -H 'content-type: application/json' \
 --data '{"jsonrpc":"2.0","id":4,"method":"getwalletinfo","params":[]}' "$WAL" | jq .

# Test 5: Balance check
echo "5. Testing balance retrieval..."
curl -s --user "$AUTH" -H 'content-type: application/json' \
 --data '{"jsonrpc":"2.0","id":5,"method":"getbalance","params":[]}' "$WAL" | jq .

# Test 6: Transaction history
echo "6. Testing transaction history..."
curl -s --user "$AUTH" -H 'content-type: application/json' \
 --data '{"jsonrpc":"2.0","id":6,"method":"listtransactions","params":[]}' "$WAL" | jq .

echo "=== RC Smoke Test Complete ==="
