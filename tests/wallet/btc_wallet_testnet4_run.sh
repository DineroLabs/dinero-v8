#!/usr/bin/env bash
# The built-in Bitcoin wallet on the PUBLIC testnet4: real blocks, real
# servers (mempool.space), real peers (docs/design/multi-coin-wallet.md).
#
# One dinerod (a local Dinero regtest node, as in the swap rehearsal) turns
# the wallet on, is paid from an outside wallet, reads its balance, pays an
# address, raises that payment's fee, and finally sends everything back.
#
# Not a ctest: it needs test coins and real time. Run it again on the same
# workdir to carry on where it stopped.
#
#   usage: btc_wallet_testnet4_run.sh <dinerod> <workdir> <pay-script> <pay_to_address> <return_address> [sat] [max_minutes]
#   <pay-script> ADDRESS SAT pays the wallet from outside (the swap rehearsal's pay-deposit).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}; WORK=${2:?workdir}; PAY=${3:?pay script}; PAY_TO=${4:?address to pay}; RETURN=${5:?return address}
SAT=${6:-30000}; MAX=${7:-180}
mkdir -p "$WORK/node"
[[ -f "$WORK/ports" ]] || swap_free_base 2 >"$WORK/ports"
PORT=$(cat "$WORK/ports"); P2P=$((PORT + 1))
say() { echo "$(date '+%H:%M:%S') $*"; }
fail=0
check() { if eval "$2"; then say "OK   $1"; else say "FAIL $1"; fail=1; fi; }
cleanup() { [[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || true; wait 2>/dev/null || true; }
trap cleanup EXIT
rpc() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$1" "${2:-{\}}")
  local resp; resp=$(curl -s --max-time 180 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$PORT/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error"); r=d.get("result")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
if isinstance(r,dict) and r.get("error"): sys.stderr.write("RPC in-band error %s: %s\n"%(sys.argv[2],r["error"])); sys.exit(1)
print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$1"
}
jget() { python3 -c 'import sys,json
d=json.loads(sys.argv[1])
for k in sys.argv[2].split("."): d=d[int(k)] if isinstance(d,list) else d.get(k) if isinstance(d,dict) else None
print(d)' "$1" "$2"; }
start() {
  "$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$PORT -port=$P2P -datadir="$WORK/node" \
    -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 --consensus-contextual-locks-height=1 "$@" >>"$WORK/node.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 120); do rpc getblockcount '[]' >/dev/null 2>&1 && return 0; sleep 1; done
  echo "dinerod did not start"; return 1
}
# A public server that is busy for a moment is not a result: ask again a few times.
retry() { local out; for _ in 1 2 3 4 5 6; do if out=$("$@" 2>&1); then echo "$out"; return 0; fi; [[ "$out" == *"cannot reach any Bitcoin server"* ]] || { echo "$out"; return 1; }; sleep 30; done; echo "$out"; return 1; }
balance() { jget "$(rpc btc.getbalance '{}' 2>/dev/null || echo '{"sat":"?"}')" sat; }
wait_for() {  # wait_for "what" 'condition' minutes
  local until=$(( $(date +%s) + ${3:-$MAX} * 60 ))
  while (( $(date +%s) < until )); do eval "$2" && { say "     $1"; return 0; }; sleep 120; done
  say "FAIL waited ${3:-$MAX} min for: $1"; fail=1; return 1
}

if [[ ! -f "$WORK/wallet.made" ]]; then
  start; rpc wallet.createhd '["w"]' >/dev/null; rpc wallet.encrypt '["pw-btcwallet"]' >/dev/null
  kill "$PID"; wait "$PID" 2>/dev/null || true; PID=; touch "$WORK/wallet.made"
fi
start --swap.enable=1 --swap.btc_backend=light --swap.btc_rehearsal_chain=testnet4
rpc wallet.unlock '["pw-btcwallet", 86400]' >/dev/null
ST=$(rpc btc.enable '{}'); ADDR=$(jget "$ST" address)
check "the wallet is on, with a testnet address ($ADDR)" '[[ "$ADDR" == tb1q* ]]'
wait_for "the node has Bitcoin's testnet4 headers and reads an (empty or paid) balance: $(balance) sat" '[[ "$(balance)" =~ ^[0-9]+$ ]]' 30

if [[ ! -f "$WORK/paid" ]]; then
  say "paying the wallet $SAT sat from outside"
  "$PAY" "$ADDR" "$SAT" | tee "$WORK/paid.tmp"; mv "$WORK/paid.tmp" "$WORK/paid"
fi
if [[ ! -f "$WORK/sent" ]]; then
  wait_for "the payment is confirmed and proven: the wallet holds $SAT sat" '[[ "$(balance)" == "$SAT" ]]'
  rpc wallet.lock '[]' >/dev/null 2>&1 || true
  PV=$(retry rpc btc.preview "{\"address\":\"$PAY_TO\",\"amount_sat\":10000,\"target_blocks\":24}")
  say "preview (wallet locked): $PV"
  check "the preview adds up to the wallet's coin" '(( $(jget "$PV" amount_sat) + $(jget "$PV" fee_sat) + $(jget "$PV" change_sat) == SAT ))'
  out=$(rpc btc.send "{\"address\":\"$PAY_TO\",\"amount_sat\":10000,\"target_blocks\":24,\"txid\":\"$(jget "$PV" txid)\"}" 2>&1 || true)
  check "a locked wallet does not send (${out:0:70})" '[[ "$out" == *"locked"* ]]'
  rpc wallet.unlock '["pw-btcwallet", 86400]' >/dev/null
  SENT=$(retry rpc btc.send "{\"address\":\"$PAY_TO\",\"amount_sat\":10000,\"target_blocks\":24,\"txid\":\"$(jget "$PV" txid)\"}")
  say "sent: $(jget "$SENT" txid) fee $(jget "$SENT" fee_sat) sat"
  sleep 20
  seen=$(curl -s -m 30 "https://mempool.space/testnet4/api/tx/$(jget "$SENT" txid)/status" || true)
  check "the public server has the payment (${seen:0:60})" '[[ "$seen" == *confirmed* ]]'
  BS=$(retry rpc btc.bumpfee "{\"txid\":\"$(jget "$SENT" txid)\"}")
  BD=$(rpc btc.bumpfee "{\"txid\":\"$(jget "$SENT" txid)\",\"new_txid\":\"$(jget "$BS" txid)\"}" 2>&1 || true)
  say "fee raised: $BD" | cut -c1-260
  if [[ "$BD" == *'"sent": true'* ]]; then
    echo "$(jget "$BS" txid) $(jget "$BS" fee_sat) bumped" >"$WORK/sent"
  else  # the first one may already be in a block: then there is nothing to replace
    echo "$(jget "$SENT" txid) $(jget "$SENT" fee_sat) first" >"$WORK/sent"
  fi
fi
read -r TXID FEE WHICH <"$WORK/sent"
wait_for "the payment ($WHICH, fee $FEE sat) is confirmed: the wallet holds $((SAT - 10000 - FEE)) sat" '[[ "$(balance)" == "$((SAT - 10000 - FEE))" ]]' || {
  say "     balance now $(balance); the list: $(rpc btc.listtransactions '{}' 2>&1 | cut -c1-400)"; }
conf=$(curl -s -m 30 "https://mempool.space/testnet4/api/tx/$TXID/status" || true)
check "the public chain has it in a block (${conf:0:80})" '[[ "$conf" == *"\"confirmed\":true"* ]]'
say "the list: $(retry rpc btc.listtransactions '{}' | cut -c1-500)"

if [[ ! -f "$WORK/returned" ]]; then
  PVA=$(retry rpc btc.preview "{\"address\":\"$RETURN\",\"send_all\":true,\"target_blocks\":6}")
  retry rpc btc.send "{\"address\":\"$RETURN\",\"send_all\":true,\"target_blocks\":6,\"txid\":\"$(jget "$PVA" txid)\"}" >/dev/null
  echo "$(jget "$PVA" txid) $(jget "$PVA" amount_sat)" >"$WORK/returned"
  say "everything left ($(jget "$PVA" amount_sat) sat) sent back to $RETURN: $(jget "$PVA" txid)"
fi
wait_for "the wallet is empty" '[[ "$(balance)" == 0 ]]'
[[ $fail -eq 0 ]] && say "BTC WALLET TESTNET4 RUN: PASS" || { say "BTC WALLET TESTNET4 RUN: FAIL"; exit 1; }
