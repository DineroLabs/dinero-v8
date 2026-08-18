#!/usr/bin/env bash
#
# CSN-mode live stress harness for the Utreexo forest read/write lock (PR #570).
#
# The base harness (run_regtest_stress.sh) exercises a STATEFUL node: block
# connect via ReplaceForestGuarded vs forest-RPC readers. This variant exercises
# the CSN (utreexo_stateless=1) paths that the base harness cannot reach:
#
#   WRITERS (all on the single ordered CSN worker, #377):
#     - ValidateUtreexoProof        -> mutateForest wrap (the :606 fix)
#     - ValidateWithTransitionProof -> mutateForest wrap (pre-existing)
#     - RewindToCheckpoint / ReplayBlock (reorg) -> MutateForestGuarded
#   READERS (off-thread, shared lock):
#     - forest RPCs on the CSN (getutreexoroots/commitment/stats/cachestats)
#
# Topology: BRIDGE node mines continuously; CSN node connects to it and applies
# every block through the StatelessNode proof path while readers hammer its
# forest RPCs. Periodic invalidateblock on the bridge forces reorgs through the
# CSN rewind/replay path.
#
# Detection: PRIMARY = CSN chain height must keep tracking the miner. A stall
# for STALL_SECS = deadlock. A dead node = crash/UAF. Secondary: at the end the
# CSN log MUST show proof-application activity (otherwise the run exercised
# nothing and the harness FAILS — silence is not success), and bridge/CSN must
# agree on the best hash.
#
# Usage: run_csn_stress.sh [seconds] [reader_workers]
set -u

SECONDS_TO_RUN="${1:-90}"
READERS="${2:-6}"
STALL_SECS="${STALL_SECS:-30}"
REORG_EVERY="${REORG_EVERY:-15}"
REORG_DEPTH="${REORG_DEPTH:-2}"
CHECKPOINT_INTERVAL="${CHECKPOINT_INTERVAL:-25}"
READER_SLEEP="${READER_SLEEP:-0.15}"

RPC_B="${RPC_B:-22998}";  P2P_B="${P2P_B:-22999}";  WS_B="${WS_B:-23996}"
RPC_C="${RPC_C:-23998}";  P2P_C="${P2P_C:-23999}";  WS_C="${WS_C:-23997}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DINEROD="${DINEROD:-$ROOT/build/dinerod}"
CLI_BIN="${DINERO_CLI:-$ROOT/build/dinero-cli}"
DIR_B="$(mktemp -d "${TMPDIR:-/tmp}/csn-stress-bridge.XXXXXX")"
DIR_C="$(mktemp -d "${TMPDIR:-/tmp}/csn-stress-csn.XXXXXX")"
LOG_B="${KEEP_LOG:+${KEEP_LOG}.bridge}"; LOG_B="${LOG_B:-$DIR_B/dinerod.log}"
LOG_C="${KEEP_LOG:+${KEEP_LOG}.csn}"; LOG_C="${LOG_C:-$DIR_C/dinerod.log}"
KEEP_LOG="${KEEP_LOG:-}"

PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
  [[ -n "${PID_B:-}" ]] && kill "$PID_B" 2>/dev/null
  [[ -n "${PID_C:-}" ]] && kill "$PID_C" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$DIR_B" "$DIR_C"
}
trap cleanup EXIT

[[ -x "$DINEROD" ]] || { echo "FAIL: dinerod not found at $DINEROD"; exit 3; }
echo "[csn-harness] bridge rpc=$RPC_B  csn rpc=$RPC_C  readers=$READERS run=${SECONDS_TO_RUN}s stall=${STALL_SECS}s reorg-every=${REORG_EVERY}s ckpt=${CHECKPOINT_INTERVAL}"

"$DINEROD" --regtest --datadir="$DIR_B" --rpcport="$RPC_B" --port="$P2P_B" \
  --wallet-socket-port="$WS_B" --listen=1 --utreexo=1 --utreexo-bridge=1 \
  >"$LOG_B" 2>&1 &
PID_B=$!

"$DINEROD" --regtest --datadir="$DIR_C" --rpcport="$RPC_C" --port="$P2P_C" \
  --wallet-socket-port="$WS_C" --listen=1 --utreexo=1 --utreexo-stateless=1 \
  --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL" \
  --connect="127.0.0.1:$P2P_B" \
  >"$LOG_C" 2>&1 &
PID_C=$!

