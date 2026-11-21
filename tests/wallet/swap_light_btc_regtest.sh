#!/usr/bin/env bash
# Selling DIN for BTC with NO Bitcoin Core on the seller's side.
#
# Alice's dinerod runs swap.btc_backend=light: Bitcoin headers and broadcast go
# over the Bitcoin P2P protocol, lookups to two Esplora servers, one of which
# hides everything it is asked. She has no Bitcoin RPC at all. Bob's dinerod is
# an ordinary swap node with Bitcoin Core. The swap is driven through RPCs
# only; Alice's daemon is killed and restarted mid-swap; her BTC is paid to a
# legacy (1…-style) address. (Buying without Bitcoin Core: swap_light_btc_buy_regtest.sh.)
#   usage: swap_light_btc_regtest.sh <dinerod built with DINERO_SWAP_LIGHT_BTC>
# To see the test fail when the servers cannot be trusted (it must):
#   LIGHT_SERVER_FLAGS=--hide        the second server hides everything too
#   LIGHT_SERVER_FLAGS=--bad-proofs LIGHT_HIDER_FLAGS=--bad-proofs
#                                    every Merkle proof served is wrong (one honest proof,
#                                    from either server, is enough: it is checked, not believed)
#   LIGHT_ROUNDS=N                   give up after N rounds (default 150)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}
command -v bitcoind >/dev/null || { echo "SKIP: bitcoind not installed"; exit 77; }
DIR=$(mktemp -d)
BASE=$(swap_free_base 12)
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3))
CPORT=$((BASE + 8)); CP2P=$((BASE + 9))        # bitcoind RPC and P2P
EPORT=$((BASE + 10)); HPORT=$((BASE + 11))     # the honest server and the one that hides
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$CPORT -rpcuser=test -rpcpassword=test)
CONSENSUS=(--consensus-contextual-locks-height=1)
BOBCFG=(--swap.enable=1 --swap.btc_rpc=127.0.0.1:$CPORT --swap.btc_rpc_user=test --swap.btc_rpc_pass=test --swap.tick_seconds=1)
ALICECFG=(--swap.enable=1 --swap.btc_backend=light --swap.btc_servers=http://127.0.0.1:$HPORT,http://127.0.0.1:$EPORT
          --swap.btc_peers=127.0.0.1 --swap.btc_peer_port=$CP2P --swap.btc_sync_seconds=1 --swap.tick_seconds=1)
