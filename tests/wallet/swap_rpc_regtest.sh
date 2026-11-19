#!/usr/bin/env bash
# swap.* RPCs end to end: two dinerods (Alice's and Bob's, one encrypted wallet
# each, P2P-connected) and one bitcoind. The swap is driven ONLY through RPCs;
# each daemon's SwapService thread does the work. Bob's daemon is killed with
# SIGKILL mid-swap and restarted; Alice's daemon must stop cleanly; a daemon
# without swap.enable must stay inert.
#   usage: swap_rpc_regtest.sh <dinerod>
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}
ZEBRAD=${2:-}  # a dinerod built with DINERO_SWAP_ZEC: the daemons then serve Zcash too
DIR=$(mktemp -d)
BASE=$(swap_free_base 11)
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3)); XPORT=$((BASE + 4)); XP2P=$((BASE + 5))
CPORT=$((BASE + 11))  # bitcoind RPC
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$CPORT -rpcuser=test -rpcpassword=test)
CONSENSUS=(--consensus-contextual-locks-height=1)
SWAPCFG=(--swap.enable=1 --swap.btc_rpc=127.0.0.1:$CPORT --swap.btc_rpc_user=test --swap.btc_rpc_pass=test
         --swap.tick_seconds=1)
ZPORT=$((BASE + 6)); ZP2P=$((BASE + 7))
cleanup() { for p in "${APID:-}" "${BPID:-}" "${XPID:-}" "${ZPID:-}"; do [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null || true; done
            "${BCLI[@]}" stop >/dev/null 2>&1 || true; sleep 1
            [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
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
field() { python3 -c 'import sys,json; d=json.loads(sys.argv[1]); print(d.get(sys.argv[2],""))' "$1" "$2"; }

start_node() {  # <name> <rpcport> <p2pport> [extra args...]
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

mkdir -p "$DIR/alice" "$DIR/bob" "$DIR/inert" "$DIR/btc"
bitcoind -regtest -datadir="$DIR/btc" -listen=0 -rpcport=$CPORT -rpcuser=test -rpcpassword=test -daemon \
  -fallbackfee=0.0002 >/dev/null
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
swap_require_regtest bitcoind "$("${BCLI[@]}" getblockchaininfo)"
"${BCLI[@]}" createwallet bob >/dev/null
BTC_MINER=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 101 "$BTC_MINER" >/dev/null
if [[ -n "$ZEBRAD" ]]; then  # Zebra regtest beside bitcoind: the swap below runs on two-chain daemons
  mkdir -p "$DIR/zebra"
  printf '[network]\nnetwork = "Regtest"\nlisten_addr = "127.0.0.1:%s"\ncache_dir = false\n[rpc]\nlisten_addr = "127.0.0.1:%s"\nenable_cookie_auth = false\n[state]\nephemeral = true\ncache_dir = "%s"\n' \
    "$ZP2P" "$ZPORT" "$DIR/zebra" >"$DIR/zebrad.toml"
  "$ZEBRAD" -c "$DIR/zebrad.toml" start >"$DIR/zebrad.log" 2>&1 & ZPID=$!
  for _ in $(seq 1 60); do curl -s -m 5 -H 'content-type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":[0]}' \
    "http://127.0.0.1:$ZPORT/" | grep -q 029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327 && break; sleep 0.5; done
  SWAPCFG+=(--swap.zec_rpc=127.0.0.1:$ZPORT)
  echo "  both chains: bitcoind on $CPORT, Zebra on $ZPORT"
fi
ALICE_BTC=$("${BCLI[@]}" getnewaddress "" bech32m)   # Alice's payout address (only its balance matters)
BOB_BTC_REFUND=$("${BCLI[@]}" getnewaddress "" bech32m)

# --- A daemon without swap.enable is inert --------------------------------------
start_node inert $XPORT $XP2P; XPID=$STARTED_PID; wait_rpc $XPORT
out=$(rpc $XPORT swap.list '[]' 2>&1 || true)
check "without swap.enable, swap.* reports disabled" '[[ "$out" == *disabled* ]]'
check "without swap.enable, no swap directory is created" '[[ ! -e "$DIR/inert/swaps" ]]'
kill "$XPID"; wait "$XPID" 2>/dev/null || true; XPID=

# --- A refused swap setting stops the node, unless swap.optional=1 (dinero-qt's switch) ---
mkdir -p "$DIR/refused" "$DIR/optional"
start_node refused $XPORT $XP2P --swap.enable=1 --swap.btc_backend=nonsense; XPID=$STARTED_PID
stopped=0; for _ in $(seq 1 120); do kill -0 "$XPID" 2>/dev/null || { stopped=1; break; }; sleep 0.5; done
check "a refused swap setting stops the node" '[[ $stopped == 1 ]] && grep -q "swap.btc_backend must be core or light" "$DIR/refused.log"'
kill -9 "$XPID" 2>/dev/null || true; wait "$XPID" 2>/dev/null || true; XPID=
start_node optional $XPORT $XP2P --swap.enable=1 --swap.btc_backend=nonsense --swap.optional=1; XPID=$STARTED_PID
check "with swap.optional=1 the node starts all the same" 'wait_rpc $XPORT'
out=$(rpc $XPORT swap.list '[]' 2>&1 || true)
check "and swap.* say why swaps are off" '[[ "$out" == *"swaps are disabled: swap.btc_backend must be core or light"* ]]'
check "and no swap directory is created" '[[ ! -e "$DIR/optional/swaps" ]]'
kill "$XPID"; wait "$XPID" 2>/dev/null || true; XPID=
mkdir -p "$DIR/rehearsal"
start_node rehearsal $XPORT $XP2P "${SWAPCFG[@]}" --swap.optional=1 --swap.btc_rehearsal_chain=main; XPID=$STARTED_PID
wait_rpc $XPORT
out=$(rpc $XPORT swap.list '[]' 2>&1 || true)
check "a rehearsal may only pair regtest with Bitcoin testnet4" '[[ "$out" == *"swap.btc_rehearsal_chain is for a regtest node only, and only testnet4"* ]]'
kill "$XPID"; wait "$XPID" 2>/dev/null || true; XPID=
if [[ $(strings "$DINEROD" | grep -c "swap.tower_url: ") -gt 0 ]]; then  # a dinerod with the HTTPS client
  mkdir -p "$DIR/towerurl"
  start_node towerurl $XPORT $XP2P "${SWAPCFG[@]}" --swap.optional=1 --swap.tower_url=http://tower.example; XPID=$STARTED_PID
  wait_rpc $XPORT
  out=$(rpc $XPORT swap.list '[]' 2>&1 || true)
  check "a watchtower address on another machine must be https (${out:0:110})" '[[ "$out" == *"swap.tower_url: a watchtower on another machine needs https://"* ]]'
  kill "$XPID"; wait "$XPID" 2>/dev/null || true; XPID=
fi

# --- Alice's and Bob's daemons ---------------------------------------------------
# Establish each intended payer before SwapService captures its durable owner.
start_node alice $APORT $AP2P; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.createhd '["w"]' >/dev/null
  rpc $port wallet.encrypt '["pw-swap-rpc"]' >/dev/null
done
kill "$APID" "$BPID"; wait "$APID"; wait "$BPID"; APID=; BPID=
start_node alice $APORT $AP2P "${SWAPCFG[@]}"; APID=$STARTED_PID; wait_rpc $APORT
start_node bob $BPORT $BP2P "${SWAPCFG[@]}" --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.unlock '["pw-swap-rpc", 7200]' >/dev/null
done
ALICE_DIN=$(rpc $APORT wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
rpc $APORT generatetoaddress "[110, \"$ALICE_DIN\"]" >/dev/null
for _ in $(seq 1 60); do [[ "$(rpc $BPORT getblockcount '[]')" == "$(rpc $APORT getblockcount '[]')" ]] && break; sleep 1; done
if [[ -n "$ZEBRAD" ]]; then
  CAPS=$(rpc $APORT swap.capabilities "[]")
  check "swap.capabilities lists btc and zec ($CAPS)" '[[ "$CAPS" == *btc* && "$CAPS" == *zec* ]]'
fi
check "bob's daemon follows alice's chain" '[[ "$(rpc $BPORT getblockcount "[]")" == "$(rpc $APORT getblockcount "[]")" ]]'

# --- Offer / accept through RPCs only ----------------------------------------------
out=$(rpc $APORT swap.offer "{\"din_amount_una\":2000000000000000,\"btc_amount_sat\":1000000,\"btc_address\":\"$ALICE_BTC\",\"din_funding_fee_rate_hint\":2,\"din_funding_maximum_fee_una\":5000000}" 2>&1 || true)
check "an offer beyond alice's spendable DIN is refused (${out:0:60})" '[[ "$out" == *"exceeds available balance"* ]]'
OFFER_JSON=$(rpc $APORT swap.offer "{\"din_amount_una\":1000000000,\"btc_amount_sat\":1000000,\"btc_address\":\"$ALICE_BTC\",\"din_funding_fee_rate_hint\":2,\"din_funding_maximum_fee_una\":5000000}")
OFFER=$(field "$OFFER_JSON" offer); ID=$(field "$OFFER_JSON" id)
echo "  offer $ID: ${OFFER:0:40}..."
DECODED=$(rpc $BPORT swap.decode "{\"text\":\"$OFFER\"}")
check "swap.decode shows bob the terms before accepting" \
  '[[ "$(field "$DECODED" kind)" == offer && "$(field "$DECODED" id)" == "$ID" && "$(field "$DECODED" btc_amount_sat)" == 1000000 && "$(field "$DECODED" acceptable_now)" == True ]]'
check "swap.decode refuses garbage" '! rpc $BPORT swap.decode "{\"text\":\"dinswap1o00\"}" 2>/dev/null'
ACCEPT_JSON=$(rpc $BPORT swap.accept "{\"text\":\"$OFFER\",\"btc_refund_address\":\"$BOB_BTC_REFUND\"}")
ACCEPT=$(field "$ACCEPT_JSON" accept)
check "bob accepted the offer" '[[ "$ACCEPT" == dinswap1a* ]]'
START_JSON=$(rpc $APORT swap.accept "{\"text\":\"$ACCEPT\"}")
check "alice started the swap" '[[ "$(field "$START_JSON" started)" == True ]]'
check "status never shows a secret or key field" \
  '! rpc $APORT swap.status "[\"$ID\"]" | grep -qiE "secret|preimage|priv"'

state() { field "$(rpc $1 swap.status "[\"$ID\"]" 2>/dev/null || echo '{}')" state; }
killed=0
early_refund=0
for round in $(seq 1 150); do
  rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null
  "${BCLI[@]}" generatetoaddress 1 "$BTC_MINER" >/dev/null
  sleep 1.2
  SA=$(state $APORT); SB=$(state $BPORT)
  if [[ $early_refund == 0 && "$SA" == din-locked ]]; then
    out=$(rpc $APORT swap.refund "[\"$ID\"]" 2>&1 || true)
    check "swap.refund before T_din is refused by consensus (${out:0:70})" '[[ "$out" == *non-final* ]]'
    early_refund=1
  fi
  (( round % 5 == 0 )) && echo "  [$round] alice=$SA bob=$SB"
  if [[ $killed == 0 && ( "$SB" == btc-lock-broadcast || "$SB" == btc-locked ) ]]; then  # Bob's BTC is committed
    echo "  [$round] SIGKILL bob's daemon, restart, unlock"
    kill -9 "$BPID"; wait "$BPID" 2>/dev/null || true
    start_node bob $BPORT $BP2P "${SWAPCFG[@]}" --connect=127.0.0.1:$AP2P; BPID=$STARTED_PID; wait_rpc $BPORT
    check "after restart, bob's swap is paused while the wallet is locked" \
      '[[ "$(state $BPORT)" == "paused: wallet locked" ]]'
    rpc $BPORT wallet.unlock '["pw-swap-rpc", 7200]' >/dev/null
    killed=1
  fi
  [[ "$SA" == done && "$SB" == done ]] && break
done
echo "  final: alice=$(state $APORT) bob=$(state $BPORT)"
check "bob's daemon was killed and restarted mid-swap" '[[ $killed == 1 ]]'
check "the early manual refund was tried" '[[ $early_refund == 1 ]]'
out=$(rpc $BPORT swap.refund "[\"$ID\"]" 2>&1 || true)
check "swap.refund after the swap is refused (${out:0:60})" '[[ "$out" == *"already spent"* ]]'
check "alice done" '[[ "$(state $APORT)" == done ]]'
check "bob done" '[[ "$(state $BPORT)" == done ]]'
GOT_BTC=$("${BCLI[@]}" scantxoutset start "[\"addr($ALICE_BTC)\"]" | python3 -c 'import sys,json;print(round(json.load(sys.stdin)["total_amount"]*1e8))')
check "alice received BTC minus fee on chain ($GOT_BTC sat)" '[[ $GOT_BTC == 999000 ]]'
BAL=$(rpc $BPORT wallet.getbalance '[]'); echo "  bob wallet.getbalance: $BAL"
# "total" is in DIN (a decimal) or una (an integer); normalise to una.
BOB_DIN=$(python3 -c 'import sys,json
d=json.loads(sys.argv[1]); t=d["total"]
print(int(t) if isinstance(t,int) and t > 10**6 else int(round(float(t)*1e8)))' "$BAL")
check "bob's wallet received the DIN ($BOB_DIN una)" '[[ $BOB_DIN -ge 999000000 && $BOB_DIN -le 1000000000 ]]'

# --- Clean shutdown must not hang on the swap thread ---------------------------------
rpc $APORT stop '[]' >/dev/null 2>&1 || true
stopped=0
for _ in $(seq 1 30); do kill -0 "$APID" 2>/dev/null || { stopped=1; break; }; sleep 1; done
check "alice's daemon stopped cleanly within 30 s" '[[ $stopped == 1 ]]'
[[ $stopped == 1 ]] && APID=

[[ $fail -eq 0 ]] && echo "SWAP RPC REGTEST: PASS" || { echo "SWAP RPC REGTEST: FAIL"; grep -h "\[Swap\]" "$DIR"/*.log | tail -5; exit 1; }
