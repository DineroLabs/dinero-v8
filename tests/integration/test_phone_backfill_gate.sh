#!/usr/bin/env bash
# Pre-base body backfill feeds background validation, which the ios_utreexo
# phone profile never runs (it keeps no genesis-to-base bodies). A phone that
# loads an AssumeUTXO snapshot must therefore not arm backfill, while the
# mac_fullblock profile (desktop dinerod, macOS DineroDPI) still must.
#
# Both nodes load the same snapshot with the base header already known, so
# the scheduler tick loop would arm backfill within seconds if allowed.
# Usage: DINEROD=/path/to/dinerod test_phone_backfill_gate.sh
set -euo pipefail
DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
[[ -x "$DINEROD" ]] || { echo "dinerod not executable at $DINEROD"; exit 1; }
command -v jq >/dev/null || { echo "jq is required"; exit 1; }
BASE_PORT="${BASE_PORT:-37500}"
SRC_RPC=$((BASE_PORT + 0)); SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
PH_RPC=$((BASE_PORT + 1));  PH_P2P=$((BASE_PORT + 101));  PH_WS=$((BASE_PORT + 201))
DT_RPC=$((BASE_PORT + 2));  DT_P2P=$((BASE_PORT + 102));  DT_WS=$((BASE_PORT + 202))
WORK="$(mktemp -d -t dinero_phone_backfill_XXXXXX)"
SRC_DIR="$WORK/source"; PH_DIR="$WORK/phone"; DT_DIR="$WORK/desktop"; SNAP="$WORK/utxo-snapshot.dat"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for lg in "$WORK"/*/daemon*.log; do [[ -f "$lg" ]] && { printf -- '--- tail %s ---\n' "$lg" >&2; tail -30 "$lg" >&2; }; done
    KEEP_ON_FAIL=1; exit 1
}
cleanup() {
    pkill -f "datadir=$WORK" 2>/dev/null || true; sleep 1; pkill -9 -f "datadir=$WORK" 2>/dev/null || true
    if [[ "$KEEP_ON_FAIL" != "1" ]]; then rm -rf "$WORK"; else echo "[INFO] kept $WORK" >&2; fi
}
trap cleanup EXIT

cookie_for() { local f; for f in "$1/.cookie" "$1/regtest/.cookie"; do [[ -f "$f" ]] && { tr -d '\n' < "$f"; return 0; }; done; return 1; }
rpc() {  # <port> <datadir> <method> [params]
    local cookie; cookie="$(cookie_for "$2")" || return 1
    curl -fsS --max-time 30 --user "$cookie" -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$3\",\"params\":${4:-[]}}" "http://127.0.0.1:$1/"
}
start_node() {  # <datadir> <rpc> <p2p> <ws> [extra...]
    local d="$1" r="$2" p="$3" w="$4"; shift 4; mkdir -p "$d"
    "$DINEROD" --regtest --consensus-state-commitment-height=4294967295 --datadir="$d" \
        --rpcport="$r" --port="$p" --wallet-socket-port="$w" --listen=0 --dnsseed=0 --fixedseeds=0 "$@" \
        > "$d/daemon.log" 2>&1 &
    local i; for i in $(seq 1 90); do
        rpc "$r" "$d" getblockcount 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1 && return 0; sleep 1
    done
    fail "node $d did not become ready"
}
stop_node() {
    pkill -f "datadir=$1" 2>/dev/null || true
    local i; for i in $(seq 1 60); do pgrep -f "datadir=$1" >/dev/null 2>&1 || return 0; sleep 0.5; done
    pkill -9 -f "datadir=$1" 2>/dev/null || true; sleep 1
}
snap_status() { rpc "$1" "$2" getsnapshotbootstrapstatus 2>/dev/null | jq -c '.result.snapshot_bootstrap // {}' 2>/dev/null || echo '{}'; }

# Source chain and snapshot at its tip; copy its headers so the base is known.
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS"
rpc "$SRC_RPC" "$SRC_DIR" generate '[110]' | jq -e '.result.blocks | length == 110' >/dev/null || fail "mining"
rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP\"]" | jq -e '.result.coins_written >= 1' >/dev/null || fail "dumptxoutset"
stop_node "$SRC_DIR"
for d in "$PH_DIR" "$DT_DIR"; do mkdir -p "$d"; cp -R "$SRC_DIR/headers" "$d/headers"; done

load() {  # <port> <datadir> <label>
    rpc "$1" "$2" loadtxoutset "[\"$SNAP\"]" | jq -e '.result.coins_loaded >= 1' >/dev/null || fail "$3: loadtxoutset"
}

# Desktop profile: backfill must arm (behaviour preserved).
start_node "$DT_DIR" "$DT_RPC" "$DT_P2P" "$DT_WS" --sync-profile=mac_fullblock
load "$DT_RPC" "$DT_DIR" desktop
armed=0
for _ in $(seq 1 40); do
    snap_status "$DT_RPC" "$DT_DIR" | jq -e '.assumeutxo_active == true and .backfill_enabled == true' >/dev/null && { armed=1; break; }
    sleep 1
done
[[ "$armed" == 1 ]] || fail "mac_fullblock: backfill never armed (last: $(snap_status "$DT_RPC" "$DT_DIR"))"
pass "mac_fullblock arms pre-base backfill"
stop_node "$DT_DIR"

# Phone profile: backfill must stay off for longer than several 5 s ticks.
start_node "$PH_DIR" "$PH_RPC" "$PH_P2P" "$PH_WS" --sync-profile=ios_utreexo
load "$PH_RPC" "$PH_DIR" phone
active=0
for i in $(seq 1 30); do
    st="$(snap_status "$PH_RPC" "$PH_DIR")"
    jq -e '.backfill_enabled == true' <<<"$st" >/dev/null && fail "ios_utreexo armed backfill at sample $i: $st"
    jq -e '.assumeutxo_active == true and .history_fully_validated == false' <<<"$st" >/dev/null && active=1
    sleep 1
done
[[ "$active" == 1 ]] || fail "ios_utreexo never reported an active, unvalidated snapshot (last: $st)"
pass "ios_utreexo with a loaded snapshot never arms pre-base backfill"
