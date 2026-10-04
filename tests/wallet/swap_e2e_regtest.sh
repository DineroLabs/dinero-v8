#!/usr/bin/env bash
# Two-chain end-to-end swap: one dinerod and one bitcoind on regtest, Alice and
# Bob each driving their own SwapRunner (tests/wallet/swap_e2e_tool.cpp).
# dinerod runs with transaction-lock enforcement from height 1, so timestamp
# locks are enforced by the chain, not only respected by the engine.
#   usage: swap_e2e_regtest.sh <dinerod> <swap_e2e_tool>
set -euo pipefail
DINEROD=${1:?dinerod}; TOOL=${2:?swap_e2e_tool}
DIR=$(mktemp -d)
DPORT=$((20000 + RANDOM % 20000)); DP2P=$((DPORT + 7)); BPORT=$((DPORT + 13))
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$BPORT -rpcuser=test -rpcpassword=test)
cleanup() { "${BCLI[@]}" stop >/dev/null 2>&1 || true
            [[ -n "${NPID:-}" ]] && kill "$NPID" 2>/dev/null || true; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT
dcall() {  # <method> <params-json> -> result (exits on error)
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$1" "$2")
  local resp; resp=$(curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$DPORT/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
r=d.get("result"); print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$1"
}

mkdir -p "$DIR/din" "$DIR/btc" "$DIR/swaps"
"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$DPORT -port=$DP2P \
  -datadir="$DIR/din" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
  --consensus-contextual-locks-height=1 >"$DIR/din.log" 2>&1 & NPID=$!
bitcoind -regtest -datadir="$DIR/btc" -listen=0 -rpcport=$BPORT -rpcuser=test -rpcpassword=test \
  -daemon -fallbackfee=0.0002 >/dev/null
for _ in $(seq 1 90); do dcall getblockcount '[]' >/dev/null 2>&1 && break; sleep 1; done
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done

# Alice's Dinero wallet (funds the DIN lock).
dcall wallet.createhd '["alice"]' >/dev/null
dcall wallet.encrypt '["pw-swap-e2e"]' >/dev/null
dcall wallet.unlock '["pw-swap-e2e", 3600]' >/dev/null
DADDR=$(dcall wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
dcall generatetoaddress "[110, \"$DADDR\"]" >/dev/null

# Bob's Bitcoin wallet (funds the BTC lock).
"${BCLI[@]}" createwallet bob >/dev/null
BADDR=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 101 "$BADDR" >/dev/null

rc=0
for scenario in happy offline; do
  echo "=== $scenario ==="
  "$TOOL" "$scenario" "$DIR/swaps" "$DPORT" "$BPORT" test test "$DADDR" "$BADDR" || rc=1
done
[[ $rc -eq 0 ]] && echo "SWAP E2E: PASS" || { echo "SWAP E2E: FAIL"; tail -20 "$DIR/din.log"; exit 1; }
