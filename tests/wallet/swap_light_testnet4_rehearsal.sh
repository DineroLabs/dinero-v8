#!/usr/bin/env bash
# The testnet rehearsal of swaps without Bitcoin Core
# (docs/design/din-btc-swaps-without-bitcoin-core.md, section 9, step 3).
#
# Two dinerod nodes, neither with Bitcoin Core, swap DIN for BTC where the
# Bitcoin side is the PUBLIC testnet4: real blocks, real servers
# (mempool.space), real peers. Bob pays a deposit address from an outside
# wallet, arms a watchtower over HTTP, and the swap runs to the end.
#
# Dinero is a local regtest pair with transaction locks from height 1: Dinero's
# testnet does not enforce time locks, so no swap can run there
# (swap.btc_rehearsal_chain=testnet4 is the pairing).
#
# Not a ctest: it needs test coins and hours of real time. Everything lives in
# <workdir>; run it again on the same workdir to carry on where it stopped.
#
#   usage: swap_light_testnet4_rehearsal.sh <dinerod> <dinero-swap-tower> <dinero-swap-tower-intake>
#            <workdir> <alice_btc_address> <bob_refund_address> [btc_sat] [max_minutes]
#   The deposit is paid by hand, or by <workdir>/pay-deposit (ADDRESS SAT) if it exists.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}; TOWER=${2:?tower}; INTAKE=${3:?intake}; WORK=${4:?workdir}
ALICE_BTC=${5:?alice btc address}; BOB_REFUND=${6:?bob refund address}; SAT=${7:-20000}; MAX=${8:-110}
API=https://mempool.space/testnet4/api
mkdir -p "$WORK/alice" "$WORK/bob" "$WORK/tower"
[[ -f "$WORK/ports" ]] || swap_free_base 6 >"$WORK/ports"
BASE=$(cat "$WORK/ports"); APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3)); TPORT=$((BASE + 4))
LIGHT=(--swap.enable=1 --swap.btc_backend=light --swap.btc_rehearsal_chain=testnet4 --swap.tick_seconds=20)
say() { echo "$(date '+%H:%M:%S') $*"; }
cleanup() { for p in "${APID:-}" "${BPID:-}" "${TPID:-}" "${IPID:-}"; do [[ -n "$p" ]] && kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; }
trap cleanup EXIT
rpc() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$2" "$3")
  local resp; resp=$(curl -s --max-time 120 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$1/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error"); r=d.get("result")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
if isinstance(r,dict) and r.get("error"): sys.stderr.write("RPC in-band error %s: %s\n"%(sys.argv[2],r["error"])); sys.exit(1)
print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$2"
}
field() { python3 -c 'import sys,json; d=json.loads(sys.argv[1]); print(d.get(sys.argv[2],""))' "$1" "$2"; }
start_node() {
  local name=$1 port=$2 p2p=$3; shift 3
  "$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$port -port=$p2p \
    -datadir="$WORK/$name" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    --consensus-contextual-locks-height=1 "$@" >>"$WORK/$name.log" 2>&1 &
  STARTED_PID=$!
}
wait_rpc() {
  for _ in $(seq 1 120); do
    if rpc "$1" getblockcount '[]' >/dev/null 2>&1; then swap_require_regtest "dinerod :$1" "$(rpc "$1" getblockchaininfo '[]')"; return 0; fi
    sleep 1
  done
  echo "dinerod on $1 did not start"; return 1
}
btc_at() { curl -s -m 30 "$API/address/$1" | python3 -c 'import sys,json
d=json.load(sys.stdin); c=d["chain_stats"]; m=d["mempool_stats"]
print(c["funded_txo_sum"]-c["spent_txo_sum"], m["funded_txo_sum"]-m["spent_txo_sum"])'; }

if [[ ! -f "$WORK/wallets.made" ]]; then
  say "first run: making the two Dinero wallets"
  start_node alice $APORT $AP2P; APID=$STARTED_PID; wait_rpc $APORT
  start_node bob $BPORT $BP2P --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
  for port in $APORT $BPORT; do
    rpc $port wallet.createhd '["w"]' >/dev/null
    rpc $port wallet.encrypt '["pw-rehearsal"]' >/dev/null
  done
  kill "$APID" "$BPID"; wait "$APID" 2>/dev/null || true; wait "$BPID" 2>/dev/null || true; APID=; BPID=
  touch "$WORK/wallets.made"
fi
"$INTAKE" --inbox "$WORK/tower" --listen 127.0.0.1:$TPORT --network regtest >>"$WORK/intake.log" 2>&1 & IPID=$!
start_node alice $APORT $AP2P "${LIGHT[@]}"; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P "${LIGHT[@]}" --swap.tower_url=http://127.0.0.1:$TPORT --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do rpc $port wallet.unlock '["pw-rehearsal", 86400]' >/dev/null; done
"$TOWER" --inbox "$WORK/tower" --din-rpc 127.0.0.1:$APORT --din-auth test:test --din-hrp rdin --btc-chain testnet4 \
  --btc-backend light --btc-servers $API --interval 20 >>"$WORK/tower.log" 2>&1 & TPID=$!
grep -h "\[Swap\]" "$WORK/alice.log" | tail -2

if [[ ! -f "$WORK/din_miner" ]]; then
  rpc $APORT wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)' >"$WORK/din_miner"
  rpc $APORT generatetoaddress "[110, \"$(cat "$WORK/din_miner")\"]" >/dev/null
