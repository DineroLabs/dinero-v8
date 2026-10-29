#!/usr/bin/env bash
# The best header is chosen by work alone. When it sits on a branch we consider
# invalid, our own new blocks must still be announced. Node B syncs 10 blocks
# from A, invalidates block 3 (its best header stays at A's height 10) and mines
# its own block 3: that block is more than 6 below the best header but on a
# different branch, so it must be announced.
# Usage: test_split_branch_announce.sh <build-dir>
set -u
BUILD=$1; WORK=$(mktemp -d "${TMPDIR:-/tmp}/dinero-split-announce.XXXXXX")
DIND="$BUILD/dinerod"; CLI="$BUILD/dinero-cli"
A="$WORK/a"; B="$WORK/b"; PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 2; for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; }
trap cleanup EXIT
cli() { local dir=$1 port=$2; shift 2; "$CLI" -datadir="$dir" -rpcport="$port" "$@" 2>/dev/null | tr -d '"[:space:]'; }
start() { local dir=$1 rpc=$2 p2p=$3; shift 3
    "$DIND" --regtest --datadir="$dir" --rpcport="$rpc" --port="$p2p" --dnsseed=0 --fixedseeds=0 "$@" >> "$dir/node.log" 2>&1 &
    PIDS+=($!)
    for _ in $(seq 60); do [[ -n "$(cli "$dir" "$rpc" getblockcount)" ]] && return 0; sleep 1; done
    echo "FAIL: node at $dir did not answer RPC"; exit 1; }
mkdir -p "$A" "$B"
start "$A" 33996 33001
for _ in $(seq 10); do "$CLI" -datadir="$A" -rpcport=33996 generate 1 > /dev/null 2>&1 || { echo "FAIL: mining"; exit 1; }; done
start "$B" 33997 33002 --addnode=127.0.0.1:33001
for _ in $(seq 120); do [[ "$(cli "$B" 33997 getblockcount)" == 10 ]] && break; sleep 1; done
[[ "$(cli "$B" 33997 getblockcount)" == 10 ]] || { echo "FAIL: B did not sync"; exit 1; }
kill "${PIDS[0]}"; sleep 2   # A gone: B cannot re-fetch A's branch
bad=$(cli "$B" 33997 getblockhash 3)
"$CLI" -datadir="$B" -rpcport=33997 invalidateblock "$bad" > /dev/null 2>&1 || { echo "FAIL: invalidateblock"; exit 1; }
[[ "$(cli "$B" 33997 getblockcount)" == 2 ]] || { echo "FAIL: B tip after invalidate is $(cli "$B" 33997 getblockcount)"; exit 1; }
"$CLI" -datadir="$B" -rpcport=33997 generate 1 > /dev/null 2>&1 || { echo "FAIL: B mining"; exit 1; }
own=$(cli "$B" 33997 getblockhash 3)
[[ -n "$own" && "$own" != "$bad" ]] || { echo "FAIL: B did not mine its own block 3"; exit 1; }
sleep 2
if grep -q "Block announced via ChainstateService (active tip) hash=${own:0:16}" "$B/node.log"; then
    echo "PASS: own block on the other branch announced"; rm -rf "$WORK"; exit 0
fi
grep -n "height=3" "$B/node.log" | tail -5
echo "FAIL: B's own block was not announced (work $WORK)"; exit 1
