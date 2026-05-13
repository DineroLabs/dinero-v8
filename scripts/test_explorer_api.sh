#!/bin/bash
# Quick smoke tests for Explorer API v1
# Copy/paste these commands to verify your API is working

set -e

BASE_URL="http://localhost:25998/api/v1"
echo "🧪 Testing Explorer API v1 at $BASE_URL"
echo

# Test 1: Health check
echo "1️⃣ Health check..."
curl -s "$BASE_URL/health" | jq '.'
echo "✅ Expected: {\"ok\":true,\"height\":N,\"db\":\"ok\",\"mempool_index\":\"ok\"}"
echo

# Test 2: Chain tip
echo "2️⃣ Chain tip..."
curl -s "$BASE_URL/chain/tip" | jq '.'
echo "✅ Expected: {\"height\":N,\"hash\":\"...\",\"time\":N}"
echo

# Test 3: Block by height (verbosity 1)
echo "3️⃣ Block by height (verbosity=1)..."
curl -s "$BASE_URL/block/height/1?verbosity=1" | jq '.tx'
echo "✅ Expected: [\"txid1\",\"txid2\",...]"
echo

# Test 4: Block by height (verbosity 2)
echo "4️⃣ Block by height (verbosity=2)..."
curl -s "$BASE_URL/block/height/1?verbosity=2" | jq '.tx[0].vout'
echo "✅ Expected: [{\"n\":0,\"value\":2000000.0,\"scriptPubKey\":{...}}]"
echo

# Test 5: Get block hash for further tests
echo "5️⃣ Getting block hash for transaction tests..."
BLOCK_HASH=$(curl -s "$BASE_URL/block/height/1" | jq -r '.hash')
echo "Block hash: $BLOCK_HASH"
echo

# Test 6: Block by hash
echo "6️⃣ Block by hash..."
curl -s "$BASE_URL/block/$BLOCK_HASH?verbosity=1" | jq '.height'
echo "✅ Expected: 1"
echo

# Test 7: Transaction by ID (get from block)
echo "7️⃣ Getting transaction ID from block..."
TXID=$(curl -s "$BASE_URL/block/height/1?verbosity=1" | jq -r '.tx[0]')
echo "Transaction ID: $TXID"
echo

echo "8️⃣ Transaction details..."
curl -s "$BASE_URL/tx/$TXID" | jq '.vout[0]'
echo "✅ Expected: {\"n\":0,\"value\":2000000.0,\"scriptPubKey\":{...}}"
echo

# Test 8: Transaction hex
echo "9️⃣ Transaction hex..."
curl -s "$BASE_URL/tx/$TXID/hex" | jq '.hex' | head -c 20
echo "..."
echo "✅ Expected: \"02000000...\" (hex string)"
echo

# Test 9: Blocks page
echo "🔟 Blocks page..."
curl -s "$BASE_URL/blocks?from_height=0&limit=2" | jq '.items | length'
echo "✅ Expected: 2 (or number of blocks requested)"
echo

# Test 10: Address summary (you'll need to replace with actual address)
echo "1️⃣1️⃣ Address summary (replace with actual address)..."
echo "curl -s \"$BASE_URL/address/din1...\" | jq '.'"
echo "✅ Expected: {\"address\":\"din1...\",\"scripthash\":\"...\",\"received\":N,\"sent\":N,\"balance\":N,\"tx_count\":N}"
echo

# Test 11: Address UTXOs
echo "1️⃣2️⃣ Address UTXOs (replace with actual address)..."
echo "curl -s \"$BASE_URL/address/din1.../utxos?limit=10\" | jq '.items'"
echo "✅ Expected: [{\"txid\":\"...\",\"vout\":0,\"value\":N,\"height\":N,\"spk_hex\":\"...\"}]"
echo

# Test 12: Mempool summary
echo "1️⃣3️⃣ Mempool summary..."
curl -s "$BASE_URL/mempool" | jq '.'
echo "✅ Expected: {\"tx_count\":N,\"total_bytes\":N,\"min_fee_rate\":N,\"histogram\":[...]}"
echo

