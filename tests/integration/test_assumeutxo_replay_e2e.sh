#!/usr/bin/env bash
#
# AssumeUTXO replay engine e2e (regtest).
# Spec docs/design/assumeutxo-fatal-state-machine.md Required Tests:
#   Scenario A+D = Test 4 (good snapshot retires trust marker through REAL
#                  genesis->base replay; fully_validated survives restart)
#                  PLUS cross-restart stall recovery: peerless load stalls in
#                  ~15s (proves --assumeutxo_bg_stall_timeout CLI delivery),
#                  the stall persists across a peerless restart (no silent
#                  promotion), and a complete replay pass after bodies arrive
#                  retires the trust marker.
#   Scenario B   = Test 1 (poisoned snapshot is fatal: load-time gates pass —
#                  regtest has no compiled-in anchor, simulating a compromised
#                  binary — but the genesis replay recomputes an honest digest
#                  that mismatches the poisoned commitment). Fatal survives
#                  restart; B2 = explicit operator reset recovers.
#   Scenario C   = re-entry: loading a DIFFERENT snapshot mid-lifecycle is
#                  refused. C1: the live RPC path refuses (the consensus set is
#                  populated by the active snapshot, so the empty-set
#                  precondition fires first — the belt is unreachable over RPC
#                  by construction). C2: the belt itself ("another snapshot
#                  lifecycle is active") fires on the startup-rehydrate path
#                  when a DIFFERENT-base assumeutxo_snapshot is configured
#                  mid-lifecycle, and the daemon refuses to start.
#
# TOPOLOGY NOTES (discovered empirically; they shape this script):
#   * loadtxoutset requires an EMPTY consensus UTXO set, and the snapshot
#     base block must already be on the node's header chain. The consumer
#     nodes are therefore seeded with a copy of the source's headers/ store
#     (the production "headers-first, no bodies yet" state) taken while the
#     source is stopped.
#   * The daemon has NO P2P backfill of pre-base bodies yet: after a snapshot
#     load publishes the tip at the base, the download scheduler reports
#     is_fully_synchronized and never requests heights 1..base. Bodies are
#     delivered through the node's real block-acceptance path via submitblock.
#   * Stored (not connected) bodies do not populate ChainDB's canonical
#     height->hash index, which the background worker requires. A restart
#     with --reindex (the documented operator remedy) rebuilds the index from
#     the flatfiles; the worker's complete pass then runs the real replay.
#
set -euo pipefail

DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
BASE_PORT="${BASE_PORT:-36500}"
RUN_SCENARIOS="${RUN_SCENARIOS:-AD B C}"

SRC_RPC=$((BASE_PORT + 0));  SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
AD_RPC=$((BASE_PORT + 1));   AD_P2P=$((BASE_PORT + 101));  AD_WS=$((BASE_PORT + 201))
B_RPC=$((BASE_PORT + 2));    B_P2P=$((BASE_PORT + 102));   B_WS=$((BASE_PORT + 202))
C_RPC=$((BASE_PORT + 3));    C_P2P=$((BASE_PORT + 103));   C_WS=$((BASE_PORT + 203))

WORK="$(mktemp -d -t dinero_replay_e2e_XXXXXX)"
SRC_DIR="$WORK/source"; AD_DIR="$WORK/nodeAD"; B_DIR="$WORK/nodeB"; C_DIR="$WORK/nodeC"
SNAP="$WORK/utxo-snapshot.dat"; SNAP2="$WORK/utxo-snapshot-2.dat"; POISON="$WORK/poisoned.dat"
HEADERS_AT_BASE="$WORK/headers_at_base"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
SHIELDED_NOTE="not attempted"

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for d in "$SRC_DIR" "$AD_DIR" "$B_DIR" "$C_DIR"; do
        for lg in "$d"/daemon*.log; do
            [[ -f "$lg" ]] || continue
            printf -- '--- tail %s ---\n' "$lg" >&2
            tail -50 "$lg" >&2 || true
        done
    done
    KEEP_ON_FAIL=1
    exit 1
}

