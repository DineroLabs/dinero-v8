#!/usr/bin/env bash
# Swaps without Bitcoin Core start on mainnet.
# One dinerod on Dinero MAINNET, in an empty temporary folder and with no
# network at all (no Dinero peers; Bitcoin servers that do not exist), is
# started the way dinero-qt starts it with swaps switched on. Nothing is
# sent and no wallet exists: this checks only that the node starts swaps.
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

echo "$CHECKS checks, $FAILS failed"
[[ $FAILS == 0 && $CHECKS == 5 ]]
