#!/bin/bash
COOKIE=$(cat ~/.dinero-regtest/.cookie | cut -d: -f2)
URL="http://127.0.0.1:20998"

echo "=== Testing wallet.listunspent ==="
RESULT=$(curl -s -u "__cookie__:$COOKIE" $URL -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listunspent","params":[],"id":1}')

echo "Full result:"
echo "$RESULT" | jq '.'

echo -e "\n=== First UTXO ==="
echo "$RESULT" | jq '.result[0]'

echo -e "\n=== Extract txid and vout ==="
UTXO=$(echo "$RESULT" | jq -r '.result[0]')
echo "UTXO object: $UTXO"

TXID=$(echo "$UTXO" | jq -r .txid)
VOUT=$(echo "$UTXO" | jq -r .vout)

echo "TXID: $TXID"
echo "VOUT: $VOUT"