cleanup() {
    pkill -f "datadir=$WORK" 2>/dev/null || true
    sleep 1
    pkill -9 -f "datadir=$WORK" 2>/dev/null || true
    if [[ "$KEEP_ON_FAIL" != "1" ]]; then
        rm -rf "$WORK"
    else
        printf '[INFO] keeping work dir for debugging: %s\n' "$WORK" >&2
    fi
}
trap cleanup EXIT

command -v curl >/dev/null   || fail "curl is required"
command -v jq >/dev/null     || fail "jq is required"
command -v python3 >/dev/null || fail "python3 is required"
[[ -x "$DINEROD" ]] || fail "dinerod not executable at $DINEROD"

cookie_for() {  # <datadir>
    if [[ -f "$1/.cookie" ]]; then tr -d '\n' < "$1/.cookie"; return 0; fi
    if [[ -f "$1/regtest/.cookie" ]]; then tr -d '\n' < "$1/regtest/.cookie"; return 0; fi
    return 1
}

rpc() {  # <rpcport> <datadir> <method> [params-json]
    local port="$1" datadir="$2" method="$3" params="${4:-[]}"
    local cookie
    cookie="$(cookie_for "$datadir")" || return 1
    curl -fsS --max-time 30 --user "$cookie" \
        -H 'Content-Type: application/json' \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${port}/"
}

start_node() {  # <datadir> <rpcport> <p2pport> <wsport> <logfile> [extra args...]
    local datadir="$1" rpcport="$2" p2pport="$3" wsport="$4" logfile="$5"; shift 5
    mkdir -p "$datadir"
    "$DINEROD" --regtest --datadir="$datadir" \
        --rpcport="$rpcport" --port="$p2pport" --wallet-socket-port="$wsport" \
        --listen=1 "$@" > "$logfile" 2>&1 &
    local i
    for i in $(seq 1 90); do
        if rpc "$rpcport" "$datadir" getblockcount 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    fail "node on rpc port $rpcport ($datadir) did not become ready"
}

stop_node() {  # <datadir>  — wait for FULL process death (shutdown can take ~10s;
               # restarting on a live datadir lock kills the new instance)
    local datadir="$1" i
    pkill -f "datadir=$datadir" 2>/dev/null || true
    for i in $(seq 1 60); do
        pgrep -f "datadir=$datadir" >/dev/null 2>&1 || return 0
        sleep 0.5
    done
    pkill -9 -f "datadir=$datadir" 2>/dev/null || true
    sleep 1
}

snap_status() {  # <rpcport> <datadir> -> snapshot_bootstrap JSON object on stdout
    rpc "$1" "$2" getsnapshotbootstrapstatus 2>/dev/null \
        | jq -c '.result.snapshot_bootstrap // {}' 2>/dev/null || echo '{}'
}

wait_status() {  # <rpcport> <datadir> <jq-bool-expr over snapshot_bootstrap> <timeout_s> <desc>
    local port="$1" datadir="$2" expr="$3" timeout="$4" desc="$5" i st
    for i in $(seq 1 "$timeout"); do
        st="$(snap_status "$port" "$datadir")"
        if jq -e "$expr" <<<"$st" >/dev/null 2>&1; then
            pass "$desc"
            return 0
        fi
        sleep 1
    done
    printf '[FAIL] timeout (%ss) waiting for: %s\nlast status: %s\n' "$timeout" "$desc" \
        "$(snap_status "$port" "$datadir")" >&2
    fail "$desc"
}