url_for() { # datadir rpcport
  [[ -f "$1/.cookie" ]] && echo "http://$(cat "$1/.cookie")@127.0.0.1:$2/"
}
URL_B=""; URL_C=""
for i in $(seq 1 30); do
  [[ -z "$URL_B" ]] && URL_B="$(url_for "$DIR_B" "$RPC_B")"
  [[ -z "$URL_C" ]] && URL_C="$(url_for "$DIR_C" "$RPC_C")"
  ok_b=""; ok_c=""
  [[ -n "$URL_B" ]] && curl -s --max-time 5 --data-binary '{"method":"getblockcount","params":[]}' "$URL_B" 2>/dev/null | grep -q '"result"' && ok_b=1
  [[ -n "$URL_C" ]] && curl -s --max-time 5 --data-binary '{"method":"getblockcount","params":[]}' "$URL_C" 2>/dev/null | grep -q '"result"' && ok_c=1
  [[ -n "$ok_b" && -n "$ok_c" ]] && break
  sleep 1
done
[[ -n "$URL_B" && -n "$URL_C" ]] || { echo "FAIL: a node never came up"; tail -20 "$LOG_B" "$LOG_C"; exit 3; }

rpc() { # url body
  local url="$1" body="$2" out
  for _ in 1 2 3 4 5; do
    out="$(curl -s --max-time 15 --data-binary "$body" -H 'content-type: application/json' "$url" 2>/dev/null)"
    case "$out" in
      *'Rate limit'*) sleep 0.2; continue ;;
      *) printf '%s' "$out"; return 0 ;;
    esac
  done
  printf '%s' "$out"
}
height()   { rpc "$1" '{"method":"getblockcount","params":[]}' | grep -oE '"result"[[:space:]]*:[[:space:]]*[0-9]+' | grep -oE '[0-9]+$'; }
besthash() { rpc "$1" '{"method":"getbestblockhash","params":[]}' | grep -oE '"result"[[:space:]]*:[[:space:]]*"[0-9a-f]+"' | grep -oE '[0-9a-f]{64}'; }
blockhash(){ rpc "$1" "{\"method\":\"getblockhash\",\"params\":[$2]}" | grep -oE '"result"[[:space:]]*:[[:space:]]*"[0-9a-f]+"' | grep -oE '[0-9a-f]{64}'; }

