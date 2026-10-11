#!/usr/bin/env bash
# The Qt Swap tab end to end: two regtest dinerods with swap.enable (Alice's and
# Bob's) plus bitcoind; test_swap_widget_regtest drives both tabs offscreen to
# offer, accept and start; the daemons then run the swap while blocks are mined;
# finally both tabs must show it as Done. Uses its own ports and datadirs only.
#   usage: swap_widget_regtest.sh <dinerod> <test_swap_widget_regtest>
set -euo pipefail
DINEROD=${1:?dinerod}; QTTEST=${2:?test binary}
DIR=$(mktemp -d)
BASE=$((21000 + RANDOM % 15000))
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3)); CPORT=$((BASE + 11))
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$CPORT -rpcuser=test -rpcpassword=test)
SWAPCFG=(--swap.enable=1 --swap.btc_rpc=127.0.0.1:$CPORT --swap.btc_rpc_user=test --swap.btc_rpc_pass=test
         --swap.tick_seconds=1)
cleanup() { for p in "${APID:-}" "${BPID:-}"; do [[ -n "$p" ]] && kill "$p" 2>/dev/null || true; done
            "${BCLI[@]}" stop >/dev/null 2>&1 || true; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT
rpc() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$2" "$3")
  curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$1/" |
    python3 -c 'import sys,json
d=json.load(sys.stdin); r=d.get("result")
if d.get("error") or (isinstance(r,dict) and r.get("error")): sys.stderr.write(str(d)+"\n"); sys.exit(1)
print(r if isinstance(r,str) else json.dumps(r))'
}
start_node() {
  local name=$1 port=$2 p2p=$3; shift 3
  mkdir -p "$DIR/$name"; printf 'test:test' > "$DIR/$name/.cookie"  # the tab's RpcClient reads this first
  "$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$port -port=$p2p \
    -datadir="$DIR/$name" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    --consensus-contextual-locks-height=1 "$@" >>"$DIR/$name.log" 2>&1 &
  echo $!
}
wait_rpc() { for _ in $(seq 1 90); do rpc "$1" getblockcount '[]' >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }

mkdir -p "$DIR/btc"
bitcoind -regtest -datadir="$DIR/btc" -listen=0 -rpcport=$CPORT -rpcuser=test -rpcpassword=test -daemon \
  -fallbackfee=0.0002 >/dev/null
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
"${BCLI[@]}" createwallet bob >/dev/null
BTC_MINER=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 101 "$BTC_MINER" >/dev/null
ALICE_BTC=$("${BCLI[@]}" getnewaddress "" bech32m)
BOB_BTC_REFUND=$("${BCLI[@]}" getnewaddress "" bech32m)

APID=$(start_node alice $APORT $AP2P "${SWAPCFG[@]}"); wait_rpc $APORT
BPID=$(start_node bob $BPORT $BP2P "${SWAPCFG[@]}" --connect=127.0.0.1:$AP2P); wait_rpc $BPORT
for port in $APORT $BPORT; do
  rpc $port wallet.createhd '["w"]' >/dev/null
  rpc $port wallet.encrypt '["pw-swap-qt"]' >/dev/null
  rpc $port wallet.unlock '["pw-swap-qt", 7200]' >/dev/null
done
ALICE_DIN=$(rpc $APORT wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
rpc $APORT generatetoaddress "[110, \"$ALICE_DIN\"]" >/dev/null
for _ in $(seq 1 60); do [[ "$(rpc $BPORT getblockcount '[]')" == "$(rpc $APORT getblockcount '[]')" ]] && break; sleep 1; done

export QT_QPA_PLATFORM=offscreen SWAP_QT_ALICE_PORT=$APORT SWAP_QT_ALICE_DIR="$DIR/alice" \
       SWAP_QT_BOB_PORT=$BPORT SWAP_QT_BOB_DIR="$DIR/bob" SWAP_QT_ALICE_BTC=$ALICE_BTC SWAP_QT_BOB_BTC_REFUND=$BOB_BTC_REFUND
echo "=== GUI: offer, accept, start ==="
setup=$(SWAP_QT_MODE=setup "$QTTEST" -o -,txt 2>&1 || true)
echo "$setup" | grep -E "PASS|FAIL|alice:|Totals|Loc:"
[[ "$setup" == *"Totals:"*" 0 failed"* ]] || { echo "SWAP QT REGTEST: FAIL (GUI setup)"; exit 1; }
ID=$(rpc $APORT swap.list '[]' | python3 -c 'import sys,json; l=json.load(sys.stdin); print(l[0]["id"] if l else "")')
[[ -n "$ID" ]] || { echo "SWAP QT REGTEST: FAIL (no swap was started from the GUI)"; exit 1; }

echo "=== daemons run swap $ID while blocks are mined ==="
state() { rpc $1 swap.status "[\"$ID\"]" | python3 -c 'import sys,json; print(json.load(sys.stdin)["state"])'; }
for round in $(seq 1 150); do
  rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null
  "${BCLI[@]}" generatetoaddress 1 "$BTC_MINER" >/dev/null
  sleep 1.2
  [[ "$(state $APORT)" == done && "$(state $BPORT)" == done ]] && break
done
echo "  alice=$(state $APORT) bob=$(state $BPORT)"

echo "=== GUI: both tabs show Done ==="
out=$(SWAP_QT_MODE=verify "$QTTEST" -o -,txt 2>&1 || true)
echo "$out" | grep -E "PASS|FAIL|Totals|Loc:"
[[ "$out" == *"Totals:"*" 0 failed"* ]] && echo "SWAP QT REGTEST: PASS" || { echo "SWAP QT REGTEST: FAIL"; exit 1; }