cleanup() { for p in "${APID:-}" "${BPID:-}" "${EPID:-}" "${HPID:-}"; do [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null || true; done
            "${BCLI[@]}" stop >/dev/null 2>&1 || true; sleep 1
            [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT

rpc() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$2" "$3")
  local resp; resp=$(curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$1/")
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

mkdir -p "$DIR/alice" "$DIR/bob" "$DIR/btc"
bitcoind -regtest -datadir="$DIR/btc" -listen=1 -bind=127.0.0.1 -port=$CP2P -rpcport=$CPORT -rpcuser=test -rpcpassword=test \
  -daemon -fallbackfee=0.0002 -txindex=1 -dnsseed=0 -fixedseeds=0 >/dev/null
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
swap_require_regtest bitcoind "$("${BCLI[@]}" getblockchaininfo)"
"${BCLI[@]}" createwallet bob >/dev/null
BTC_MINER=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 101 "$BTC_MINER" >/dev/null
ALICE_BTC=$("${BCLI[@]}" getnewaddress "" legacy)   # a 1…-style address, as an exchange might hand out (m…/n… on regtest)
BOB_BTC_REFUND=$("${BCLI[@]}" getnewaddress "" bech32m)
python3 "$HERE/esplora_regtest_shim.py" $EPORT $CPORT test test ${LIGHT_SERVER_FLAGS:-} >"$DIR/esplora.log" 2>&1 & EPID=$!
python3 "$HERE/esplora_regtest_shim.py" $HPORT $CPORT test test --hide ${LIGHT_HIDER_FLAGS:-} >"$DIR/esplora-hide.log" 2>&1 & HPID=$!
for _ in $(seq 1 50); do [[ "$(curl -s -m 5 http://127.0.0.1:$EPORT/blocks/tip/height)" == 101 ]] && break; sleep 0.2; done
check "the test Esplora server answers" '[[ "$(curl -s -m 5 http://127.0.0.1:$EPORT/blocks/tip/height)" == 101 ]]'

# Establish each intended payer before SwapService captures its durable owner.
start_node alice $APORT $AP2P; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.createhd '["w"]' >/dev/null
  rpc $port wallet.encrypt '["pw-swap-light"]' >/dev/null
done
kill "$APID" "$BPID"; wait "$APID" 2>/dev/null || true; wait "$BPID" 2>/dev/null || true; APID=; BPID=
start_node alice $APORT $AP2P "${ALICECFG[@]}"; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P "${BOBCFG[@]}" --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do rpc $port wallet.unlock '["pw-swap-light", 7200]' >/dev/null; done
check "alice's daemon says how it reaches Bitcoin" 'grep -q "Bitcoin without Bitcoin Core: 2 server(s), 1 peer seed(s)" "$DIR/alice.log"'
check "alice offers BTC swaps without any Bitcoin RPC" '[[ "$(rpc $APORT swap.capabilities "[]")" == *btc* ]]'
check "alice reports the light backend, bob Bitcoin Core" \
  '[[ "$(field "$(rpc $APORT swap.capabilities "[]")" btc_backend)" == light && "$(field "$(rpc $BPORT swap.capabilities "[]")" btc_backend)" == core ]]'

ALICE_DIN=$(rpc $APORT wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
rpc $APORT generatetoaddress "[110, \"$ALICE_DIN\"]" >/dev/null
for _ in $(seq 1 60); do [[ "$(rpc $BPORT getblockcount '[]')" == "$(rpc $APORT getblockcount '[]')" ]] && break; sleep 1; done
for _ in $(seq 1 30); do [[ -s "$DIR/alice/swaps/btc_headers.bin" ]] && break; sleep 1; done
check "alice has synced Bitcoin headers over P2P (the header file exists)" '[[ -s "$DIR/alice/swaps/btc_headers.bin" ]]'
check "101 headers of 80 bytes after a short label" '(( $(wc -c <"$DIR/alice/swaps/btc_headers.bin") > 101 * 80 && $(wc -c <"$DIR/alice/swaps/btc_headers.bin") < 101 * 80 + 100 ))'

OFFER_JSON=$(rpc $APORT swap.offer "{\"din_amount_una\":1000000000,\"btc_amount_sat\":1000000,\"btc_address\":\"$ALICE_BTC\",\"din_funding_fee_rate_hint\":2,\"din_funding_maximum_fee_una\":5000000}")
OFFER=$(field "$OFFER_JSON" offer); ID=$(field "$OFFER_JSON" id)
echo "  offer $ID to be paid at $ALICE_BTC"
out=$(rpc $APORT swap.accept "{\"text\":\"$OFFER\"}" 2>&1 || true)
check "a light node that has paid no deposit cannot accept an offer (${out:0:70})" '[[ "$out" == *"pay the deposit first"* ]]'
ACCEPT_JSON=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\",\"btc_refund_address\":\"$BOB_BTC_REFUND\"}")
ACCEPT=$(field "$ACCEPT_JSON" accept)
check "bob accepted the offer" '[[ "$ACCEPT" == dinswap1a* ]]'
START_JSON=$(rpc $APORT swap.accept "{\"text\":\"$ACCEPT\"}")
check "alice started the swap" '[[ "$(field "$START_JSON" started)" == True ]]'

state() { field "$(rpc $1 swap.status "[\"$ID\"]" 2>/dev/null || echo '{}')" state; }
killed=0
for round in $(seq 1 ${LIGHT_ROUNDS:-150}); do
  rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null 2>&1 || true
  "${BCLI[@]}" generatetoaddress 1 "$BTC_MINER" >/dev/null
  sleep 1.5
  SA=$(state $APORT); SB=$(state $BPORT)
  (( round % 5 == 0 )) && echo "  [$round] alice=$SA bob=$SB"
  if [[ $killed == 0 && "$SB" == btc-locked ]]; then   # Bob's BTC is locked: Alice must find it after a restart
    echo "  [$round] SIGKILL alice's daemon, restart, unlock"
    kill -9 "$APID"; wait "$APID" 2>/dev/null || true
    # Bob dialled Alice; after her restart she dials him, so the two Dinero nodes stay connected.
    start_node alice $APORT $AP2P "${ALICECFG[@]}" --connect=127.0.0.1:$BP2P; APID=$STARTED_PID; wait_rpc $APORT
    rpc $APORT wallet.unlock '["pw-swap-light", 7200]' >/dev/null
    killed=1
  fi
  [[ "$SA" == done && "$SB" == done ]] && break
done
echo "  final: alice=$(state $APORT) bob=$(state $BPORT)"
check "alice's daemon was killed and restarted mid-swap" '[[ $killed == 1 ]]'
check "alice done" '[[ "$(state $APORT)" == done ]]'
check "bob done" '[[ "$(state $BPORT)" == done ]]'
GOT_BTC=$("${BCLI[@]}" scantxoutset start "[\"addr($ALICE_BTC)\"]" | python3 -c 'import sys,json;print(round(json.load(sys.stdin)["total_amount"]*1e8))')
check "alice received the BTC at her legacy address, minus the claim fee ($GOT_BTC sat)" '[[ $GOT_BTC == 999000 ]]'
BAL=$(rpc $BPORT wallet.getbalance '[]')
BOB_DIN=$(python3 -c 'import sys,json
d=json.loads(sys.argv[1]); t=d["total"]
print(int(t) if isinstance(t,int) and t > 10**6 else int(round(float(t)*1e8)))' "$BAL")
check "bob's wallet received the DIN ($BOB_DIN una)" '[[ $BOB_DIN -ge 999000000 && $BOB_DIN -le 1000000000 ]]'
check "alice's daemon never spoke Bitcoin RPC (no swap.btc_rpc in its log)" '! grep -q "btc_rpc=127" "$DIR/alice.log"'

rpc $APORT stop '[]' >/dev/null 2>&1 || true
stopped=0
for _ in $(seq 1 30); do kill -0 "$APID" 2>/dev/null || { stopped=1; break; }; sleep 1; done
check "alice's daemon stopped cleanly within 30 s" '[[ $stopped == 1 ]]'
[[ $stopped == 1 ]] && APID=

[[ $fail -eq 0 ]] && echo "SWAP LIGHT BTC REGTEST: PASS" || { echo "SWAP LIGHT BTC REGTEST: FAIL"; grep -h "\[Swap\]" "$DIR"/*.log | tail -8; exit 1; }
