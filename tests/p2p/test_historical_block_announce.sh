#!/usr/bin/env bash
# A syncing node must not announce (cmpctblock/inv) the historical blocks it
# connects. Node A mines 30 blocks; a fresh node B syncs them from A. B may
# announce only blocks within kMaxAnnounceLagBlocks (6) of its best header.
# Usage: test_historical_block_announce.sh <build-dir>
set -u
BUILD=$1; WORK=$(mktemp -d "${TMPDIR:-/tmp}/dinero-announce.XXXXXX")
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
start "$A" 32996 32001
for _ in $(seq 30); do "$CLI" -datadir="$A" -rpcport=32996 generate 1 > /dev/null 2>&1 || { echo "FAIL: mining"; exit 1; }; done
start "$B" 32997 32002 --addnode=127.0.0.1:32001
for i in $(seq 120); do
    [[ "$(cli "$B" 32997 getblockcount)" == 30 ]] && break
    sleep 1
done
[[ "$(cli "$B" 32997 getblockcount)" == 30 ]] || { echo "FAIL: B did not sync (height $(cli "$B" 32997 getblockcount))"; exit 1; }
sleep 2
announced=$(grep -o "Block announced via ChainstateService (active tip) hash=[0-9a-f]*" "$B/node.log" | sort -u | wc -l | tr -d ' ')
held=$(grep -c "catching up; not announced" "$B/node.log")
echo "B announced=$announced held_back=$held (work $WORK)"
# Acceptance notifications can arrive after a batch has advanced the active
# tip. Those callbacks intentionally announce nothing, so held + announced
# need not equal 30. Check which blocks were announced, not a scheduler-
# dependent minimum count of historical callbacks.
allowed=" "
for height in $(seq 24 30); do
    hash=$(cli "$A" 32996 getblockhash "$height")
    [[ ${#hash} == 64 ]] || { echo "FAIL: cannot read allowed block $height"; exit 1; }
    allowed+="${hash:0:16} "
done
bad=0
while read -r hash; do
    [[ "$allowed" == *" $hash "* ]] || bad=1
done < <(grep -o "Block announced via ChainstateService (active tip) hash=[0-9a-f]*" "$B/node.log" | sed 's/.*hash=//' | sort -u)
if (( held > 0 && announced >= 1 && announced <= 7 && bad == 0 )); then echo "PASS"; rm -rf "$WORK"; exit 0; fi
echo "FAIL: a syncing node announced historical blocks"; exit 1
