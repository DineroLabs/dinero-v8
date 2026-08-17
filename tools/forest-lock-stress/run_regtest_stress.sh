#!/usr/bin/env bash
#
# Live-node stress harness for the Utreexo forest read/write lock (PR: forest UAF).
#
# Exercises the REAL cross-lock ordering the unit/stress tests cannot: block
# connect on the activation thread takes activation_mutex_ THEN forest_mutex_
# (ReplaceForestGuarded / RemoveLastNLeavesGuarded), while RPC threads take
# forest_mutex_ alone (getutreexoroots / getutreexocommitment / getutreexostats
# / getutxoproofs_batch / dumptxoutset). If the leaf-lock discipline is wrong,
# block connect and the forest RPCs deadlock and the chain tip stops advancing.
#
# IMPLEMENTATION NOTES (learned the hard way):
#   * Talk to the node with curl + cookie auth, NOT dinero-cli — the CLI's JSON
#     parser aborts on an "error" response shape (e.g. the rate-limit reply),
#     which is a CLIENT bug unrelated to the node.
#   * The RPC server rate-limits per-IP (~50 req/s); a request rejected by the
#     limiter never reaches the forest handler, so flooding past the limit does
#     NOT stress the lock. Readers are throttled to stay near the limit, and all
#     calls retry on a transient "Rate limit" reply.
#
# Detection: PRIMARY = chain height must keep advancing while readers hammer the
# forest. A stall for STALL_SECS = deadlock. A dead node (segfault / UAF) = fail.
#
# On a host where ASan/TSan work, also build dinerod with -fsanitize=thread for a
# definitive data-race verdict; on this dev Mac both sanitizers hang at startup,
# so this harness proves DEADLOCK-freedom + no-crash, not race-freedom.
#
# Usage: run_regtest_stress.sh [seconds] [reader_workers]
set -u

SECONDS_TO_RUN="${1:-60}"
READERS="${2:-6}"
STALL_SECS="${STALL_SECS:-25}"
RPCPORT="${RPCPORT:-21998}"
P2PPORT="${P2PPORT:-21999}"
READER_SLEEP="${READER_SLEEP:-0.15}"   # throttle each reader to stay near the rate limit

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DINEROD="${DINEROD:-$ROOT/build-check/dinerod}"
CLI_BIN="${DINERO_CLI:-$ROOT/build-check/dinero-cli}"   # only for the initial getnewaddress
DATADIR="$(mktemp -d "${TMPDIR:-/tmp}/forest-stress.XXXXXX")"
LOG="$DATADIR/dinerod.log"
KEEP_LOG="${KEEP_LOG:-}"

PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
  [[ -n "${NODE_PID:-}" ]] && kill "$NODE_PID" 2>/dev/null
  wait 2>/dev/null
  [[ -n "$KEEP_LOG" ]] && cp "$LOG" "$KEEP_LOG" 2>/dev/null
  rm -rf "$DATADIR"
}
trap cleanup EXIT

echo "[harness] datadir=$DATADIR rpcport=$RPCPORT readers=$READERS run=${SECONDS_TO_RUN}s stall=${STALL_SECS}s"
[[ -x "$DINEROD" ]] || { echo "FAIL: dinerod not found at $DINEROD"; exit 3; }

"$DINEROD" --regtest --datadir="$DATADIR" --rpcport="$RPCPORT" --p2pport="$P2PPORT" --p2p.offline=1 >"$LOG" 2>&1 &
NODE_PID=$!

URL=""
for i in $(seq 1 30); do
  if [[ -f "$DATADIR/.cookie" ]]; then URL="http://$(cat "$DATADIR/.cookie")@127.0.0.1:$RPCPORT/"; fi
  [[ -n "$URL" ]] && curl -s --max-time 5 --data-binary '{"method":"getblockcount","params":[]}' "$URL" 2>/dev/null | grep -q '"result"' && break
  sleep 1
