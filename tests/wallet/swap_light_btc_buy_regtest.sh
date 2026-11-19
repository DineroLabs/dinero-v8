#!/usr/bin/env bash
# A whole DIN <-> BTC swap where NEITHER dinerod has Bitcoin Core.
#
# Alice sells DIN and Bob buys it; both run swap.btc_backend=light against the
# Bitcoin network (a regtest bitcoind over P2P) and an Esplora server. Bob has
# no Bitcoin wallet in his node: swap.deposit gives him an address, which is
# paid in two ordinary payments from an outside wallet, and his node builds
# the swap's lock from them and sends it. The deposit's excess returns to his
# refund address. A second deposit is cancelled and comes back whole.
#   usage: swap_light_btc_buy_regtest.sh <dinerod built with DINERO_SWAP_LIGHT_BTC>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}
command -v bitcoind >/dev/null || { echo "SKIP: bitcoind not installed"; exit 77; }
DIR=$(mktemp -d)
BASE=$(swap_free_base 12)
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3))
CPORT=$((BASE + 8)); CP2P=$((BASE + 9)); EPORT=$((BASE + 10))
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$CPORT -rpcuser=test -rpcpassword=test)
CONSENSUS=(--consensus-contextual-locks-height=1)
LIGHT=(--swap.enable=1 --swap.btc_backend=light --swap.btc_servers=http://127.0.0.1:$EPORT
       --swap.btc_peers=127.0.0.1 --swap.btc_peer_port=$CP2P --swap.btc_sync_seconds=1 --swap.tick_seconds=1)
