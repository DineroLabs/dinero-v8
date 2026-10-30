#!/usr/bin/env bash
# A DIN <-> BTC swap whose Bitcoin leg runs on the public Bitcoin testnet4
# (tests/wallet/swap_testnet4_tool.cpp). Not a ctest: it needs a synced
# testnet4 bitcoind whose wallet holds some tBTC, and it takes real time
# (30 local Dinero blocks plus real testnet4 confirmations).
# Dinero is a local regtest node with transaction locks enforced from height 1
# (Dinero testnet does not enforce them, so swaps there are refused). It and the
# swap stores live in <workdir>; run again on the same workdir to resume.
#   usage: swap_testnet4_rehearsal.sh <dinerod> <swap_testnet4_tool> <btc_datadir> <workdir> [btc_sat] [max_minutes] [scenario]
#   scenario: happy (default) or bob-refund (~14 h: see the tool's header)
#   btc_datadir: the testnet4 bitcoind's -datadir (RPC 127.0.0.1:48332, cookie auth)
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}; TOOL=${2:?swap_testnet4_tool}; BTCDIR=${3:?btc_datadir}; WORK=${4:?workdir}
SAT=${5:-100000}; MAX=${6:-240}; SCENARIO=${7:-happy}
BCLI=(bitcoin-cli -datadir="$BTCDIR")
[[ "$("${BCLI[@]}" getblockchaininfo | python3 -c 'import sys,json; print(json.load(sys.stdin)["chain"])')" == testnet4 ]] ||
  { echo "ABORT: $BTCDIR is not a testnet4 node"; exit 1; }
COOKIE=$(cat "$BTCDIR/testnet4/.cookie")

mkdir -p "$WORK/din"
DPORT=$(swap_free_base 2); DP2P=$((DPORT + 1))
"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$DPORT -port=$DP2P \
  -datadir="$WORK/din" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 -connect=0 \
  --consensus-contextual-locks-height=1 >>"$WORK/din.log" 2>&1 & NPID=$!
trap 'kill $NPID 2>/dev/null || true' EXIT
dcall() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$1" "$2")
  curl -s --max-time 120 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:${DPORT}/" |
    python3 -c 'import sys,json
d=json.load(sys.stdin); e=d.get("error")
if e: sys.stderr.write("RPC error: %s\n"%e); sys.exit(1)
r=d.get("result"); print(r if isinstance(r,str) else json.dumps(r))'
}
for _ in $(seq 1 90); do dcall getblockcount '[]' >/dev/null 2>&1 && break; sleep 1; done
swap_require_regtest dinerod "$(dcall getblockchaininfo '[]')"

# Alice's Dinero wallet funds the DIN lock (made once per workdir).
if [[ ! -f "$WORK/din_miner" ]]; then
  dcall wallet.createhd '["alice"]' >/dev/null
  dcall wallet.encrypt '["pw-swap-testnet4"]' >/dev/null
  dcall wallet.unlock '["pw-swap-testnet4", 86400]' >/dev/null
  dcall wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)' > "$WORK/din_miner"
  dcall generatetoaddress "[110, \"$(cat "$WORK/din_miner")\"]" >/dev/null
else
  dcall wallet.unlock '["pw-swap-testnet4", 86400]' >/dev/null
fi
echo "dinerod regtest on $DPORT (height $(dcall getblockcount '[]')); bitcoind testnet4 block $("${BCLI[@]}" getblockcount)"
echo "testnet4 wallet: $("${BCLI[@]}" getbalance) tBTC"
"$TOOL" "$SCENARIO" "$WORK" "$DPORT" "$(cat "$WORK/din_miner")" 48332 __cookie__ "${COOKIE#__cookie__:}" "$SAT" "$MAX"