deliver_bodies() {  # <dst_rpcport> <dst_datadir> <from_h> <to_h>
    # The daemon has no P2P backfill of pre-base bodies (see TOPOLOGY NOTES):
    # push the source's canonical blocks through the node's REAL block
    # acceptance path (submitblock -> BlockAcceptor -> flatfile storage).
    local port="$1" datadir="$2" from="$3" to="$4" h bh hex
    info "delivering bodies $from..$to via submitblock (no P2P pre-base backfill exists)"
    for h in $(seq "$from" "$to"); do
        bh="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$h]" | jq -re '.result')" \
            || fail "source getblockhash $h failed"
        hex="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$bh\",0]" | jq -re '.result')" \
            || fail "source getblock $h failed"
        rpc "$port" "$datadir" submitblock "[\"$hex\"]" >/dev/null \
            || fail "submitblock height $h failed"
    done
}

# ═════════════════════════════════════════════════════════════════════════
# Setup: source chain (101 mature blocks + shielded tx + 1 confirm), snapshot
# ═════════════════════════════════════════════════════════════════════════
info "=== Setup: source node mines chain, exports snapshot at tip ==="
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log"

rpc "$SRC_RPC" "$SRC_DIR" generate '[101]' | jq -e '.result.blocks | length == 101' >/dev/null \
    || fail "source failed to mine 101 blocks"

# Shielded tx below the base (spec SHOULD): replay must validate it through
# the engine's genesis-fresh shielded state. Soft requirement: record outcome.
SHIELD_RES="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.shield '[1.0, 10000]' || echo '{}')"
if jq -e '.result.txid // empty' <<<"$SHIELD_RES" >/dev/null 2>&1; then
    SHIELDED_NOTE="shield txid $(jq -r '.result.txid' <<<"$SHIELD_RES") below base"
    info "shielded tx created: $SHIELDED_NOTE"
else
    SHIELDED_NOTE="SKIPPED (wallet.shield returned: $(jq -c '.result.error // .error // "unknown"' <<<"$SHIELD_RES"))"
    info "shielded tx skipped: $SHIELDED_NOTE"
fi
rpc "$SRC_RPC" "$SRC_DIR" generate '[1]' >/dev/null || fail "confirm block failed"

BASE="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
info "source chain height (snapshot base): $BASE"

DUMP_RES="$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_written >= 1 and (.result.error == null or (.result | has("error") | not))' <<<"$DUMP_RES" >/dev/null \
    || fail "dumptxoutset failed: $DUMP_RES"
[[ -s "$SNAP" ]] || fail "snapshot not written"
pass "snapshot exported: $(jq -c '.result | {base_height, coins_written}' <<<"$DUMP_RES")"

# Consistent headers-store copy requires the source to be STOPPED.
stop_node "$SRC_DIR"
cp -R "$SRC_DIR/headers" "$HEADERS_AT_BASE"
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon2.log"

seed_headers() {  # <datadir> — production "headers-first" state, no bodies
    mkdir -p "$1"
    cp -R "$HEADERS_AT_BASE" "$1/headers"
}

# ═════════════════════════════════════════════════════════════════════════
# Scenario A+D: good snapshot — stall (15s knob), stall survives peerless
# restart, replay retires the trust marker, retirement survives restart
# ═════════════════════════════════════════════════════════════════════════
if [[ " $RUN_SCENARIOS " == *" AD "* ]]; then
info "=== Scenario A+D: good snapshot lifecycle with stall + recovery ==="
seed_headers "$AD_DIR"
start_node "$AD_DIR" "$AD_RPC" "$AD_P2P" "$AD_WS" "$AD_DIR/daemon.log" \
    --assumeutxo_bg_stall_timeout=15

LOAD_RES="$(rpc "$AD_RPC" "$AD_DIR" loadtxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_loaded >= 1' <<<"$LOAD_RES" >/dev/null \
    || fail "loadtxoutset (good snapshot) failed: $LOAD_RES"
pass "good snapshot loaded: $(jq -c '.result | {base_height, coins_loaded}' <<<"$LOAD_RES")"