cleanup() { for p in "${APID:-}" "${BPID:-}" "${EPID:-}"; do [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null || true; done
            "${BCLI[@]}" stop >/dev/null 2>&1 || true; sleep 1
            [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT

rpc() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$2" "$3")
  local resp; resp=$(curl -s --max-time 90 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$1/")
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
    -datadir="$DIR/$name" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 "${CONSENSUS[@]}" "$@" \
    >>"$DIR/$name.log" 2>&1 &
  STARTED_PID=$!
}
wait_rpc() {
  for _ in $(seq 1 90); do
    if rpc "$1" getblockcount '[]' >/dev/null 2>&1; then swap_require_regtest "dinerod :$1" "$(rpc "$1" getblockchaininfo '[]')"; return 0; fi
    sleep 1
  done
  return 1
}
fail=0
check() { if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; fail=1; fi; }
sats() { "${BCLI[@]}" scantxoutset start "[\"addr($1)\"]" | python3 -c 'import sys,json;print(round(json.load(sys.stdin)["total_amount"]*1e8))'; }
mine_btc() { "${BCLI[@]}" generatetoaddress "${1:-1}" "$BTC_MINER" >/dev/null; }

mkdir -p "$DIR/alice" "$DIR/bob" "$DIR/btc"
bitcoind -regtest -datadir="$DIR/btc" -listen=1 -bind=127.0.0.1 -port=$CP2P -rpcport=$CPORT -rpcuser=test -rpcpassword=test \
  -daemon -fallbackfee=0.0002 -txindex=1 -dnsseed=0 -fixedseeds=0 >/dev/null
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
swap_require_regtest bitcoind "$("${BCLI[@]}" getblockchaininfo)"
"${BCLI[@]}" createwallet outside >/dev/null     # stands for the buyer's phone wallet or exchange
BTC_MINER=$("${BCLI[@]}" getnewaddress)
mine_btc 101
ALICE_BTC=$("${BCLI[@]}" getnewaddress "" legacy)
BOB_REFUND=$("${BCLI[@]}" getnewaddress "" bech32)
python3 "$HERE/esplora_regtest_shim.py" $EPORT $CPORT test test >"$DIR/esplora.log" 2>&1 & EPID=$!
for _ in $(seq 1 50); do [[ "$(curl -s -m 5 http://127.0.0.1:$EPORT/blocks/tip/height)" == 101 ]] && break; sleep 0.2; done

start_node alice $APORT $AP2P; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.createhd '["w"]' >/dev/null
  rpc $port wallet.encrypt '["pw-swap-light"]' >/dev/null
done
kill "$APID" "$BPID"; wait "$APID" 2>/dev/null || true; wait "$BPID" 2>/dev/null || true; APID=; BPID=
start_node alice $APORT $AP2P "${LIGHT[@]}"; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P "${LIGHT[@]}" --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do rpc $port wallet.unlock '["pw-swap-light", 7200]' >/dev/null; done
check "neither daemon has a Bitcoin RPC" '! grep -q "btc_rpc=127" "$DIR/alice.log" "$DIR/bob.log"'
check "both say they buy from a deposit address" '[[ $(grep -c "buys DIN from a BTC deposit address" "$DIR/alice.log" "$DIR/bob.log" | cut -d: -f2 | paste -sd+ - | bc) -ge 2 ]]'

ALICE_DIN=$(rpc $APORT wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
rpc $APORT generatetoaddress "[110, \"$ALICE_DIN\"]" >/dev/null
for _ in $(seq 1 60); do [[ "$(rpc $BPORT getblockcount '[]')" == "$(rpc $APORT getblockcount '[]')" ]] && break; sleep 1; done
for _ in $(seq 1 30); do [[ -s "$DIR/alice/swaps/btc_headers.bin" && -s "$DIR/bob/swaps/btc_headers.bin" ]] && break; sleep 1; done

offer() { rpc $APORT swap.offer "{\"din_amount_una\":1000000000,\"btc_amount_sat\":1000000,\"btc_address\":\"$ALICE_BTC\",\"din_funding_fee_rate_hint\":2,\"din_funding_maximum_fee_una\":5000000}"; }

echo "=== a deposit that is cancelled comes back ==="
OFFER0_JSON=$(offer); OFFER0=$(field "$OFFER0_JSON" offer)
DEP0=$(rpc $BPORT swap.deposit "{\"text\":\"$OFFER0\",\"btc_refund_address\":\"$BOB_REFUND\"}")
ADDR0=$(field "$DEP0" deposit_address)
check "bob is given a bcrt1q deposit address and the amount due (1,003,000 sat) ($ADDR0)" \
  '[[ "$ADDR0" == bcrt1q* && "$(field "$DEP0" deposit_due_sat)" == 1003000 && "$(field "$DEP0" deposit_coin)" == BTC && "$(field "$DEP0" confirmations_needed)" == 2 ]]'
"${BCLI[@]}" sendtoaddress "$ADDR0" 0.004 >/dev/null; mine_btc 1; sleep 3
LIST=$(rpc $BPORT swap.list '[]')
check "the part payment shows: awaiting-deposit, 400,000 sat seen" \
  '[[ "$LIST" == *awaiting-deposit* && "$LIST" == *"\"deposit_seen_zat\": 400000"* ]]'
out=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER0\"}" 2>&1 || true)
check "accepting on a short deposit is refused with the shortfall (${out:0:90})" '[[ "$out" == *"deposit is short by"*BTC* ]]'
rpc $BPORT swap.cancel "[\"$(field "$DEP0" id)\"]" >/dev/null
mine_btc 1
BACK=$(sats "$BOB_REFUND")
check "the cancelled deposit is back at bob's refund address, less the network fee ($BACK sat)" '(( BACK > 399000 && BACK < 400000 ))'
check "and gone from his list" '[[ "$(rpc $BPORT swap.list "[]")" != *awaiting-deposit* ]]'
rpc $APORT swap.cancel "[\"$(field "$OFFER0_JSON" id)\"]" >/dev/null

echo "=== the swap ==="
OFFER_JSON=$(offer); OFFER=$(field "$OFFER_JSON" offer); ID=$(field "$OFFER_JSON" id)
DEP=$(rpc $BPORT swap.deposit "{\"text\":\"$OFFER\",\"btc_refund_address\":\"$BOB_REFUND\"}")
ADDR=$(field "$DEP" deposit_address)
check "a new swap, a new deposit address" '[[ "$ADDR" == bcrt1q* && "$ADDR" != "$ADDR0" ]]'
# Two payments, as a wallet topping up might make them: 0.007 + 0.0045 BTC = 1,150,000 sat (147,000 more than due).
"${BCLI[@]}" sendtoaddress "$ADDR" 0.007 >/dev/null; mine_btc 1
"${BCLI[@]}" sendtoaddress "$ADDR" 0.0045 >/dev/null; mine_btc 1; sleep 3
out=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}" 2>&1 || true)
check "one confirmation on the second payment is not enough yet (${out:0:80})" '[[ "$out" == *"deposit is short by"* ]]'
mine_btc 1; sleep 3
ACCEPT_JSON=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}")
ACCEPT=$(field "$ACCEPT_JSON" accept)
check "with two confirmations bob accepts, giving no address again" '[[ "$ACCEPT" == dinswap1a* ]]'
START_JSON=$(rpc $APORT swap.accept "{\"text\":\"$ACCEPT\"}")
check "alice started the swap" '[[ "$(field "$START_JSON" started)" == True ]]'

state() { field "$(rpc $1 swap.status "[\"$ID\"]" 2>/dev/null || echo '{}')" state; }
REFUND_BEFORE=$(sats "$BOB_REFUND")
for round in $(seq 1 ${LIGHT_ROUNDS:-150}); do
  rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null 2>&1 || true
  mine_btc 1
  sleep 1.5
  SA=$(state $APORT); SB=$(state $BPORT)
  (( round % 5 == 0 )) && echo "  [$round] alice=$SA bob=$SB"
  [[ "$SA" == done && "$SB" == done ]] && break
done
echo "  final: alice=$(state $APORT) bob=$(state $BPORT)"
check "alice done" '[[ "$(state $APORT)" == done ]]'
check "bob done" '[[ "$(state $BPORT)" == done ]]'
GOT_BTC=$(sats "$ALICE_BTC")
check "alice received the BTC, minus the claim fee ($GOT_BTC sat)" '[[ $GOT_BTC == 999000 ]]'
CHANGE=$(( $(sats "$BOB_REFUND") - REFUND_BEFORE ))
check "bob's deposit excess came back to his refund address: 150,000 sat less the lock's fee ($CHANGE sat)" '(( CHANGE > 147000 && CHANGE < 150000 ))'
check "nothing is left at the deposit address" '[[ "$(sats "$ADDR")" == 0 ]]'
BAL=$(rpc $BPORT wallet.getbalance '[]')
BOB_DIN=$(python3 -c 'import sys,json
d=json.loads(sys.argv[1]); t=d["total"]
print(int(t) if isinstance(t,int) and t > 10**6 else int(round(float(t)*1e8)))' "$BAL")
check "bob's wallet received the DIN ($BOB_DIN una)" '[[ $BOB_DIN -ge 999000000 && $BOB_DIN -le 1000000000 ]]'

[[ $fail -eq 0 ]] && echo "SWAP LIGHT BTC BUY REGTEST: PASS" || { echo "SWAP LIGHT BTC BUY REGTEST: FAIL"; grep -h "\[Swap\]" "$DIR"/*.log | tail -8; exit 1; }
