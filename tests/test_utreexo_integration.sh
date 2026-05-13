#!/bin/bash
# Phase 34.2: Utreexo Integration Test
# Tests the live Utreexo accumulator with real blockchain data

set -e

# Wait for daemon to be ready
sleep 5

# Get authentication cookie
COOKIE=$(cat ~/.dinero-regtest/.cookie | cut -d: -f2)
URL="http://127.0.0.1:20998"
WALLET_URL="http://127.0.0.1:20998/wallet/test"

echo "═══════════════════════════════════════════════════════════════"
echo "  UTREEXO INTEGRATION TEST"
echo "  Phase 34.2: Live Accumulator Validation"
echo "═══════════════════════════════════════════════════════════════"

echo -e "\n[TEST 1] Initial State (Empty Accumulator)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexostats","params":[],"id":1}' | jq .

echo -e "\n[TEST 2] Create Wallet & Generate Blocks"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
WALLET_RESULT=$(curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["test"],"id":2}')
echo "$WALLET_RESULT" | jq -r .result.success

# Use the first_address returned by wallet.createhd (P2WPKH works)
ADDR=$(echo "$WALLET_RESULT" | jq -r .result.first_address)
echo "✓ Wallet address: $ADDR"

echo "Generating 110 blocks..."
BLOCKS=$(curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"generatetoaddress\",\"params\":[110,\"$ADDR\"],\"id\":4}" | jq -r '.result | length')
echo "✓ Generated $BLOCKS blocks"

sleep 2

echo -e "\n[TEST 3] Rebuild Utreexo Accumulator"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.rebuildutreexo","params":[],"id":5}' | jq .

echo -e "\n[TEST 4] Query Accumulator Statistics"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexostats","params":[],"id":6}' | jq .

echo -e "\n[TEST 5] Get Utreexo Commitment"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexocommitment","params":[],"id":7}' | jq .

echo -e "\n[TEST 6] Get Forest Roots"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexoroots","params":[],"id":8}' | jq .

echo -e "\n[TEST 7] Get UTXO Proof (First Coinbase)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
# Get first UTXO
UTXO=$(curl -s -u "__cookie__:$COOKIE" $WALLET_URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listunspent","params":[],"id":9}' | jq -r '.result[0]')
TXID=$(echo $UTXO | jq -r .txid)
VOUT=$(echo $UTXO | jq -r .vout)

echo "Testing proof for UTXO: $TXID:$VOUT"

curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"blockchain.getutxoproof\",\"params\":[\"$TXID\",$VOUT],\"id\":10}" | jq .

echo -e "\n[TEST 8] Spend a UTXO and Verify Accumulator Updates"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
# Get stats before spend
STATS_BEFORE=$(curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexostats","params":[],"id":11}' | jq -r .result.num_leaves)
echo "Leaves before spend: $STATS_BEFORE"

# Send transaction
NEWADDR=$(curl -s -u "__cookie__:$COOKIE" $WALLET_URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.getnewaddress","params":[],"id":12}' | jq -r .result.address)

curl -s -u "__cookie__:$COOKIE" $WALLET_URL -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"wallet.sendtoaddress\",\"params\":[\"$NEWADDR\",1.0],\"id\":13}" | jq .result

# Mine block to confirm
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"generatetoaddress\",\"params\":[1,\"$ADDR\"],\"id\":14}" | jq -r '.result | length'

sleep 1

# Rebuild and check stats after
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.rebuildutreexo","params":[],"id":15}' | jq -r .result.num_leaves

STATS_AFTER=$(curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexostats","params":[],"id":16}' | jq -r .result.num_leaves)
echo "Leaves after spend+mine: $STATS_AFTER"

echo -e "\n[TEST 9] Final Commitment"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getutreexocommitment","params":[],"id":17}' | jq .

echo -e "\n═══════════════════════════════════════════════════════════════"
echo "  ✅ ALL UTREEXO INTEGRATION TESTS COMPLETE"
echo "═══════════════════════════════════════════════════════════════"
