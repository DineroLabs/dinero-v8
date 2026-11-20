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
#          [<dinero-swap-tower> <dinero-swap-tower-intake>]
#
# With the two tower programs (CTest SwapPublicTowerRegtest) Bob also has a
# public watchtower: his node arms it over HTTP (swap.tower_url), and he goes
# offline the moment his BTC is locked. The tower, which has only his
# pre-signed transactions and no Bitcoin Core either, claims his DIN.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}
TOWER=${2:-}; INTAKE=${3:-}
command -v bitcoind >/dev/null || { echo "SKIP: bitcoind not installed"; exit 77; }
DIR=$(mktemp -d)
BASE=$(swap_free_base 12)
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3))
CPORT=$((BASE + 8)); CP2P=$((BASE + 9)); EPORT=$((BASE + 10)); TPORT=$((BASE + 11))
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$CPORT -rpcuser=test -rpcpassword=test)
CONSENSUS=(--consensus-contextual-locks-height=1)
LIGHT=(--swap.enable=1 --swap.btc_backend=light --swap.btc_servers=http://127.0.0.1:$EPORT
       --swap.btc_peers=127.0.0.1 --swap.btc_peer_port=$CP2P --swap.btc_sync_seconds=1 --swap.tick_seconds=1)
cleanup() { for p in "${APID:-}" "${BPID:-}" "${EPID:-}" "${TPID:-}" "${IPID:-}"; do [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null || true; done
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
BOBCFG=("${LIGHT[@]}")
if [[ -n "$TOWER" ]]; then
  # The public tower: its door, and the tower guarding the same inbox (its
  # Dinero node is Alice's here; a real one runs its own).
  mkdir -p "$DIR/tower"
  "$INTAKE" --inbox "$DIR/tower" --listen 127.0.0.1:$TPORT --network regtest >"$DIR/intake.log" 2>&1 & IPID=$!
  for _ in $(seq 1 50); do curl -s -m 2 http://127.0.0.1:$TPORT/v1/health | grep -q '"ok":true' && break; sleep 0.2; done
  check "the tower's door answers" '[[ "$(curl -s -m 5 http://127.0.0.1:$TPORT/v1/health)" == *"\"ok\":true"* ]]'
  check "and refuses what is not a package" '[[ "$(curl -s -m 5 -o /dev/null -w "%{http_code}" -d junk http://127.0.0.1:$TPORT/v1/packages)" == 400 ]]'
  check "and a body over its limit" '[[ "$(head -c 300000 /dev/zero | tr "\0" a | curl -s -m 10 -o /dev/null -w "%{http_code}" --data-binary @- http://127.0.0.1:$TPORT/v1/packages)" == 413 ]]'
  BOBCFG+=(--swap.tower_url=http://127.0.0.1:$TPORT)
fi
start_node alice $APORT $AP2P "${LIGHT[@]}"; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P "${BOBCFG[@]}" --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
if [[ -n "$TOWER" ]]; then
  # It reads the node's credentials from a cookie file, which is wrong at first
  # (as after the node restarted) and is put right below: the tower must follow.
  echo "test:stale" >"$DIR/tower.cookie"
  "$TOWER" --inbox "$DIR/tower" --din-rpc 127.0.0.1:$APORT --din-cookie "$DIR/tower.cookie" --din-hrp rdin --btc-chain regtest \
    --btc-backend light --btc-servers http://127.0.0.1:$EPORT --btc-peers 127.0.0.1 --btc-peer-port $CP2P \
    --interval 1 >"$DIR/tower.log" 2>&1 & TPID=$!
  # Put right only once the buyer's package is waiting: he locks nothing until
  # the tower, with the new cookie, confirms.
  (for _ in $(seq 1 600); do ls "$DIR/tower" 2>/dev/null | grep -q '\.pkg$' && break; sleep 1; done; sleep 4
   echo "test:test" >"$DIR/tower.cookie.new"; mv "$DIR/tower.cookie.new" "$DIR/tower.cookie"; touch "$DIR/cookie.fixed") &
fi
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
naddr() { rpc $BPORT wallet.listaddresses '[]' | python3 -c 'import sys,json; print(len(json.load(sys.stdin)))'; }
ADDR_BEFORE=$(naddr)
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
check "the refused accepts took no address from bob's wallet ($ADDR_BEFORE before, $(naddr) now)" '[[ "$(naddr)" == "$ADDR_BEFORE" ]]'
ACCEPT_JSON=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}")
check "the accept that went through took exactly one" '[[ "$(naddr)" == "$((ADDR_BEFORE + 1))" ]]'
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
  SA=$(state $APORT); SB=${BOB_OFFLINE:+offline}; [[ -n "$SB" ]] || SB=$(state $BPORT)
  (( round % 5 == 0 )) && echo "  [$round] alice=$SA bob=$SB"
  if [[ -n "$TOWER" && -z "${BOB_OFFLINE:-}" && "$SB" == btc-locked ]]; then
    PKG=$(ls "$DIR/tower" | grep -E '^[0-9a-f]{64}\.pkg$' | head -1)
    check "bob's node armed the tower over HTTP before locking: the inbox holds his package under its hash" '[[ -n "$PKG" ]]'
    BOB_STATUS=$(rpc $BPORT swap.status "[\"$ID\"]")
    check "the tower could not see Dinero with the stale cookie, and followed the new one" \
      '[[ -e "$DIR/cookie.fixed" ]] && grep -q "Dinero node is unreachable\|neither node is reachable" "$DIR/tower.log"'
    check "his node says the tower is armed" '[[ "$(field "$BOB_STATUS" tower_armed)" == True ]]'
    echo "  bob goes offline with his BTC locked"
    kill "$BPID"; wait "$BPID" 2>/dev/null || true; BPID=; BOB_OFFLINE=1
  fi
  if [[ -n "${BOB_OFFLINE:-}" ]]; then
    # Alice has her BTC and the tower has claimed Bob's DIN and seen it buried.
    [[ "$SA" == done ]] && ls "$DIR/tower" | grep -q '\.pkg\.done$' && break
    continue
  fi
  [[ "$SA" == done && "$SB" == done ]] && break
done
if [[ -n "${BOB_OFFLINE:-}" ]]; then
  check "the tower finished the swap for bob while he was away" 'ls "$DIR/tower" | grep -q "\.pkg\.done$"'
  grep -h "tower:" "$DIR/tower.log" | grep -iE "claim|secret" | tail -3 | cut -c1-200
  start_node bob $BPORT $BP2P "${BOBCFG[@]}" --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
  rpc $BPORT wallet.unlock '["pw-swap-light", 7200]' >/dev/null
  for _ in $(seq 1 60); do
    rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null 2>&1 || true
    mine_btc 1; sleep 1.5
    [[ "$(state $BPORT)" == done ]] && break
  done
fi
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
# (A claim sent by the tower pays one of its pre-signed fee levels, up to 1 % of the amount.)
MIN_DIN=999000000; [[ -n "$TOWER" ]] && MIN_DIN=989000000
check "bob's wallet received the DIN ($BOB_DIN una)" '[[ $BOB_DIN -ge $MIN_DIN && $BOB_DIN -le 1000000000 ]]'

echo "=== each node reads the Bitcoin its swaps paid to (watch-only) ==="
holding() {  # holding JSON ADDRESS -> "sat sale_proceeds purchase_refund rows complete"
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); rows=d.get("addresses",[])
r=[a for a in rows if a["address"]==sys.argv[2]]
print(r[0].get("sat","?") if r else "missing", r[0]["sale_proceeds"] if r else "-", r[0]["purchase_refund"] if r else "-", len(rows), d.get("complete"))' "$1" "$2"
}
HA=$(rpc $APORT swap.btcholdings '{}'); echo "  alice: $HA" | cut -c1-260
check "alice sees the BTC she sold for at her payout address ($(holding "$HA" "$ALICE_BTC"))" '[[ "$(holding "$HA" "$ALICE_BTC")" == "999000 True False 1 True" ]]'
HB=$(rpc $BPORT swap.btcholdings '{}'); echo "  bob: $HB" | cut -c1-260
check "bob sees what came back to his refund address ($(holding "$HB" "$BOB_REFUND"))" '[[ "$(holding "$HB" "$BOB_REFUND")" == "$(sats "$BOB_REFUND") False True 1 True" ]]'

echo "=== BTC that reaches a deposit address late is recovered ==="
# A second withdrawal from an exchange arrives after the swap is over, and something lands on the first, cancelled address.
"${BCLI[@]}" sendtoaddress "$ADDR" 0.0006 >/dev/null
"${BCLI[@]}" sendtoaddress "$ADDR0" 0.0002 >/dev/null
mine_btc 1; sleep 3
HOME_ADDR=$("${BCLI[@]}" getnewaddress "" bech32m)
REC=$(rpc $BPORT swap.recoverdeposits "{\"btc_address\":\"$HOME_ADDR\"}")
echo "  $REC" | cut -c1-240
check "both stray payments are found (80,000 sat)" '[[ "$(field "$REC" found_sat)" == 80000 ]]'
mine_btc 1
GOT=$(sats "$HOME_ADDR")
check "and arrive at the address given, less two network fees ($GOT sat)" '(( GOT > 79000 && GOT < 80000 ))'
check "the deposit addresses are empty again" '[[ "$(sats "$ADDR")" == 0 && "$(sats "$ADDR0")" == 0 ]]'
REC2=$(rpc $BPORT swap.recoverdeposits "{\"btc_address\":\"$HOME_ADDR\"}")
check "a second look finds nothing" '[[ "$(field "$REC2" found_sat)" == 0 ]]'
out=$(rpc $BPORT swap.recoverdeposits '{"btc_address":"not-an-address"}' 2>&1 || true)
check "a bad address is refused (${out:0:60})" '[[ "$out" == *"not a Bitcoin address"* ]]'

[[ $fail -eq 0 ]] && echo "SWAP LIGHT BTC BUY REGTEST: PASS" || { echo "SWAP LIGHT BTC BUY REGTEST: FAIL"; grep -h "\[Swap\]" "$DIR"/*.log | tail -8; exit 1; }
