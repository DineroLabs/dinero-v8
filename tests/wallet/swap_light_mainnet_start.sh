#!/usr/bin/env bash
# Swaps without Bitcoin Core start on mainnet.
# One dinerod on Dinero MAINNET, in an empty temporary folder and with no
# network at all (no Dinero peers; Bitcoin servers that do not exist), is
# started the way dinero-qt starts it with swaps switched on. Nothing is
# sent and no wallet exists: this checks only that the node starts swaps,
# and that the Bitcoin wallet is offered there, with or without swaps.
# Until v8.2.4 it refused them here ("not offered on mainnet yet").
#   usage: swap_light_mainnet_start.sh <dinerod>
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}
DIR=$(mktemp -d)
BASE=$(swap_free_base 3)
PORT=$BASE; P2P=$((BASE + 1))
PID=
cleanup() { [[ -n "$PID" ]] && kill -9 "$PID" 2>/dev/null || true
            [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT
FAILS=0; CHECKS=0
check() { CHECKS=$((CHECKS + 1)); if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }
call() { curl -s --max-time 30 -u test:test -H 'Content-Type: application/json' \
           -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":[]}" "http://127.0.0.1:$PORT/" | tr -d '\n\t'; }

mkdir -p "$DIR/node"
"$DINEROD" -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$PORT -port=$P2P -datadir="$DIR/node" \
  -connect=0 -listen=0 -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
  --swap.enable=1 --swap.btc_backend=light --swap.mainnet_beta=1 \
  --swap.btc_servers=http://127.0.0.1:$((BASE + 2)),http://127.0.0.1:$((BASE + 3)) --swap.btc_peers=none \
  >"$DIR/node.log" 2>&1 &
PID=$!
up=0; for _ in $(seq 1 180); do
  kill -0 "$PID" 2>/dev/null || break
  [[ "$(call getblockchaininfo 2>/dev/null)" == *'"chain"'* ]] && { up=1; break; }
  sleep 1
done
check "the node starts (swaps are not optional here: a refusal would stop it)" '[[ $up == 1 ]]'
info=$(call getblockchaininfo || true)
check "it is a mainnet node" '[[ "$info" == *"\"chain\" : \"mainnet\""* ]]'
check "swaps without Bitcoin Core are enabled" 'grep -q "^\[Swap\] enabled:" "$DIR/node.log" && grep -q "Bitcoin without Bitcoin Core" "$DIR/node.log"'
check "no mainnet refusal" '! grep -q "not offered on mainnet" "$DIR/node.log"'
list=$(call swap.list || true)
check "swap.list answers with an empty list, not \"disabled\"" '[[ "$list" == *"\"result\" : []"* && "$list" != *disabled* ]]'

# --- The Bitcoin wallet on mainnet: started with the node, or by one request in a running node ---
kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; PID=
BTCNET=(--swap.btc_servers=http://127.0.0.1:$((BASE + 2)),http://127.0.0.1:$((BASE + 3)) --swap.btc_peers=none)
start_plain() {  # <name> [extra args...]
  local name=$1; shift
  mkdir -p "$DIR/$name"
  "$DINEROD" -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$PORT -port=$P2P -datadir="$DIR/$name" \
    -connect=0 -listen=0 -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 "${BTCNET[@]}" "$@" >"$DIR/$name.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 180); do
    kill -0 "$PID" 2>/dev/null || return 1
    [[ "$(call getblockchaininfo 2>/dev/null)" == *'"chain" : "mainnet"'* ]] && return 0
    sleep 1
  done
  return 1
}
check "a mainnet node starts with only the Bitcoin wallet switched on" 'start_plain wallet --swap.btc_wallet=1'
st=$(call btc.status || true)
check "the Bitcoin wallet is offered on mainnet, and can send" '[[ "$st" == *"\"offered\" : true"* && "$st" == *"\"can_send\" : true"* ]]'
check "swaps stay off beside it" '[[ "$(call swap.list || true)" == *"swaps are disabled"* ]]'
kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; PID=
check "a mainnet node starts with nothing switched on" 'start_plain plain'
check "there the Bitcoin wallet is not running" '[[ "$(call btc.status || true)" == *"not running on this node"* ]]'
st=$(call btc.start || true)
check "btc.start starts it in the running mainnet node" '[[ "$st" == *"\"offered\" : true"* ]]'
check "and it stays offered" '[[ "$(call btc.status || true)" == *"\"offered\" : true"* ]]'

# --- A Bitcoin server that has gone quiet does not hold up stopping the node ---
kill -9 "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; PID=
# It takes the connection and never answers: the node's requests to it hang until their time limit (30 s each).
python3 -c 'import socket,sys,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(50)
held=[]
while True: held.append(s.accept()[0])' $((BASE + 2)) & QUIET=$!
trap 'kill -9 $QUIET 2>/dev/null || true; cleanup' EXIT
BTCNET=(--swap.btc_servers=http://127.0.0.1:$((BASE + 2)) --swap.btc_peers=none --swap.btc_min_servers=1)
check "a mainnet node starts with a Bitcoin server that never answers" 'start_plain quiet --swap.btc_wallet=1'
for _ in $(seq 1 30); do grep -q "the Bitcoin wallet is available" "$DIR/quiet.log" && break; sleep 1; done
sleep 5  # the swap thread is now waiting for that server's answer
asked=$(date +%s); kill "$PID"
for _ in $(seq 1 60); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
took=$(( $(date +%s) - asked ))
check "asked to stop, it stops without waiting for that server (${took}s)" '! kill -0 "$PID" 2>/dev/null && (( took <= 20 ))'
check "and stops cleanly, not by the watchdog" 'grep -q "Clean shutdown complete" "$DIR/quiet.log" && ! grep -q "WATCHDOG" "$DIR/quiet.log"'
PID=; kill -9 $QUIET 2>/dev/null || true

echo "$CHECKS checks, $FAILS failed"
[[ $FAILS == 0 && $CHECKS == 15 ]]
