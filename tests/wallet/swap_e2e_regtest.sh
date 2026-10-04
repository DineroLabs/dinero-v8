#!/usr/bin/env bash
# Two-chain end-to-end swap: one dinerod and one bitcoind on regtest, Alice and
# Bob each driving their own SwapRunner (tests/wallet/swap_e2e_tool.cpp).
# dinerod runs with transaction-lock enforcement from height 1, so timestamp
# locks are enforced by the chain, not only respected by the engine. It is a
# Utreexo bridge; a stateless CSN node follows it and must validate every swap
# spend, and the 60-second-block rules activate at height 130 (mid-swap).
# Bob's watchtower runs as its own process (dinero-swap-tower) on an inbox dir.
#   usage: swap_e2e_regtest.sh <dinerod> <swap_e2e_tool> <dinero-swap-tower>
set -euo pipefail
DINEROD=${1:?dinerod}; TOOL=${2:?swap_e2e_tool}; TOWER=${3:?dinero-swap-tower}
DIR=$(mktemp -d)
DPORT=$((20000 + RANDOM % 20000)); DP2P=$((DPORT + 7)); BPORT=$((DPORT + 13))
CPORT=$((DPORT + 21)); CP2P=$((DPORT + 23))
CONSENSUS=(--consensus-contextual-locks-height=1 --consensus-sixty-second-height=130)
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$BPORT -rpcuser=test -rpcpassword=test)
cleanup() { [[ -n "${TPID:-}" ]] && kill "$TPID" 2>/dev/null || true
            "${BCLI[@]}" stop >/dev/null 2>&1 || true
            [[ -n "${CPID:-}" ]] && kill "$CPID" 2>/dev/null || true
            [[ -n "${NPID:-}" ]] && kill "$NPID" 2>/dev/null || true; sleep 1; [[ -n "${KEEP_LOGS:-}" ]] && cp "$DIR"/*.log "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT
dcall() {  # <method> <params-json> -> result (exits on error)
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$1" "$2")
  local resp; resp=$(curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:${DPORT}/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
r=d.get("result"); print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$1"
}

mkdir -p "$DIR/din" "$DIR/csn" "$DIR/btc" "$DIR/swaps" "$DIR/inbox"; chmod 700 "$DIR/inbox"
"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$DPORT -port=$DP2P \
  -datadir="$DIR/din" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
  --utreexo=1 --utreexo-bridge=1 "${CONSENSUS[@]}" >"$DIR/din.log" 2>&1 & NPID=$!
bitcoind -regtest -datadir="$DIR/btc" -listen=0 -rpcport=$BPORT -rpcuser=test -rpcpassword=test \
  -daemon -fallbackfee=0.0002 >/dev/null
for _ in $(seq 1 90); do dcall getblockcount '[]' >/dev/null 2>&1 && break; sleep 1; done
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done

# Alice's Dinero wallet (funds the DIN lock).
dcall wallet.createhd '["alice"]' >/dev/null
dcall wallet.encrypt '["pw-swap-e2e"]' >/dev/null
dcall wallet.unlock '["pw-swap-e2e", 3600]' >/dev/null
DADDR=$(dcall wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
dcall generatetoaddress "[110, \"$DADDR\"]" >/dev/null

# Bob's Bitcoin wallet (funds the BTC lock).
"${BCLI[@]}" createwallet bob >/dev/null
BADDR=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 101 "$BADDR" >/dev/null

# A stateless Utreexo (CSN) node following the bridge: it must validate every
# block the swaps produce, using only the proofs the bridge serves.
"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$CPORT -port=$CP2P \
  -datadir="$DIR/csn" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
  --utreexo=1 --utreexo-stateless=1 --connect="127.0.0.1:$DP2P" "${CONSENSUS[@]}" >"$DIR/csn.log" 2>&1 & CPID=$!
ccall() { DPORT=$CPORT dcall "$@"; }

# Bob's watchtower: its own process, its own RPC connections, no keys.
"$TOWER" --inbox "$DIR/inbox" --din-rpc 127.0.0.1:$DPORT --din-auth test:test \
  --btc-rpc 127.0.0.1:$BPORT --btc-auth test:test --din-hrp rdin --interval 1 --escalate-after 5 \
  >"$DIR/tower.log" 2>&1 & TPID=$!

rc=0
# Scenarios that move bitcoind's clock forward are safe in any order: each
# honest scenario starts its locks from max(wall clock, Bitcoin MTP).
for scenario in happy stale-clocks tower-claim din-sign-first din-race race-late-reveal race-reorg offline tower-refund race-refund-overtaken; do
  echo "=== $scenario ==="
  "$TOOL" "$scenario" "$DIR/swaps" "$DPORT" "$BPORT" test test "$DADDR" "$BADDR" "$DIR/inbox" || rc=1
done
echo "=== CSN node ==="
TIP=$(dcall getbestblockhash '[]'); HEIGHT=$(dcall getblockcount '[]')
for _ in $(seq 1 120); do [[ "$(ccall getbestblockhash '[]' 2>/dev/null)" == "$TIP" ]] && break; sleep 1; done
CTIP=$(ccall getbestblockhash '[]' 2>/dev/null || echo none)
if [[ "$CTIP" == "$TIP" ]]; then
  echo "  OK   CSN node validated the whole swap chain to height $HEIGHT (tip ${TIP:0:16})"
else
  echo "  FAIL CSN node tip ${CTIP:0:16} != bridge tip ${TIP:0:16} (height $HEIGHT)"; rc=1
  grep -iE "reject|invalid|fail" "$DIR/csn.log" | tail -15
fi
SIXTY=$(dcall getblockchaininfo '[]' | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d.get("blocks"))')
[[ "$SIXTY" -gt 130 ]] && echo "  OK   swaps ran across the 60-second activation (height 130, tip $SIXTY)" \
  || { echo "  FAIL chain never crossed height 130"; rc=1; }
echo "=== tower log ==="; grep -v "not observed" "$DIR/tower.log" | sed 's|'"$DIR"'/inbox/||' || true
[[ $rc -eq 0 ]] && echo "SWAP E2E: PASS" || { echo "SWAP E2E: FAIL"; tail -20 "$DIR/din.log"; exit 1; }
