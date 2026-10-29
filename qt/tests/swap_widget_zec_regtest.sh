#!/usr/bin/env bash
# The Qt Swap tab for DIN <-> ZEC end to end: two regtest dinerods built with
# DINERO_SWAP_ZEC (Alice's and Bob's) plus a Zebra regtest. test_swap_widget_regtest
# drives both tabs offscreen: Alice offers ZEC, Bob gets the deposit panel; the
# Zebra miner pays the deposit; a fresh tab restores Bob's panel, accepts once
# confirmed, and Alice starts; the daemons run the swap while both chains are
# mined; finally both tabs must show it as Done. Own ports and datadirs only.
#   usage: swap_widget_zec_regtest.sh <dinerod> <test_swap_widget_regtest> <swap_zec_regtest_tool> <zebrad>
set -euo pipefail
DINEROD=${1:?dinerod}; QTTEST=${2:?test binary}; TOOL=${3:?swap_zec_regtest_tool}; ZEBRAD=${4:?zebrad}
MINE="$(cd "$(dirname "$0")/../../tests/wallet" && pwd)/zebra_regtest_mine.py"
# A dinerod built without DINERO_SWAP_ZEC has no Zcash code: skipped (77), not passed.
nm "$DINEROD" 2>/dev/null | grep -q dzs_tx_build || { echo "SKIP: $DINEROD is built without DINERO_SWAP_ZEC"; exit 77; }
REGTEST_GENESIS=029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327
DIR=$(mktemp -d)
BASE=$((30000 + RANDOM % 8000))
APORT=$BASE; AP2P=$((BASE + 1)); BPORT=$((BASE + 2)); BP2P=$((BASE + 3)); ZPORT=$((BASE + 5)); ZP2P=$((BASE + 6))
SWAPCFG=(--swap.enable=1 --swap.zec_rpc=127.0.0.1:$ZPORT --swap.tick_seconds=1)
cleanup() { for p in "${APID:-}" "${BPID:-}" "${ZPID:-}"; do [[ -n "$p" ]] && kill "$p" 2>/dev/null || true; done
            sleep 1; [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT
rpc() {
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$2" "$3")
  curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$1/" |
    python3 -c 'import sys,json
d=json.load(sys.stdin); r=d.get("result")
if d.get("error") or (isinstance(r,dict) and r.get("error")): sys.stderr.write(str(d)+"\n"); sys.exit(1)
print(r if isinstance(r,str) else json.dumps(r))'
}
zcall() { curl -s --max-time 60 -H 'content-type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" "http://127.0.0.1:$ZPORT" |
  python3 -c 'import sys,json; r=json.load(sys.stdin).get("result"); print(r if isinstance(r,str) else json.dumps(r))'; }
mine_zec() { python3 "$MINE" "$ZPORT" "${1:-1}" >/dev/null; }
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
start_node() {
  local name=$1 port=$2 p2p=$3; shift 3
  mkdir -p "$DIR/$name"; printf 'test:test' > "$DIR/$name/.cookie"  # the tab's RpcClient reads this first
  "$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$port -port=$p2p \
    -datadir="$DIR/$name" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    --consensus-contextual-locks-height=1 "$@" >>"$DIR/$name.log" 2>&1 &
  echo $!
}
wait_rpc() { for _ in $(seq 1 90); do rpc "$1" getblockcount '[]' >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
gui() {  # <mode>: run the tabs; exit unless every check passed
  local out; out=$(SWAP_QT_MODE=$1 "$QTTEST" -o -,txt 2>&1 || true)
  echo "$out" | grep -E "PASS|FAIL|alice:|Totals|Loc:" >&2
  [[ "$out" == *"Totals:"*" 0 failed"* ]] || { echo "SWAP QT ZEC REGTEST: FAIL ($1)" >&2; exit 1; }
  echo "$out"
}

# --- Zebra regtest, its miner paying Bob's deposit ------------------------------
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

APID=$(start_node alice $APORT $AP2P "${SWAPCFG[@]}"); wait_rpc $APORT
BPID=$(start_node bob $BPORT $BP2P "${SWAPCFG[@]}" --connect=127.0.0.1:$AP2P); wait_rpc $BPORT
for port in $APORT $BPORT; do
  [[ "$(rpc $port getblockchaininfo '[]')" == *regtest* ]] || { echo "ABORT: dinerod :$port is not regtest"; exit 1; }
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
       SWAP_QT_BOB_PORT=$BPORT SWAP_QT_BOB_DIR="$DIR/bob" SWAP_QT_ALICE_ZEC=$ALICE_ZEC SWAP_QT_BOB_ZEC=$BOB_ZEC
echo "=== GUI: Alice offers ZEC, Bob gets the deposit panel ==="
out=$(gui zec-offer)
DEPOSIT=$(echo "$out" | sed -n 's/.*deposit: \(tm[A-Za-z0-9]*\).*/\1/p' | head -1)
[[ -n "$DEPOSIT" ]] || { echo "SWAP QT ZEC REGTEST: FAIL (no deposit address from the GUI)"; exit 1; }

echo "=== the Zebra miner pays $DEPOSIT; 10 confirmations ==="
paid=$("$TOOL" pay $ZPORT 7 "$DEPOSIT" 2520000); echo "  ${paid:0:80}"
[[ "$paid" == accepted* ]] || { echo "SWAP QT ZEC REGTEST: FAIL (payment refused)"; exit 1; }
deep_enough "$DEPOSIT" 2520000 10 || { echo "SWAP QT ZEC REGTEST: FAIL (deposit never confirmed)"; exit 1; }

echo "=== GUI: a fresh tab restores the panel; Bob accepts, Alice starts ==="
SWAP_QT_DEPOSIT=$DEPOSIT gui zec-accept >/dev/null
ID=$(rpc $APORT swap.list '[]' | python3 -c 'import sys,json; l=json.load(sys.stdin); print(l[0]["id"] if l else "")')
[[ -n "$ID" ]] || { echo "SWAP QT ZEC REGTEST: FAIL (no swap was started from the GUI)"; exit 1; }

echo "=== daemons run swap $ID while both chains are mined ==="
state() { rpc $1 swap.status "[\"$ID\"]" | python3 -c 'import sys,json; print(json.load(sys.stdin)["state"])'; }
for round in $(seq 1 160); do
  rpc $APORT generatetoaddress "[1, \"$ALICE_DIN\"]" >/dev/null
  mine_zec 1
  sleep 1.2
  [[ "$(state $APORT)" == done && "$(state $BPORT)" == done ]] && break
done
echo "  alice=$(state $APORT) bob=$(state $BPORT)"

echo "=== GUI: both tabs show Done ==="
gui verify >/dev/null
echo "SWAP QT ZEC REGTEST: PASS"
