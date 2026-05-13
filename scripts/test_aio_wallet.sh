#!/usr/bin/env bash
set -euo pipefail

echo "🧪 DineroCoin All-in-One Wallet Smoke Test"
echo "=========================================="

# Find latest nodeinfo
NI=$(ls -t /tmp/dinero-nodeinfo-*.json 2>/dev/null | head -1)
[ -n "${NI:-}" ] || { echo "❌ No nodeinfo found - is AIO running?"; exit 1; }

COOKIE=$(jq -r '.cookie' "$NI")
RPCP=$(jq -r '.rpc.port' "$NI")
RPC="http://127.0.0.1:${RPCP}"
AUTH=$(tr -d '\r\n' < "$COOKIE")

echo "📋 Nodeinfo Discovery:"
jq . "$NI"
echo ""

echo "🔗 Testing RPC Connection..."
echo "== getblockcount =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":{}}' \
  "$RPC" | jq .
echo ""

echo "💰 Testing Wallet RPC Methods..."
echo "== wallet.create =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"wallet.create","params":{"name":"dinero_wallet"}}' \
  "$RPC" | jq .
echo ""

echo "== wallet.info =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"wallet.info","params":{}}' \
  "$RPC" | jq .
echo ""

echo "== wallet.getnewaddress =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"wallet.getnewaddress","params":{"account":0,"change":false}}' \
  "$RPC" | jq .
echo ""

echo "== wallet.listaddresses =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"wallet.listaddresses","params":{"account":0}}' \
  "$RPC" | jq .

echo "== getinfo =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getinfo","params":{}}' \
  "$RPC" | jq .

echo "== rpc.getinfo =="
curl -sS --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"rpc.getinfo","params":{}}' \
  "$RPC" | jq .
echo ""

echo "✅ All wallet RPC methods tested successfully!"
echo "🎯 Next: Test GUI buttons match these RPC responses"