ADDR="$("$CLI_BIN" -regtest -datadir="$DIR_B" -rpcport="$RPC_B" getnewaddress 2>/dev/null | grep -oE '"address"[^,]*' | head -1 | sed -E 's/.*: *"([^"]+)".*/\1/')"
[[ -n "$ADDR" ]] || { echo "FAIL: could not get a mining address on the bridge"; exit 3; }
for _ in $(seq 1 30); do rpc "$URL_B" "{\"method\":\"generatetoaddress\",\"params\":[1,\"$ADDR\"]}" >/dev/null; done
echo "[csn-harness] nodes up (bridge $PID_B, csn $PID_C). bridge height=$(height "$URL_B"), waiting for CSN to track..."

# CSN must demonstrably sync before the stress phase, or the run proves nothing.
synced=""
for _ in $(seq 1 45); do
  hb="$(height "$URL_B")"; hc="$(height "$URL_C")"
  [[ -n "$hb" && -n "$hc" && "$hc" -ge $((hb - 1)) && "$hc" -gt 0 ]] && { synced=1; break; }
  sleep 1
done
[[ -n "$synced" ]] || { echo "FAIL: CSN never synced to the bridge (bridge=$(height "$URL_B") csn=$(height "$URL_C"))"; tail -30 "$LOG_C"; exit 3; }
echo "[csn-harness] CSN tracking at height $(height "$URL_C") — starting stress"

STOP="$DIR_C/stop"

# READERS: throttled forest-RPC hammer on the CSN (shared-lock path).
reader_loop() {
  # getblocktemplate drives block_assembler's guarded clone (ComputeUtreexoRootPure);
  # if it errors on a CSN that's tolerated — the getutreexo* four are the core coverage.
  local methods=(getutreexoroots getutreexocommitment getutreexostats getutreexocachestats getblocktemplate)
  local i=0
  while [[ ! -f "$STOP" ]]; do
    rpc "$URL_C" "{\"method\":\"${methods[$((i % ${#methods[@]}))]}\",\"params\":[]}" >/dev/null
    i=$((i+1)); sleep "$READER_SLEEP"
  done
}
for _ in $(seq 1 "$READERS"); do reader_loop & PIDS+=($!); done

# WRITER: continuous mining on the bridge -> continuous CSN proof application.
( while [[ ! -f "$STOP" ]]; do rpc "$URL_B" "{\"method\":\"generatetoaddress\",\"params\":[1,\"$ADDR\"]}" >/dev/null; sleep 0.3; done ) & PIDS+=($!)

# REORGER stops when $STOP2 appears (45s before end) so the final reorg settles.
# REORGER: every REORG_EVERY s, invalidate tip-2 on the bridge and mine past it
# -> the CSN must rewind (RewindToCheckpoint) and replay (ReplayBlock).
STOP2="$DIR_C/stop-reorg"
( while [[ ! -f "$STOP" && ! -f "$STOP2" ]]; do
    sleep "$REORG_EVERY"
    [[ -f "$STOP" || -f "$STOP2" ]] && break
    hb="$(height "$URL_B")"; [[ -z "$hb" || "$hb" -lt 5 ]] && continue
    target="$(blockhash "$URL_B" $((hb - REORG_DEPTH)))"; [[ -z "$target" ]] && continue
    rpc "$URL_B" "{\"method\":\"invalidateblock\",\"params\":[\"$target\"]}" >/dev/null
    for _ in $(seq 1 $((REORG_DEPTH + 2))); do rpc "$URL_B" "{\"method\":\"generatetoaddress\",\"params\":[1,\"$ADDR\"]}" >/dev/null; done
    echo "[csn-harness] reorg injected at bridge height $hb (invalidated $((hb-REORG_DEPTH)))"
  done ) & PIDS+=($!)

start_c="$(height "$URL_C")"; last_h="$start_c"; last_change=0; elapsed=0; rc=0
while [[ "$elapsed" -lt "$SECONDS_TO_RUN" ]]; do
  sleep 3; elapsed=$((elapsed+3))
  [[ "$elapsed" -ge $((SECONDS_TO_RUN - 45)) && ! -f "$DIR_C/stop-reorg" ]] && touch "$DIR_C/stop-reorg"
  if ! kill -0 "$PID_C" 2>/dev/null; then
    echo "FAIL: CSN dinerod died (possible UAF/crash) at ${elapsed}s"; tail -40 "$LOG_C"; rc=1; break
  fi
  if ! kill -0 "$PID_B" 2>/dev/null; then
    echo "FAIL: bridge dinerod died at ${elapsed}s"; tail -40 "$LOG_B"; rc=1; break
  fi
  h="$(height "$URL_C")"
  [[ -z "$h" ]] && continue
  if [[ "$h" == "$last_h" ]]; then
    last_change=$((last_change+3))
    if [[ "$last_change" -ge "$STALL_SECS" ]]; then
      echo "FAIL: CSN height stuck at $h for ${last_change}s under reader load — DEADLOCK"; rc=1; break
    fi
  else
    echo "[csn-harness] ${elapsed}s: csn=$h bridge=$(height "$URL_B")"
    last_h="$h"; last_change=0
  fi
done

touch "$STOP"; sleep 2

if [[ "$rc" -eq 0 ]]; then
  # Coverage proof: the CSN log must show StatelessNode proof application —
  # a run that never exercised the guarded apply path is a FAILED run.
  applies="$(grep -c "StatelessNode" "$LOG_C" 2>/dev/null || echo 0)"
  if [[ "$applies" -lt 10 ]]; then
    echo "FAIL: CSN log shows only ${applies} StatelessNode lines — the proof-application path was not exercised"; rc=1
  fi
fi
if [[ "$rc" -eq 0 ]]; then
  # NB: capture then test — `if grep | head` would test head's status, which is
  # always 0, turning this into an unconditional FAIL (bug caught on first run).
  markers="$(grep -nE "FATAL|Assertion failed|SIGSEGV|deadlock" "$LOG_C" | head -3)"
  if [[ -n "$markers" ]]; then
    echo "$markers"
    echo "FAIL: fatal markers in the CSN log (above)"; rc=1
  fi
fi
if [[ "$rc" -eq 0 ]]; then
  # Convergence: after the last reorg settles, tips must agree.
  sleep 20
  bh_b="$(besthash "$URL_B")"; bh_c="$(besthash "$URL_C")"
  if [[ -n "$bh_b" && "$bh_b" == "$bh_c" ]]; then
    echo "PASS: CSN forest-lock stress — csn height $start_c -> $(height "$URL_C") over ${SECONDS_TO_RUN}s;"
    echo "      $READERS forest-RPC readers vs continuous proof application + $((SECONDS_TO_RUN / REORG_EVERY)) injected reorgs;"
    echo "      no deadlock, no crash; ${applies} StatelessNode log lines (path exercised); tips converged ($bh_c)"
  else
    echo "FAIL: tips diverged after stress (bridge=$bh_b csn=$bh_c)"; rc=1
  fi
fi
exit "$rc"
