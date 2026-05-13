#!/usr/bin/env bash
set -euo pipefail
DATADIR="${1:-./test_mine}"
RPCPORT="${2:-20999}"
BIN="${BIN:-./dinerod}"

rm -rf "$DATADIR"
"$BIN" --datadir="$DATADIR" --regtest --rpcport="$RPCPORT" --printtoconsole > "$DATADIR/run.log" 2>&1 &
PID=$!
sleep 2

COOKIE_FILE="$DATADIR/regtest/.cookie"
AUTH="$(cat "$COOKIE_FILE")"
URL="http://127.0.0.1:$RPCPORT/"
rpc () { curl -s --user "$AUTH" -H 'content-type: application/json' "$URL" -d "$1"; }

# 1) New address (or your project's equivalent)
ADDR=$(rpc '{"jsonrpc":"2.0","id":"na","method":"wallet.newaddress","params":[]}' | jq -r '.result // empty')
if [[ -z "$ADDR" ]]; then
  echo "wallet.newaddress missing; trying generic 'getnewaddress'"
  ADDR=$(rpc '{"jsonrpc":"2.0","id":"gn","method":"getnewaddress","params":[]}' | jq -r '.result // empty')
fi
echo "Mining payout address: $ADDR"

# 2) Set miner payout + start (adapt method names if yours differ)
rpc "{\"jsonrpc\":\"2.0\",\"id\":\"sa\",\"method\":\"mining.setaddress\",\"params\":[\"$ADDR\"]}" | jq .
rpc '{"jsonrpc":"2.0","id":"st","method":"mining.start","params":[1]}' | jq .

# Let it mine a bit
sleep 3

# 3) Check height & balance
rpc '{"jsonrpc":"2.0","id":"h","method":"getblockchaininfo","params":[]}' | jq '{blocks:.result.blocks, best:.result.bestblockhash}'
rpc '{"jsonrpc":"2.0","id":"bal","method":"wallet.getbalance","params":[]}' | jq '.result // .error'

# Stop
rpc '{"jsonrpc":"2.0","id":"sp","method":"mining.stop","params":[]}' | jq . || true
kill -TERM $PID; wait $PID 2>/dev/null || true