# D1: peerless, bodies missing -> validation_stalled in ~15-45s. The default
# stall timeout is 1800s, so reaching stalled this fast PROVES the
# --assumeutxo_bg_stall_timeout=15 CLI flag reached config_->GetInt.
wait_status "$AD_RPC" "$AD_DIR" \
    '.history_validation_state == "validation_stalled" and .fatal == false and .history_fully_validated == false' \
    120 "D1: peerless validation stalls in ~15s (stall-timeout CLI knob delivered)"

# D2: restart still peerless -> stall persists (validation may flip to
# validating_history briefly while the worker re-validates genesis, but it
# must re-stall and must NOT silently promote to fully_validated).
stop_node "$AD_DIR"
start_node "$AD_DIR" "$AD_RPC" "$AD_P2P" "$AD_WS" "$AD_DIR/daemon2.log" \
    --assumeutxo_bg_stall_timeout=15 --assumeutxo_snapshot="$SNAP"
ST="$(snap_status "$AD_RPC" "$AD_DIR")"
jq -e '.history_fully_validated == false and .fatal == false' <<<"$ST" >/dev/null \
    || fail "D2: peerless restart must not promote a stalled lifecycle (got: $ST)"
wait_status "$AD_RPC" "$AD_DIR" \
    '.history_validation_state == "validation_stalled" and .fatal == false and .history_fully_validated == false' \
    120 "D2: stall persists across peerless restart"

# D3 + A: connect the source peer (headers/handshake machinery), deliver the
# pre-base bodies through real block acceptance, rebuild the canonical height
# index (--reindex), and let the worker's COMPLETE replay pass recover the
# persisted stall and retire the trust marker.
rpc "$AD_RPC" "$AD_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"add\"]" >/dev/null || true
rpc "$AD_RPC" "$AD_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"onetry\"]" >/dev/null || true
for i in $(seq 1 30); do
    CONNS="$(rpc "$AD_RPC" "$AD_DIR" getconnectioncount | jq -r '.result // 0')"
    [[ "$CONNS" -ge 1 ]] && break
    sleep 1
done
[[ "${CONNS:-0}" -ge 1 ]] || fail "nodeAD could not connect to the source peer"
pass "source peer connected (P2P handshake works; pre-base body backfill is RPC-driven, see header)"

deliver_bodies "$AD_RPC" "$AD_DIR" 1 "$BASE"

stop_node "$AD_DIR"
start_node "$AD_DIR" "$AD_RPC" "$AD_P2P" "$AD_WS" "$AD_DIR/daemon3.log" \
    --reindex --assumeutxo_bg_stall_timeout=15 --assumeutxo_snapshot="$SNAP"

wait_status "$AD_RPC" "$AD_DIR" \
    '.history_fully_validated == true and .assumeutxo_active == false and .fatal == false' \
    240 "A: real genesis->base replay retires the trust marker (fully_validated, assumeutxo_active=false)"

# A: retirement survives a plain restart (spec Persistence / Required Test 4).
stop_node "$AD_DIR"
start_node "$AD_DIR" "$AD_RPC" "$AD_P2P" "$AD_WS" "$AD_DIR/daemon4.log" \
    --assumeutxo_snapshot="$SNAP"
wait_status "$AD_RPC" "$AD_DIR" \
    '.history_fully_validated == true and .fatal == false' \
    60 "A: fully_validated survives restart"
stop_node "$AD_DIR"
fi

# ═════════════════════════════════════════════════════════════════════════
# Scenario B: poisoned snapshot (valid trailing checksum, wrong content)
# ═════════════════════════════════════════════════════════════════════════
if [[ " $RUN_SCENARIOS " == *" B "* ]]; then
info "=== Scenario B: poisoned snapshot -> fatal_mismatch; B2: operator reset ==="
python3 - "$SNAP" "$POISON" <<'PYEOF'
import hashlib, sys
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
body = data[:-32]
# Header is 68 bytes (magic4+version4+hash32+height4+count8+timestamp8+reserved8
# — verified against SnapshotMetadata write order in ExportSnapshot). First
# record: txid(32)+vout(4), so its value field starts at 68+32+4. Flip one
# value byte: file-level integrity is then REPAIRED below, simulating the
# spec's compromised-binary / poisoned-content case (load gates green,
# content commitment wrong).
poke = 68 + 32 + 4
body[poke] ^= 0xFF
checksum = hashlib.sha256(bytes(body)).digest()
open(dst, "wb").write(bytes(body) + checksum)
PYEOF
[[ -s "$POISON" ]] || fail "poisoned snapshot not written"