fi
ALICE_DIN=$(cat "$WORK/din_miner")
mine_din() { rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null 2>&1 || true; }
for _ in $(seq 1 60); do [[ "$(rpc $BPORT getblockcount '[]')" == "$(rpc $APORT getblockcount '[]')" ]] && break; sleep 1; done

say "waiting for both nodes' own view of Bitcoin testnet4 (headers over the Bitcoin network)"
for _ in $(seq 1 90); do [[ -s "$WORK/alice/swaps/btc_headers.bin" && -s "$WORK/bob/swaps/btc_headers.bin" ]] && break; sleep 2; done
say "testnet4 tip by the server: $(curl -s -m 20 $API/blocks/tip/height); header files: $(wc -c <"$WORK/alice/swaps/btc_headers.bin") and $(wc -c <"$WORK/bob/swaps/btc_headers.bin") bytes"

if [[ ! -f "$WORK/offer.json" ]]; then
  rpc $APORT swap.offer "{\"din_amount_una\":100000000,\"btc_amount_sat\":$SAT,\"btc_address\":\"$ALICE_BTC\",\"expires_minutes\":600,\"din_funding_fee_rate_hint\":2,\"din_funding_maximum_fee_una\":5000000}" >"$WORK/offer.json"
  say "alice offers 1 DIN for $SAT sat (offer good for 10 hours)"
fi
OFFER=$(field "$(cat "$WORK/offer.json")" offer); ID=$(field "$(cat "$WORK/offer.json")" id)
if [[ ! -f "$WORK/deposit.json" ]]; then
  rpc $BPORT swap.deposit "{\"text\":\"$OFFER\",\"btc_refund_address\":\"$BOB_REFUND\"}" >"$WORK/deposit.json"
fi
ADDR=$(field "$(cat "$WORK/deposit.json")" deposit_address); DUE=$(field "$(cat "$WORK/deposit.json")" deposit_due_sat)
say "bob's deposit address: $ADDR   due: $DUE sat   (testnet4)"
state() { field "$(rpc $1 swap.status "[\"$ID\"]" 2>/dev/null || echo '{}')" state; }
END=$(( $(date +%s) + MAX * 60 ))

if [[ ! -f "$WORK/accept.json" ]]; then
  read -r CONF UNCONF <<<"$(btc_at "$ADDR")"
  if (( CONF + UNCONF < DUE )) && [[ ! -f "$WORK/deposit.paid" ]]; then
    if [[ -x "$WORK/pay-deposit" ]]; then
      say "paying the deposit from the outside wallet"
      "$WORK/pay-deposit" "$ADDR" "$DUE" | tee "$WORK/deposit.paid"
    else
      say "PAY $DUE sat to $ADDR on testnet4, then run this again"; exit 3
    fi
  fi
  say "waiting for the deposit's 2 confirmations as bob's node sees them"
  while :; do
    out=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}" 2>&1) && { echo "$out" >"$WORK/accept.json"; break; }
    (( $(date +%s) > END )) && { say "not confirmed yet (${out:0:120}); run again to carry on"; exit 4; }
    sleep 60
  done
  say "bob accepted: his node will build the lock from the deposit"
fi
if [[ ! -f "$WORK/started" ]]; then
  rpc $APORT swap.accept "{\"text\":\"$(field "$(cat "$WORK/accept.json")" accept)\"}" >"$WORK/started"
  say "alice started the swap"
fi

LAST=""
while :; do
  mine_din
  SA=$(state $APORT); SB=$(state $BPORT)
  [[ "$SA/$SB" != "$LAST" ]] && { say "alice=$SA bob=$SB  (testnet4 tip $(curl -s -m 20 $API/blocks/tip/height))"; LAST="$SA/$SB"; }
  [[ "$SA" == done && "$SB" == done ]] && break
  (( $(date +%s) > END )) && { say "still running; run again to carry on"; exit 4; }
  sleep 20
done
fail=0
check() { if eval "$2"; then say "OK   $1"; else say "FAIL $1"; fail=1; fi; }
read -r GOT GOT_UNCONF <<<"$(btc_at "$ALICE_BTC")"
check "alice received the BTC on testnet4, less her claim fee ($GOT sat confirmed)" '(( GOT > SAT - 3000 && GOT <= SAT ))'
read -r LEFT LEFT_UNCONF <<<"$(btc_at "$ADDR")"
check "nothing is left at the deposit address" '(( LEFT + LEFT_UNCONF == 0 ))'
BAL=$(rpc $BPORT wallet.getbalance '[]')
BOB_DIN=$(python3 -c 'import sys,json
d=json.loads(sys.argv[1]); t=d["total"]
print(int(t) if isinstance(t,int) and t > 10**6 else int(round(float(t)*1e8)))' "$BAL")
check "bob's wallet received the DIN ($BOB_DIN una)" '(( BOB_DIN >= 98000000 && BOB_DIN <= 100000000 ))'
check "bob's node had armed the watchtower over HTTP" 'ls "$WORK/tower" | grep -c "\.pkg" >/dev/null'
rpc $BPORT swap.status "[\"$ID\"]" | python3 -c 'import sys,json
for e in json.load(sys.stdin).get("events",[]): print("   bob:", e[:150])'
rpc $APORT swap.status "[\"$ID\"]" | python3 -c 'import sys,json
for e in json.load(sys.stdin).get("events",[]): print("   alice:", e[:150])'
[[ $fail -eq 0 ]] && say "TESTNET4 REHEARSAL: PASS" || { say "TESTNET4 REHEARSAL: FAIL"; exit 1; }