# Test 13: Stats - Supply
echo "1️⃣4️⃣ Supply stats..."
curl -s "$BASE_URL/stats/supply" | jq '.'
echo "✅ Expected: {\"height\":N,\"subsidy\":N,\"cumulative_issued\":N,\"circulating\":N}"
echo

# Test 14: Stats - Difficulty
echo "1️⃣5️⃣ Difficulty stats..."
curl -s "$BASE_URL/stats/difficulty" | jq '.'
echo "✅ Expected: {\"height\":N,\"bits\":\"1f002710\",\"difficulty\":N}"
echo

# Test 15: Search (block height)
echo "1️⃣6️⃣ Search by block height..."
curl -s "$BASE_URL/search?q=1" | jq '.'
echo "✅ Expected: {\"type\":\"block\",\"result\":{...}}"
echo

# Test 16: Search (transaction ID)
echo "1️⃣7️⃣ Search by transaction ID..."
curl -s "$BASE_URL/search?q=$TXID" | jq '.type'
echo "✅ Expected: \"tx\""
echo

# Test 17: Search (not found)
echo "1️⃣8️⃣ Search not found..."
curl -s "$BASE_URL/search?q=nonexistent" | jq '.type'
echo "✅ Expected: \"not_found\""
echo

# Test 18: Error handling - invalid block hash
echo "1️⃣9️⃣ Error handling - invalid block hash..."
curl -s "$BASE_URL/block/invalid_hash" | jq '.error'
echo "✅ Expected: {\"code\":N,\"message\":\"...\"}"
echo

# Test 19: Error handling - block not found
echo "2️⃣0️⃣ Error handling - block not found..."
curl -s "$BASE_URL/block/0000000000000000000000000000000000000000000000000000000000000000" | jq '.error'
echo "✅ Expected: {\"code\":-5,\"message\":\"block not found\"}"
echo

# Test 20: CORS headers
echo "2️⃣1️⃣ CORS headers..."
curl -s -I "$BASE_URL/health" | grep -i "access-control-allow-origin"
echo "✅ Expected: Access-Control-Allow-Origin: *"
echo

# Test 21: Caching headers (historical data)
echo "2️⃣2️⃣ Caching headers (historical block)..."
curl -s -I "$BASE_URL/block/height/1" | grep -i "cache-control"
echo "✅ Expected: Cache-Control: public,max-age=3600"
echo

# Test 22: Caching headers (tip data)
echo "2️⃣3️⃣ Caching headers (chain tip)..."
curl -s -I "$BASE_URL/chain/tip" | grep -i "cache-control"
echo "✅ Expected: Cache-Control: public,max-age=5"
echo

echo
echo "🎉 Explorer API v1 smoke tests complete!"
echo
echo "📋 Manual tests to run with actual data:"
echo "   • Replace 'din1...' with actual bech32 addresses from your blockchain"
echo "   • Test with actual transaction IDs from your blocks"
echo "   • Test pagination with cursor parameters"
echo "   • Test WebSocket at ws://localhost:25998/ws/explorer"
echo
echo "🔍 Performance tests:"
echo "   • Test with large limit values to check pagination"
echo "   • Test concurrent requests to verify thread safety"
echo "   • Monitor database query performance"
echo

# WebSocket test (requires websocat or similar)
if command -v websocat &> /dev/null; then
    echo "2️⃣4️⃣ WebSocket test (5 seconds)..."
    timeout 5s websocat ws://localhost:25998/ws/explorer || echo "WebSocket test completed (or timed out)"
    echo "✅ Expected: Real-time JSON messages like {\"type\":\"newBlock\",...}"
else
    echo "2️⃣4️⃣ WebSocket test (install websocat to test)..."
    echo "   websocat ws://localhost:25998/ws/explorer"
    echo "✅ Expected: Real-time JSON messages"
fi

echo
echo "✨ All tests completed! Check the output above for any errors."
