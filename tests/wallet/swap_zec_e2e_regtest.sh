#!/usr/bin/env bash
# Two-chain end-to-end DIN <-> ZEC swap: one dinerod and one Zebra on regtest,
# Alice and Bob each driving their own SwapRunner (tests/wallet/swap_zec_e2e_tool.cpp).
# dinerod enforces transaction locks from height 1. Zebra's blocks are stamped
# with the wall clock (tests/wallet/zebra_regtest_mine.py), so Zcash time
# follows real time. Zebra has no mock time and refuses blocks over 2 h ahead
# of its clock, so to mine Bob's refund after T_zec (>= 12 h away by the offer
# rules) zebrad runs under libfaketime with a clock file the tool moves
# forward. Without libfaketime those steps are skipped (and say so).
# A chain's time cannot go back, so tower-refund gets its own fresh Zebra.
#   usage: swap_zec_e2e_regtest.sh <dinerod> <swap_zec_e2e_tool> <zebrad>
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/swap_regtest_lib.sh"
DINEROD=${1:?dinerod}; TOOL=${2:?swap_zec_e2e_tool}; ZEBRAD=${3:?zebrad}
HERE=$(cd "$(dirname "$0")" && pwd); MINE="$HERE/zebra_regtest_mine.py"
REGTEST_GENESIS=029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327
DIR=$(mktemp -d)
DPORT=$(swap_free_base 16); DP2P=$((DPORT + 3)); ZPORT=$((DPORT + 6)); ZP2P=$((DPORT + 9))
FAKETIME_LIB=${FAKETIME_LIB:-}
for f in /opt/homebrew/lib/faketime/libfaketime.1.dylib /usr/local/lib/faketime/libfaketime.1.dylib \
         /usr/lib/x86_64-linux-gnu/faketime/libfaketime.so.1 /usr/lib/aarch64-linux-gnu/faketime/libfaketime.so.1; do
  [[ -z "$FAKETIME_LIB" && -f "$f" ]] && FAKETIME_LIB=$f
done
cleanup() { [[ -n "${ZPID:-}" ]] && kill "$ZPID" 2>/dev/null || true
            [[ -n "${ZPID2:-}" ]] && kill "$ZPID2" 2>/dev/null || true
            [[ -n "${NPID:-}" ]] && kill "$NPID" 2>/dev/null || true; sleep 1
            [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT
dcall() {  # <method> <params-json> -> result (exits on error)
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$1" "$2")
  local resp; resp=$(curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:${DPORT}/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
r=d.get("result"); print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$1"
}
zcall() { curl -s --max-time 60 -H 'content-type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" "http://127.0.0.1:$ZPORT" |
  python3 -c 'import sys,json; r=json.load(sys.stdin).get("result"); print(r if isinstance(r,str) else json.dumps(r))'; }

mkdir -p "$DIR/din" "$DIR/swaps"
"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$DPORT -port=$DP2P \
  -datadir="$DIR/din" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
  --consensus-contextual-locks-height=1 >"$DIR/din.log" 2>&1 & NPID=$!
start_zebra() {  # <name> <rpc port> <p2p port>; echoes the pid
  local name=$1 port=$2 p2p=$3
  cat >"$DIR/$name.toml" <<TOML
[network]
network = "Regtest"
listen_addr = "127.0.0.1:$p2p"
cache_dir = false
[network.testnet_parameters.activation_heights]
NU5 = 1
NU6 = 1
"NU6.1" = 1
[mining]
miner_address = "$("$TOOL" miner-address)"
[rpc]
listen_addr = "127.0.0.1:$port"
enable_cookie_auth = false
[state]
ephemeral = true
cache_dir = "$DIR/$name-state"
[tracing]
use_color = false
TOML
  echo "+0" >"$DIR/$name.faketime"
  if [[ -n "$FAKETIME_LIB" ]]; then
    # env (not nice/sudo, which macOS protects) so the DYLD_* variables reach zebrad.
    env DYLD_INSERT_LIBRARIES="$FAKETIME_LIB" DYLD_FORCE_FLAT_NAMESPACE=1 LD_PRELOAD="$FAKETIME_LIB" \
      FAKETIME_TIMESTAMP_FILE="$DIR/$name.faketime" FAKETIME_NO_CACHE=1 \
      "$ZEBRAD" -c "$DIR/$name.toml" start >"$DIR/$name.log" 2>&1 &
  else
    "$ZEBRAD" -c "$DIR/$name.toml" start >"$DIR/$name.log" 2>&1 &
  fi
  echo $!
}
zebra_ready() {  # <rpc port>: a fresh Zebra regtest with 110 wall-clock blocks
  local port=$1
  for _ in $(seq 1 60); do ZPORT=$port zcall getblockcount '[]' >/dev/null 2>&1 && break; sleep 0.5; done
  [[ "$(ZPORT=$port zcall getblockhash '[0]')" == "$REGTEST_GENESIS" ]] || { echo "ABORT: not a fresh Zebra regtest on $port"; exit 1; }
  python3 "$MINE" "$port" 110 >/dev/null
}
ZPID=$(start_zebra zebra "$ZPORT" "$ZP2P")

for _ in $(seq 1 90); do dcall getblockcount '[]' >/dev/null 2>&1 && break; sleep 1; done
swap_require_regtest dinerod "$(dcall getblockchaininfo '[]')"

# Alice's Dinero wallet (funds the DIN lock).
dcall wallet.createhd '["alice"]' >/dev/null
dcall wallet.encrypt '["pw-swap-zec"]' >/dev/null
dcall wallet.unlock '["pw-swap-zec", 3600]' >/dev/null
DADDR=$(dcall wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
dcall generatetoaddress "[110, \"$DADDR\"]" >/dev/null
# Zcash coins for Bob's deposits: mature coinbase, stamped with the wall clock.
zebra_ready "$ZPORT"
[[ -n "$FAKETIME_LIB" ]] && echo "Zebra clock control: $FAKETIME_LIB" || echo "Zebra clock control: none (libfaketime not found)"
clockfile() { [[ -n "$FAKETIME_LIB" ]] && echo "$DIR/$1.faketime"; }

rc=0
# alice-silent moves this Zebra's clock past T_zec, so it runs last here.
for scenario in happy tower-claim bob-silent alice-silent; do
  echo "=== $scenario ==="
  "$TOOL" "$scenario" "$DIR/swaps" "$DPORT" "$ZPORT" "$DADDR" "$MINE" $(clockfile zebra) || rc=1
done
echo "=== tower-refund (fresh Zebra) ==="
ZPORT2=$((DPORT + 12)); ZPID2=$(start_zebra zebra2 "$ZPORT2" $((DPORT + 15)))
zebra_ready "$ZPORT2"
"$TOOL" tower-refund "$DIR/swaps" "$DPORT" "$ZPORT2" "$DADDR" "$MINE" $(clockfile zebra2) || rc=1
[[ $rc -eq 0 ]] && echo "ZEC SWAP E2E: PASS" || { echo "ZEC SWAP E2E: FAIL"; tail -20 "$DIR/din.log"; exit 1; }
