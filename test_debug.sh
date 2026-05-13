#!/bin/bash
set -e

sleep 3

COOKIE=$(cat ~/.dinero-regtest/.cookie | cut -d: -f2)

echo "Creating wallet and generating blocks..."
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["test"],"id":1}' | jq -r .result.success

ADDR=$(curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.getnewaddress","params":[],"id":2}' | jq -r .result.address)
echo "Address: $ADDR"

echo "Generating 110 blocks..."
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"generatetoaddress\",\"params\":[110,\"$ADDR\"],\"id\":3}" | jq -r '.result | length'

echo "Waiting for blockchain to settle..."
sleep 2

echo "Rebuilding accumulator..."
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.rebuildutreexo","params":[],"id":4}' | jq .

sleep 1

echo "Getting first UTXO..."
UTXO=$(curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listunspent","params":[],"id":5}' | jq -r '.result[0]')
TXID=$(echo $UTXO | jq -r .txid)
VOUT=$(echo $UTXO | jq -r .vout)

echo "TXID: $TXID"
echo "VOUT: $VOUT"

echo "Requesting proof..."
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"blockchain.getutxoproof\",\"params\":[\"$TXID\",$VOUT],\"id\":6}" | jq .

echo ""
echo "Check logs for debug output at: ~/.dinero-regtest/dinero.log"
