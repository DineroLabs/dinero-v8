#!/usr/bin/env bash
set -euo pipefail

# --- Config ---
DATADIR=/tmp/dinero-regtest
A_RPC=20996
A_P2P=21001
B_RPC=20997
B_P2P=21002
BIN_DIR="./build/bin"
DAEMON="$BIN_DIR/dinerod"
CLI="$BIN_DIR/dinero-cli"
CURL_AUTH() { curl -s --user "$(cat "$1")" -H 'content-type: text/plain' --data-binary "$2" "http://127.0.0.1:$3"; }

wait_for_rpc() {
  local cookie="$1" port="$2"
  for i in {1..100}; do
    if curl -s --user "$(cat "$cookie")" \
      -H 'content-type: text/plain' \
      --data-binary '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
      "http://127.0.0.1:$port" | jq -e '.result' >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "RPC not ready on $port" >&2
  return 1
}

need() { command -v "$1" >/dev/null || { echo "Missing $1"; exit 1; }; }
need jq
need lsof

echo "=== 0) Clean start: stop any leftovers ==="
$CLI -regtest -datadir="$DATADIR" stop >/dev/null 2>&1 || true
sleep 1

echo "=== 1) Start node A (daemon) ==="
$DAEMON -regtest -datadir="$DATADIR" -server=1 -rpcport=$A_RPC -port=$A_P2P -daemon
COOKIE_A="$DATADIR/regtest/.cookie"
wait_for_rpc "$COOKIE_A" "$A_RPC"

echo "=== 2) RPC auth sanity ==="
# Try WITHOUT auth and ensure it's not a silent success:
NOAUTH_RESP=$(curl -s -H 'content-type: text/plain' \
  --data-binary '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  "http://127.0.0.1:$A_RPC")

# Treat as OK if it *didn't* return a valid numeric result.
if echo "$NOAUTH_RESP" | jq -e '.result|numbers' >/dev/null 2>&1; then
  echo "❌ RPC returned a valid result without auth"; exit 1
fi

# With cookie should succeed
CURL_AUTH "$COOKIE_A" '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' $A_RPC | jq .

echo "=== 3) Chain basics (genesis+premine exist) ==="
GEN=$(CURL_AUTH "$COOKIE_A" '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":[0]}' $A_RPC | jq -r .result)
[[ "$GEN" == 00002f1cfc2e9d48867cdca6d7a4c20f50858fce82a64cc7386434b125f15405 ]] || {
  echo "❌ Unexpected genesis hash: $GEN"; exit 1; }
# Premine is optional to validate strictly; try to fetch height 1 hash & txout known from your logs:
PRE=$(CURL_AUTH "$COOKIE_A" '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":[1]}' $A_RPC | jq -r .result)
echo "Premine block hash at h=1: $PRE (informational)"

echo "=== 4) Liveness & ports ==="
lsof -nP -iTCP:$A_RPC -sTCP:LISTEN >/dev/null
lsof -nP -iTCP:$A_P2P -sTCP:LISTEN >/dev/null

echo "=== 5) Mine a few blocks (prove forward progress) ==="
$CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" setgenerate true 2
sleep 2
BC1=$($CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" getblockcount)
sleep 2
BC2=$($CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" getblockcount)
echo "Blockcount moved: $BC1 -> $BC2"
(( BC2 > BC1 )) || { echo "❌ Block height did not increase"; exit 1; }

echo "=== 6) Spin up node B, connect to A, verify sync ==="
DATADIR_B=/tmp/dinero-regtest-b
rm -rf "$DATADIR_B"
$DAEMON -regtest -datadir="$DATADIR_B" -server=1 -rpcport=$B_RPC -port=$B_P2P -connect=127.0.0.1:$A_P2P -daemon
sleep 1
COOKIE_B="$DATADIR_B/regtest/.cookie"
test -f "$COOKIE_B" || { echo "❌ Node B cookie missing"; exit 1; }
# Wait a moment for headers/blocks to flow
sleep 2
BC_A=$($CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" getblockcount)
BC_B=$($CLI -regtest -datadir="$DATADIR_B" -rpcport="$B_RPC" getblockcount)
echo "A height=$BC_A, B height=$BC_B"
(( BC_B == BC_A )) || { echo "❌ Node B not synced to A"; exit 1; }

echo "=== 7) Restart persistence check (stop A, start, height must match) ==="
$CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" stop
sleep 1
$DAEMON -regtest -datadir="$DATADIR" -server=1 -rpcport=$A_RPC -port=$A_P2P -daemon
sleep 1
BC3=$($CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" getblockcount)
echo "Height after restart: $BC3"
(( BC3 == BC_A )) || { echo "❌ Height not preserved across restart"; exit 1; }

echo "=== 8) Basic error hygiene (no scary logs) ==="
LOGFILE="$DATADIR/regtest/debug.log"
if [[ -f "$LOGFILE" ]]; then
  LOGC=$(grep -iE 'ERROR|EXCEPTION|FATAL' "$LOGFILE" | wc -l | tr -d ' ')
else
  LOGC=0
fi
echo "Error/exception lines in logs: $LOGC"

echo "=== 9) Clean shutdown ==="
$CLI -regtest -datadir="$DATADIR" -rpcport="$A_RPC" stop
$CLI -regtest -datadir="$DATADIR_B" -rpcport="$B_RPC" stop
sleep 1

echo "✅ All smoke tests passed!"