seed_headers "$B_DIR"
start_node "$B_DIR" "$B_RPC" "$B_P2P" "$B_WS" "$B_DIR/daemon.log" \
    --assumeutxo_bg_stall_timeout=15

LOAD_RES="$(rpc "$B_RPC" "$B_DIR" loadtxoutset "[\"$POISON\"]")"
jq -e '.result.coins_loaded >= 1' <<<"$LOAD_RES" >/dev/null \
    || fail "loadtxoutset MUST accept the poisoned snapshot (gates pass): $LOAD_RES"
pass "poisoned snapshot accepted at load time (no compiled-in regtest anchor; checksum repaired)"

deliver_bodies "$B_RPC" "$B_DIR" 1 "$BASE"

stop_node "$B_DIR"
start_node "$B_DIR" "$B_RPC" "$B_P2P" "$B_WS" "$B_DIR/daemon2.log" \
    --reindex --assumeutxo_bg_stall_timeout=15 --assumeutxo_snapshot="$POISON"

wait_status "$B_RPC" "$B_DIR" \
    '.fatal == true and .history_fully_validated == false and (.fatal_reason | contains("mismatch"))' \
    240 "B: poisoned snapshot drives fatal with mismatch reason after real replay"
info "fatal_reason: $(snap_status "$B_RPC" "$B_DIR" | jq -r '.fatal_reason')"

# Fatal survives restart (spec Required Test 1).
stop_node "$B_DIR"
start_node "$B_DIR" "$B_RPC" "$B_P2P" "$B_WS" "$B_DIR/daemon3.log" \
    --assumeutxo_snapshot="$POISON"
wait_status "$B_RPC" "$B_DIR" '.fatal == true' 60 "B: fatal survives restart"

# B2: explicit operator reset with confirm token clears fatal.
RESET_RES="$(rpc "$B_RPC" "$B_DIR" resetassumeutxofatal '[{"confirm":"RESET-ASSUMEUTXO-FATAL"}]')"
jq -e '.result.reset == true' <<<"$RESET_RES" >/dev/null \
    || fail "B2: resetassumeutxofatal did not report reset:true: $RESET_RES"
wait_status "$B_RPC" "$B_DIR" '.fatal == false' 30 "B2: operator reset clears fatal over RPC"
stop_node "$B_DIR"
fi

# ═════════════════════════════════════════════════════════════════════════
# Scenario C: loading a DIFFERENT snapshot mid-lifecycle is refused
# ═════════════════════════════════════════════════════════════════════════
if [[ " $RUN_SCENARIOS " == *" C "* ]]; then
info "=== Scenario C: different-base snapshot refused mid-lifecycle ==="
seed_headers "$C_DIR"
# Long stall timeout: scenario C asserts the refusal text, not stall behavior.
start_node "$C_DIR" "$C_RPC" "$C_P2P" "$C_WS" "$C_DIR/daemon.log" \
    --assumeutxo_bg_stall_timeout=3600

LOAD_RES="$(rpc "$C_RPC" "$C_DIR" loadtxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_loaded >= 1' <<<"$LOAD_RES" >/dev/null \
    || fail "scenario C initial loadtxoutset failed: $LOAD_RES"

# Different-base snapshot: source advances 5 blocks, exports again.
rpc "$SRC_RPC" "$SRC_DIR" generate '[5]' >/dev/null || fail "source could not mine +5"
DUMP2_RES="$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP2\"]")"
jq -e '.result.coins_written >= 1' <<<"$DUMP2_RES" >/dev/null || fail "second dumptxoutset failed: $DUMP2_RES"

