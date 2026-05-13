#!/usr/bin/env bash
set -euo pipefail

A_DIR=/tmp/dinero-regtest
B_DIR=/tmp/dinero-regtest-b
A_RPC=20996
B_RPC=20997
A_P2P=21001
B_P2P=21002

CURL() { curl -s -H 'content-type: text/plain' --data-binary "$1" "http://127.0.0.1:$2"; }
RPC()  {
  local dir="$1"; shift
  local port="$A_RPC"
  [[ "$dir" == "$B_DIR" ]] && port="$B_RPC"
  ./build/bin/dinero-cli -regtest -datadir="$dir" -rpcport="$port" "$@"
}

wait_for_rpc() {
  local dir="$1" port="$2"
  for _ in {1..100}; do
    if RPC "$dir" getblockcount >/dev/null 2>&1; then return 0; fi
    sleep 0.2
  done
  echo "RPC not ready on $port" >&2; exit 1
}

echo "=== 0) Clean start ==="
pkill -f dinerod || true
rm -rf "$A_DIR" "$B_DIR"

echo "=== 1) Start node A ==="
./build/bin/dinerod -regtest -datadir="$A_DIR" -server=1 -rpcport=$A_RPC -port=$A_P2P -daemon
wait_for_rpc "$A_DIR" $A_RPC
RPC "$A_DIR" getblockcount

echo "=== 2) Sanity: unauth RPC must NOT return a valid result ==="
NOAUTH=$(CURL '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' $A_RPC || true)
if echo "$NOAUTH" | jq -e '.result|numbers' >/dev/null 2>&1; then
  echo "❌ unauth RPC produced a valid result"; exit 1
fi
echo "✅ unauth RPC blocked"

echo "=== 3) Mine 3 blocks on A ==="
RPC "$A_DIR" setgenerate true 3
sleep 1
H1=$(RPC "$A_DIR" getblockcount)
echo "Height A: $H1"

echo "=== 4) Start node B peered to A ==="
./build/bin/dinerod -regtest -datadir="$B_DIR" -server=1 -rpcport=$B_RPC -port=$B_P2P -connect=127.0.0.1:$A_P2P -daemon
wait_for_rpc "$B_DIR" $B_RPC
sleep 1
H2=$(RPC "$B_DIR" getblockcount)
echo "Height B: $H2"
test "$H1" -eq "$H2" && echo "✅ B synced to A"

echo "=== 5) TX round-trip ==="
DEST=$(RPC "$B_DIR" getnewaddress)
AMT=100000000 # 1 DIN if 1e8 si = 1 DIN (adjust if your units differ)
TXID=$(RPC "$A_DIR" sendtoaddress "$DEST" 1) # or sendtoaddress_si "$DEST" $AMT if you use si
echo "TX: $TXID"
RPC "$A_DIR" setgenerate true 1
sleep 1
CONF=$(RPC "$B_DIR" gettransaction "$TXID" | jq -r '.confirmations // 0')
echo "Confirmations on B: $CONF"
test "$CONF" -ge 1 && echo "✅ TX confirmed on B"

echo "=== 6) Basic log hygiene ==="
LOGA="$A_DIR/regtest/debug.log"
if [[ -f "$LOGA" ]]; then
  ERR=$(grep -iE 'ERROR|EXCEPTION|FATAL' "$LOGA" | wc -l | tr -d ' ')
else
  ERR=0
fi
echo "Error/exception lines in A logs: $ERR"

echo "=== 7) Graceful shutdown ==="
RPC "$A_DIR" stop
RPC "$B_DIR" stop
sleep 1
echo "✅ Smoke test completed"