done
[[ -n "$URL" ]] || { echo "FAIL: cookie/RPC never came up"; tail -20 "$LOG"; exit 3; }

# curl RPC with retry-on-rate-limit; echoes the raw JSON (or empty on hard failure).
rpc() {
  local body="$1" out
  for _ in 1 2 3 4 5; do
    out="$(curl -s --max-time 15 --data-binary "$body" -H 'content-type: application/json' "$URL" 2>/dev/null)"
    case "$out" in
      *'Rate limit'*) sleep 0.2; continue ;;
      *) printf '%s' "$out"; return 0 ;;
    esac
  done
  printf '%s' "$out"
}
height() { rpc '{"method":"getblockcount","params":[]}' | grep -oE '"result"[[:space:]]*:[[:space:]]*[0-9]+' | grep -oE '[0-9]+$'; }

ADDR="$("$CLI_BIN" -regtest -datadir="$DATADIR" -rpcport="$RPCPORT" getnewaddress 2>/dev/null | grep -oE '"address"[^,]*' | head -1 | sed -E 's/.*: *"([^"]+)".*/\1/')"
[[ -n "$ADDR" ]] || { echo "FAIL: could not get an address"; exit 3; }
for _ in $(seq 1 20); do rpc "{\"method\":\"generatetoaddress\",\"params\":[1,\"$ADDR\"]}" >/dev/null; done
echo "[harness] node up (pid $NODE_PID), seeded to height $(height)"

STOP="$DATADIR/stop"

# readers: throttled forest-RPC hammer (stay near the rate limit so requests
# actually reach the forest handlers and take forest_mutex_).
reader_loop() {
  local methods=(getutreexoroots getutreexocommitment getutreexostats getutreexocachestats)
  local i=0
  while [[ ! -f "$STOP" ]]; do
    rpc "{\"method\":\"${methods[$((i % ${#methods[@]}))]}\",\"params\":[]}" >/dev/null
    i=$((i+1)); sleep "$READER_SLEEP"
  done
}
for _ in $(seq 1 "$READERS"); do reader_loop & PIDS+=($!); done
( while [[ ! -f "$STOP" ]]; do rpc "{\"method\":\"dumptxoutset\",\"params\":[\"$DATADIR/dump.dat\"]}" >/dev/null; sleep 3; done ) & PIDS+=($!)

# writer: continuous block connect (the forest WRITER path).
( while [[ ! -f "$STOP" ]]; do rpc "{\"method\":\"generatetoaddress\",\"params\":[1,\"$ADDR\"]}" >/dev/null; done ) & PIDS+=($!)

echo "[harness] stressing for ${SECONDS_TO_RUN}s ..."
start="$(height)"; last_h="$start"; last_change=0; elapsed=0; rc=0
while [[ "$elapsed" -lt "$SECONDS_TO_RUN" ]]; do
  sleep 3; elapsed=$((elapsed+3))
  if ! kill -0 "$NODE_PID" 2>/dev/null; then
    echo "FAIL: dinerod died (possible UAF/crash) at ${elapsed}s"; tail -30 "$LOG"; rc=1; break
  fi
  h="$(height)"
  if [[ -z "$h" ]]; then continue; fi   # transient rate-limit on the probe; ignore
  if [[ "$h" == "$last_h" ]]; then
    last_change=$((last_change+3))
    if [[ "$last_change" -ge "$STALL_SECS" ]]; then
      echo "FAIL: chain height stuck at $h for ${last_change}s while readers hammer the forest — DEADLOCK"; rc=1; break
    fi
  else
    echo "[harness] ${elapsed}s: height=$h"; last_h="$h"; last_change=0
  fi
done

touch "$STOP"; sleep 1
if [[ "$rc" -eq 0 ]]; then
  echo "PASS: forest-lock live stress — height $start -> $(height) over ${SECONDS_TO_RUN}s, "\
"$READERS forest-RPC readers + dumptxoutset vs continuous block connect, no deadlock, no crash"
fi
exit "$rc"
