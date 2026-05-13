#!/bin/bash
set -e

COOKIE=$(cat ~/.dinero/.cookie | cut -d: -f2)

echo "Step 1: Restoring wallet..."
curl -s --user "__cookie__:$COOKIE" \
  --data-binary '{"jsonrpc":"2.0","id":"1","method":"wallet.restore","params":["my_wallet","<redacted legacy premine mnemonic>","",""]}' \
  http://127.0.0.1:20998 | python3 -m json.tool

echo ""
echo "Step 2: Mining Block 1..."
curl -s --user "__cookie__:$COOKIE" \
  --data-binary '{"jsonrpc":"2.0","id":"1","method":"generatetoaddress","params":[1,"din1qd43uqnzgpp8w28490ex8je03axt7wy6fh28rlh"]}' \
  http://127.0.0.1:20998 | python3 -m json.tool

sleep 2

echo ""
echo "Step 3: Checking wallet balance..."
curl -s --user "__cookie__:$COOKIE" \
  --data-binary '{"jsonrpc":"2.0","id":"1","method":"wallet.getinfo","params":[]}' \
  http://127.0.0.1:20998 | python3 -m json.tool

echo ""
echo "Step 4: Checking database..."
sqlite3 ~/.dinero/wallets/wallet_my_wallet.db "SELECT COUNT(*) FROM utxos;"
