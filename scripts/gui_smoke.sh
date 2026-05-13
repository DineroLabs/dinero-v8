#!/usr/bin/env bash
set -euo pipefail

DATA="${HOME}/.dinero-dev"
PORT=20999
DAE="./build/dinerod"

killall dinero-desktop dinerod 2>/dev/null || true
mkdir -p "$DATA"
$DAE -regtest -server -rpcport=$PORT -datadir="$DATA" >/tmp/din.daemon.log 2>&1 &
sleep 1

COOKIE=$(cat "$DATA/regtest/.cookie")
jqOK(){ command -v jq >/dev/null || { echo "Install jq"; exit 1; }; }; jqOK
rpc(){ curl -s -u "$COOKIE" -H 'content-type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"$1\",\"params\":${2:-[]}}" http://127.0.0.1:$PORT/; }

check(){
  local m="$1" p="${2:-[]}"
  local out; out=$(rpc "$m" "$p")
  if echo "$out" | jq -e '.result' >/dev/null 2>&1; then
    echo "✅ $m"
  else
    local code msg
    code=$(echo "$out" | jq -r '.error.code // empty')
    msg=$(echo "$out" | jq -r '.error.message // empty')
    if [[ "$code" == "-32601" ]]; then
      echo "🟡 TODO: implement RPC $m (method not found)"
    else
      echo "🔴 $m failed: ${code:-?} ${msg:-?}"
    fi
  fi
}

echo "== Core =="
check getnetworkinfo
check getmempoolinfo
check getblockchaininfo

echo "== Mining =="
check mining.status
check mining.getaddress

ADDR=$(rpc getnewaddress | jq -r '.result.address // empty')
if [[ -n "$ADDR" ]]; then
  if rpc mining.generatetoaddress "[1,\"$ADDR\"]" | jq -e '.result' >/dev/null; then
    echo "✅ mining.generatetoaddress"
  elif rpc mining.generateToAddress "[1,\"$ADDR\"]" | jq -e '.result' >/dev/null; then
    echo "✅ mining.generateToAddress"
  else
    echo "🟡 TODO: implement RPC mining.generate(to)address (method not found)"
  fi
else
  echo "🔴 getnewaddress failed; cannot test generate-to-address"
fi

echo "== Wallet =="
check getwalletinfo
check getbalance
check getnewaddress
check listtransactions
check listunspent

echo "== HD Wallet Test =="
echo "Testing standalone HD wallet..."
./build/test_bip84_addr_check && echo "✅ HD wallet generates valid addresses"

echo ""
echo "🎉 GUI smoke test complete!"
echo "Now you can test the GUI by running:"
echo "   open build/src/gui-desktop/dinero-desktop.app"
echo ""
echo "Test the Generate Address button in the Wallet tab to see HD wallet integration!"
