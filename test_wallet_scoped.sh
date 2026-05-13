#!/bin/bash
COOKIE=$(cat ~/.dinero-regtest/.cookie | cut -d: -f2)

echo "=== Test 1: listunspent on default URL ==="
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listunspent","params":[],"id":1}' | jq .

echo -e "\n=== Test 2: listunspent on /wallet/test URL ==="
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998/wallet/test -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listunspent","params":[],"id":2}' | jq .

echo -e "\n=== Test 3: Check if wallet 'test' exists ==="
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.listwallets","params":[],"id":3}' | jq .