# nodeC needs the NEW base header before C2's belt check is reachable
# (the base-block-known gate runs first at startup) — get it over real P2P.
# Post-base blocks connect on top of the snapshot tip, so blockcount reaches
# the new height once synced.
rpc "$C_RPC" "$C_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"add\"]" >/dev/null || true
rpc "$C_RPC" "$C_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"onetry\"]" >/dev/null || true
NEW_TIP=$((BASE + 5))
for i in $(seq 1 60); do
    H="$(rpc "$C_RPC" "$C_DIR" getblockcount | jq -r '.result // 0')"
    [[ "$H" -ge "$NEW_TIP" ]] && break
    sleep 1
done
[[ "${H:-0}" -ge "$NEW_TIP" ]] || fail "C: nodeC did not sync post-base blocks to height $NEW_TIP (got ${H:-0})"

# C1: live RPC path refuses a different-base load mid-lifecycle. The active
# snapshot's coins populate the consensus set, so LoadSnapshot's empty-set
# precondition fires before the header-aware belt — the belt message is
# unreachable over RPC by construction. Assert the call ERRORS either way.
LOAD2_RES="$(rpc "$C_RPC" "$C_DIR" loadtxoutset "[\"$SNAP2\"]" || echo '{}')"
ERR_MSG="$(jq -r '.result.error.message // .result.error // empty' <<<"$LOAD2_RES")"
if [[ -z "$ERR_MSG" ]] && jq -e '.result.coins_loaded >= 1' <<<"$LOAD2_RES" >/dev/null 2>&1; then
    fail "C1: second loadtxoutset with a DIFFERENT base was ACCEPTED mid-lifecycle: $LOAD2_RES"
fi
[[ "$ERR_MSG" == *"another snapshot lifecycle is active"* || \
   "$ERR_MSG" == *"must be empty to load snapshot"* ]] \
    || fail "C1: unexpected refusal shape for different-base mid-lifecycle load: $LOAD2_RES"
pass "C1: different-base load refused over RPC mid-lifecycle: $ERR_MSG"

# C2: the belt itself. Restart nodeC with a DIFFERENT-base snapshot configured:
# the startup rehydrate clears the stale consensus set (making the belt
# reachable), reads the new header, and must refuse with the belt message —
# and the daemon must NOT come up serving from it.
stop_node "$C_DIR"
mkdir -p "$C_DIR"
"$DINEROD" --regtest --datadir="$C_DIR" \
    --rpcport="$C_RPC" --port="$C_P2P" --wallet-socket-port="$C_WS" \
    --listen=1 --assumeutxo_bg_stall_timeout=3600 \
    --assumeutxo_snapshot="$SNAP2" > "$C_DIR/daemon2.log" 2>&1 &
C2_OK=0
for i in $(seq 1 90); do
    if grep -q "another snapshot lifecycle is active" "$C_DIR/daemon2.log" 2>/dev/null; then
        C2_OK=1
        break
    fi
    sleep 1
done
[[ "$C2_OK" == "1" ]] \
    || fail "C2: belt message 'another snapshot lifecycle is active' never appeared on different-base startup rehydrate"
pass "C2: belt refusal fired on startup rehydrate: $(grep -m1 -o 'another snapshot lifecycle is active[^"]*' "$C_DIR/daemon2.log" | head -1)"
# The node must not have come up fully_validated/healthy on the wrong snapshot.
if rpc "$C_RPC" "$C_DIR" getsnapshotbootstrapstatus 2>/dev/null \
        | jq -e '.result.snapshot_bootstrap.history_fully_validated == true' >/dev/null 2>&1; then
    fail "C2: node reached fully_validated after a refused different-base rehydrate"
fi
stop_node "$C_DIR"
fi

info "shielded-tx coverage: $SHIELDED_NOTE"
echo "ALL SCENARIOS PASSED"
