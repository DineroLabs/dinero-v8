#!/usr/bin/env bash
# swap.* RPCs for DIN <-> ZEC end to end: two dinerods built with
# DINERO_SWAP_ZEC (Alice's and Bob's, encrypted wallets, P2P-connected, no
# bitcoind) and one Zebra regtest. Driven ONLY through RPCs; each daemon's
# SwapService does the work. Bob's deposit is short, then covered; his daemon
# is restarted while the deposit is pending; a second deposit is cancelled and
# swept back. Where bitcoind is installed, both daemons also get swap.btc_rpc,
# so the Zcash swap runs on a node that serves both chains.
#   usage: swap_zec_rpc_regtest.sh <dinerod (DINERO_SWAP_ZEC)> <swap_zec_regtest_tool> <zebrad>
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}; TOOL=${2:?swap_zec_regtest_tool}; ZEBRAD=${3:?zebrad}
MINE="$(cd "$(dirname "$0")" && pwd)/zebra_regtest_mine.py"
REGTEST_GENESIS=029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327
DIR=$(mktemp -d)
BASE=$(swap_free_base 8)
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3)); ZPORT=$((BASE + 5)); ZP2P=$((BASE + 6))
CONSENSUS=(--consensus-contextual-locks-height=1)
SWAPCFG=(--swap.enable=1 --swap.zec_rpc=127.0.0.1:$ZPORT --swap.tick_seconds=1)
CPORT=$((BASE + 7)); BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$CPORT -rpcuser=test -rpcpassword=test)
DUAL=0; command -v bitcoind >/dev/null && DUAL=1
cleanup() { for p in "${APID:-}" "${BPID:-}" "${ZPID:-}"; do [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null || true; done
            [[ $DUAL == 1 ]] && { "${BCLI[@]}" stop >/dev/null 2>&1 || true; }
            sleep 1; [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT

rpc() {  # <port> <method> <params-json> -> result (prints; exit 1 on JSON-RPC or in-band error)
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$2" "$3")
  local resp; resp=$(curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$1/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error"); r=d.get("result")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
if isinstance(r,dict) and r.get("error"): sys.stderr.write("RPC in-band error %s: %s\n"%(sys.argv[2],r["error"])); sys.exit(1)
print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$2"
}
zcall() { curl -s --max-time 60 -H 'content-type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" "http://127.0.0.1:$ZPORT" |
  python3 -c 'import sys,json; r=json.load(sys.stdin).get("result"); print(r if isinstance(r,str) else json.dumps(r))'; }
field() { python3 -c 'import sys,json; d=json.loads(sys.argv[1]); print(d.get(sys.argv[2],""))' "$1" "$2"; }
zec_balance() {  # <t-address> -> zatoshi with >= 1 confirmation
  zcall getaddressutxos "[{\"addresses\":[\"$1\"]}]" | python3 -c 'import sys,json; print(sum(u["satoshis"] for u in json.load(sys.stdin)))'
}
mine_zec() { python3 "$MINE" "$ZPORT" "${1:-1}" >/dev/null; }
pay() {  # <address> <zat>: the Zebra miner pays; the payment must be accepted
  local out; out=$("$TOOL" pay $ZPORT 7 "$1" "$2"); echo "  pay $2 zat to ${1:0:12}...: ${out:0:80}"
  [[ "$out" == accepted* ]] || { echo "  FAIL payment refused"; exit 1; }
}
deep_enough() {  # <address> <zat> <confirmations>: mine until that much has that many confirmations
  for _ in $(seq 1 30); do
    local tip; tip=$(zcall getblockcount '[]')
    local got; got=$(zcall getaddressutxos "[{\"addresses\":[\"$1\"]}]" |
      python3 -c "import sys,json; print(sum(u['satoshis'] for u in json.load(sys.stdin) if $tip - u['height'] + 1 >= $3))")
    (( got >= $2 )) && return 0
    mine_zec 1
  done
  return 1
}
start_node() {  # <name> <rpcport> <p2pport> [extra args...]
  local name=$1 port=$2 p2p=$3; shift 3
  "$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$port -port=$p2p \
    -datadir="$DIR/$name" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 "${CONSENSUS[@]}" "$@" \
    >>"$DIR/$name.log" 2>&1 &
  echo $!
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

# --- Zebra regtest, its miner paying Bob's deposits --------------------------------
mkdir -p "$DIR/alice" "$DIR/bob"
cat >"$DIR/zebrad.toml" <<TOML
[network]
network = "Regtest"
listen_addr = "127.0.0.1:$ZP2P"
cache_dir = false
[network.testnet_parameters.activation_heights]
NU5 = 1
NU6 = 1
"NU6.1" = 1
[mining]
miner_address = "$("$TOOL" addr 7)"
[rpc]
listen_addr = "127.0.0.1:$ZPORT"
enable_cookie_auth = false
[state]
ephemeral = true
cache_dir = "$DIR/zebra"
[tracing]
use_color = false
TOML
"$ZEBRAD" -c "$DIR/zebrad.toml" start >"$DIR/zebrad.log" 2>&1 & ZPID=$!
for _ in $(seq 1 60); do zcall getblockcount '[]' >/dev/null 2>&1 && break; sleep 0.5; done
[[ "$(zcall getblockhash '[0]')" == "$REGTEST_GENESIS" ]] || { echo "ABORT: not a fresh Zebra regtest on $ZPORT"; exit 1; }
mine_zec 110
ALICE_ZEC=$("$TOOL" addr 21); BOB_ZEC=$("$TOOL" addr 22)

# --- bitcoind too, where installed: each daemon then serves both chains ------------
if [[ $DUAL == 1 ]]; then
  mkdir -p "$DIR/btc"
  bitcoind -regtest -datadir="$DIR/btc" -listen=0 -rpcport=$CPORT -rpcuser=test -rpcpassword=test -daemon >/dev/null
  for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
  swap_require_regtest bitcoind "$("${BCLI[@]}" getblockchaininfo)"
  SWAPCFG+=(--swap.btc_rpc=127.0.0.1:$CPORT --swap.btc_rpc_user=test --swap.btc_rpc_pass=test)
  echo "  both chains: bitcoind on $CPORT, Zebra on $ZPORT"
fi

# --- Alice's and Bob's daemons -----------------------------------------------------
# Establish each intended payer before SwapService captures its durable owner.
APID=$(start_node alice $APORT $AP2P); wait_rpc $APORT
BPID=$(start_node bob $BPORT $BP2P --connect=127.0.0.1:$AP2P); wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.createhd '["w"]' >/dev/null
  rpc $port wallet.encrypt '["pw-swap-zec"]' >/dev/null
done
kill "$APID" "$BPID"; while kill -0 "$APID" 2>/dev/null || kill -0 "$BPID" 2>/dev/null; do sleep 0.2; done; APID=; BPID=
APID=$(start_node alice $APORT $AP2P "${SWAPCFG[@]}"); wait_rpc $APORT
BPID=$(start_node bob $BPORT $BP2P "${SWAPCFG[@]}" --connect=127.0.0.1:$AP2P); wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.unlock '["pw-swap-zec", 7200]' >/dev/null
done
CAPS=$(rpc $APORT swap.capabilities "[]")
check "swap.capabilities lists zec" '[[ "$CAPS" == *zec* ]]'
[[ $DUAL == 1 ]] && check "swap.capabilities lists btc too ($CAPS)" '[[ "$CAPS" == *btc* ]]'
ALICE_DIN=$(rpc $APORT wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
rpc $APORT generatetoaddress "[110, \"$ALICE_DIN\"]" >/dev/null
for _ in $(seq 1 60); do [[ "$(rpc $BPORT getblockcount '[]')" == "$(rpc $APORT getblockcount '[]')" ]] && break; sleep 1; done

# --- Offer, deposit, accept through RPCs only --------------------------------------
offer() { rpc $APORT swap.offer "{\"chain\":\"zec\",\"din_amount_una\":1000000000,\"zec_amount_zat\":2500000,\"zec_address\":\"$ALICE_ZEC\",\"din_funding_fee_rate_hint\":2,\"din_funding_maximum_fee_una\":5000000}"; }
OFFER_JSON=$(offer); OFFER=$(field "$OFFER_JSON" offer); ID=$(field "$OFFER_JSON" id)
check "the offer is a dinswap2o" '[[ "$OFFER" == dinswap2o* ]]'
DECODED=$(rpc $BPORT swap.decode "{\"text\":\"$OFFER\"}")
check "swap.decode shows a zec offer" '[[ "$(field "$DECODED" chain)" == zec && "$(field "$DECODED" zec_amount_zat)" == 2500000 ]]'
out=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}" 2>&1 || true)
check "accepting before any deposit is refused (${out:0:50})" '[[ "$out" == *"pay the deposit first"* ]]'
DEP=$(rpc $BPORT swap.deposit "{\"text\":\"$OFFER\",\"zec_refund_address\":\"$BOB_ZEC\"}")
DEPOSIT=$(field "$DEP" deposit_address)
check "swap.deposit gives a t-address and the amount due" \
  '[[ "$DEPOSIT" == tm* && "$(field "$DEP" deposit_due_zat)" == 2520000 ]]'
pay "$DEPOSIT" 1000000; deep_enough "$DEPOSIT" 1000000 10
out=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}" 2>&1 || true)
check "a short deposit is refused (${out:0:60})" '[[ "$out" == *"deposit is short by 0.0152 ZEC"* ]]'
pay "$DEPOSIT" 1520000; mine_zec 3
status() { rpc $1 swap.status "[\"$2\"]" 2>/dev/null || echo '{}'; }
check "bob's list shows the deposit awaiting confirmations" '[[ "$(field "$(status $BPORT $ID)" state)" == awaiting-deposit ]]'
echo "  restart bob's daemon while the deposit is pending"
kill -9 "$BPID"; wait "$BPID" 2>/dev/null || true
BPID=$(start_node bob $BPORT $BP2P "${SWAPCFG[@]}" --connect=127.0.0.1:$AP2P); wait_rpc $BPORT
rpc $BPORT wallet.unlock '["pw-swap-zec", 7200]' >/dev/null
check "after the restart the same deposit address is kept" '[[ "$(field "$(status $BPORT $ID)" deposit_address)" == "$DEPOSIT" ]]'
deep_enough "$DEPOSIT" 2520000 10 || echo "  deposit never reached 10 confirmations"
for _ in $(seq 1 5); do [[ "$(field "$(status $BPORT $ID)" state)" == deposit-confirmed ]] && break; sleep 1; done
check "the deposit is confirmed" '[[ "$(field "$(status $BPORT $ID)" state)" == deposit-confirmed ]]'
ACCEPT=$(field "$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\"}")" accept)
check "bob accepted from his deposit" '[[ "$ACCEPT" == dinswap1a* ]]'
check "alice started the swap" '[[ "$(field "$(rpc $APORT swap.accept "{\"text\":\"$ACCEPT\"}")" started)" == True ]]'

state() { field "$(status $1 $ID)" state; }
for round in $(seq 1 160); do
  rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null
  mine_zec 1
  sleep 1.2
  SA=$(state $APORT); SB=$(state $BPORT)
  (( round % 10 == 0 )) && echo "  [$round] alice=$SA bob=$SB"
  [[ "$SA" == done && "$SB" == done ]] && break
done
echo "  final: alice=$(state $APORT) bob=$(state $BPORT)"
check "alice done" '[[ "$(state $APORT)" == done ]]'
check "bob done" '[[ "$(state $BPORT)" == done ]]'
GOT=$(zec_balance "$ALICE_ZEC")
check "alice received ZEC minus the claim fee on chain ($GOT zat)" '[[ $GOT == 2480000 ]]'
check "the deposit was swept into the lock" '[[ $(zec_balance "$DEPOSIT") == 0 ]]'

# --- Cancel: a paid deposit goes back to Bob ---------------------------------------
OFFER2=$(field "$(offer)" offer)
DEP2=$(rpc $BPORT swap.deposit "{\"text\":\"$OFFER2\",\"zec_refund_address\":\"$BOB_ZEC\"}")
ID2=$(field "$DEP2" id); DEPOSIT2=$(field "$DEP2" deposit_address)
pay "$DEPOSIT2" 2520000; deep_enough "$DEPOSIT2" 2520000 1
rpc $BPORT swap.cancel "[\"$ID2\"]" >/dev/null
deep_enough "$BOB_ZEC" 2510000 1 || true
check "cancel swept the deposit back to bob ($(zec_balance "$BOB_ZEC") zat)" '[[ $(zec_balance "$BOB_ZEC") == 2510000 ]]'
check "the cancelled deposit is gone from bob's list" '! rpc $BPORT swap.status "[\"$ID2\"]" >/dev/null 2>&1'

[[ $fail -eq 0 ]] && echo "ZEC SWAP RPC: PASS" || { echo "ZEC SWAP RPC: FAIL"; tail -20 "$DIR/bob.log"; exit 1; }
