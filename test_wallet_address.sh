#!/bin/bash
set -e

COOKIE=$(cat ~/.dinero-regtest/.cookie | cut -d: -f2)

echo "=== Creating HD wallet ==="
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["debugtest"],"id":1}' | jq .

echo ""
echo "=== Getting new address ==="
curl -s -u "__cookie__:$COOKIE" http://127.0.0.1:20998 -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.getnewaddress","params":[],"id":2}' | jq .

echo ""
echo "=== Checking wallet database ==="
sqlite3 ~/.dinero-regtest/wallets/wallet_debugtest.db "SELECT COUNT(*) FROM hd_seeds;"
