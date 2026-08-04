#!/usr/bin/env bash
# test_assumeutxo_forward_connect.sh — the MOBILE PROFILE contract:
# with assumeutxo_forward_connect=1, a snapshot-bootstrapped node connects
# blocks FORWARD from the base immediately (usable at the live tip while the
# genesis->base background validation still runs), and promotion then lands in
# ADVANCED-TIP mode: coin CF reconciled against the LIVE consensus set,
# tip-anchored markers left to ConnectTip, completion recorded via a durable
# marker instead of setTip(base) — so the tip is never held and never regressed.
#
# Topology mirrors test_assumeutxo_promotion_race.sh (source mines to base,
# exports snapshot, keeps mining +K past base and stays connected; consumer
# loads the snapshot with a slowed genesis->base replay so forward sync wins
# the race). The assertions are the FORWARD-CONNECT contract:
#   F1: the #361 hold line NEVER fires; the forward-connect line DOES
#   F2 (the payoff): tip advances PAST base while assumeutxo is still active
#   F3 (CORE):  pre-base coinbase gettxout NON-null after mode exit
#               (advanced promotion materialized the coin CF, no tip rewind)
#   F3b:        post-base coinbase gettxout NON-null after mode exit
#               (the live-set reconcile did NOT delete post-base coins —
#                the reverse hazard of #353 bug 2)
#   F4: final tip >= base+K and >= the max tip ever observed (no regression)
#   F5: clean exit (not active, not fatal) + advanced-tip promotion log
#
# CORRUPTION_RECOVERY_MODE=1 reuses the same V4 snapshot topology for #369.
# It forks one stopped mismatched-checkpoint datadir into recovery-with-file
# and fail-closed-without-file legs, then compares exact persisted state.
set -uo pipefail

DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
BASE_PORT="${BASE_PORT:-37200}"
BASE_HEIGHT="${BASE_HEIGHT:-200}"    # snapshot base (large enough that slowed replay >> forward sync)
POST_BASE_K="${POST_BASE_K:-5}"      # blocks the source mines PAST base (small)
BG_DELAY_MS="${BG_DELAY_MS:-200}"    # per-block genesis->base replay delay on the consumer
PREBASE_H="${PREBASE_H:-5}"          # height of the pre-base coinbase used for the core assertion
PROMO_TIMEOUT="${PROMO_TIMEOUT:-540}" # seconds to wait for promotion / mode exit
CORRUPTION_RECOVERY_MODE="${CORRUPTION_RECOVERY_MODE:-0}"
MUTATOR="${MUTATOR:-}"
CHECKPOINT_INTERVAL="${CHECKPOINT_INTERVAL:-500}"

SRC_RPC=$((BASE_PORT + 0)); SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
CON_RPC=$((BASE_PORT + 1)); CON_P2P=$((BASE_PORT + 101)); CON_WS=$((BASE_PORT + 201))
CTL_RPC=$((BASE_PORT + 2)); CTL_P2P=$((BASE_PORT + 102)); CTL_WS=$((BASE_PORT + 202))

WORK="$(mktemp -d -t dinero_fwd_connect_XXXXXX)"
printf '[INFO] workdir: %s\n' "$WORK"
SRC_DIR="$WORK/source"; CON_DIR="$WORK/consumer"; CTL_DIR="$WORK/control-no-snapshot"
SNAP="$WORK/utxo-snapshot.dat"; HEADERS_AT_BASE="$WORK/headers_at_base"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
FAILED=0

info()   { printf '[INFO] %s\n' "$*"; }
ck_pass(){ printf '[PASS] %s\n' "$*"; }
ck_fail(){ printf '[FAIL] %s\n' "$*" >&2; FAILED=1; }
fail() {   # hard failure: infra/setup broke, the test cannot proceed
    printf '[FAIL] %s\n' "$*" >&2
    for d in "$SRC_DIR" "$CON_DIR" "$CTL_DIR"; do
        for lg in "$d"/daemon*.log; do
            [[ -f "$lg" ]] || continue
            printf -- '--- tail %s ---\n' "$lg" >&2
            tail -60 "$lg" >&2 || true
        done
    done
    KEEP_ON_FAIL=1
    exit 1
}

cleanup() {
    pkill -f "datadir=$WORK" 2>/dev/null || true
    sleep 1
    pkill -9 -f "datadir=$WORK" 2>/dev/null || true
    if [[ "$KEEP_ON_FAIL" != "1" ]]; then rm -rf "$WORK"
    else printf '[INFO] keeping work dir for debugging: %s\n' "$WORK" >&2; fi
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null   || fail "jq is required"
[[ -x "$DINEROD" ]] || fail "dinerod not executable at $DINEROD"
if [[ "$CORRUPTION_RECOVERY_MODE" == "1" ]]; then
    [[ -x "$MUTATOR" ]] || fail "recovery mode requires executable MUTATOR"
fi

cookie_for() {
    if [[ -f "$1/.cookie" ]]; then tr -d '\n' < "$1/.cookie"; return 0; fi
    if [[ -f "$1/regtest/.cookie" ]]; then tr -d '\n' < "$1/regtest/.cookie"; return 0; fi
    return 1
}

rpc() {  # <rpcport> <datadir> <method> [params-json]
    local port="$1" datadir="$2" method="$3" params="${4:-[]}" cookie
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

stop_node() {  # <datadir> — wait for FULL process death
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
        if jq -e "$expr" <<<"$st" >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    printf '[INFO] wait_status timeout (%ss) for: %s\n[INFO] last status: %s\n' \
        "$timeout" "$desc" "$(snap_status "$port" "$datadir")" >&2
    return 1
}

rpc_result() {
    local response
    response="$(rpc "$1" "$2" "$3" "${4:-[]}")" || return 1
    jq -e '.error == null and has("result")' <<<"$response" >/dev/null 2>&1 || return 1
    jq -c '.result' <<<"$response"
}

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
    else sha256sum "$1" | awk '{print $1}'; fi
}

capture_consensus_state() {
    local port="$1" datadir="$2" label="$3" dump
    dump="$WORK/${label}.forest"
    rpc_result "$port" "$datadir" utreexo.dumpforestinternal "[\"$dump\"]" >/dev/null \
        || fail "could not dump forest for $label"
    [[ -s "$dump" ]] || fail "forest dump for $label is empty"
    jq -n -c \
        --argjson height "$(rpc_result "$port" "$datadir" getblockcount)" \
        --arg tip "$(rpc_result "$port" "$datadir" getbestblockhash | jq -r '.')" \
        --argjson txoutset "$(rpc_result "$port" "$datadir" blockchain.gettxoutsetinfo)" \
        --argjson commitment "$(rpc_result "$port" "$datadir" blockchain.getutreexocommitment)" \
        --argjson roots "$(rpc_result "$port" "$datadir" blockchain.getutreexoroots)" \
        --arg forest_sha256 "$(sha256_file "$dump")" \
        '{height:$height,tip:$tip,txoutset:$txoutset,commitment:$commitment,roots:$roots,forest_sha256:$forest_sha256}'
}

assert_same_consensus_state() {
    if [[ "$(jq -S -c . <<<"$1")" == "$(jq -S -c . <<<"$2")" ]]; then
        ck_pass "$3: exact tip, UTXO set, commitment, roots, and forest dump"
    else
        printf '[INFO] expected state: %s\n[INFO] actual state:   %s\n' "$1" "$2" >&2
        ck_fail "$3: consensus state differs"
    fi
}

prepare_checkpoint_mismatch() {
    local h=0 active=false mutation
    info "=== #369: freeze V4 snapshot-active state past base ==="
    for _ in $(seq 1 60); do
        h="$(rpc_result "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null || echo 0)"
        active="$(snap_status "$CON_RPC" "$CON_DIR" | jq -r '.assumeutxo_active // false')"
        [[ "$h" -ge "$NET_TIP" && "$active" == "true" ]] && break
        sleep 1
    done
    [[ "$h" -ge "$NET_TIP" ]] || fail "consumer did not reach post-base tip before checkpoint edit"
    [[ "$active" == "true" ]] || fail "snapshot lifecycle exited before checkpoint edit"

    stop_node "$SRC_DIR"
    CLEAN_STATE="$(capture_consensus_state "$CON_RPC" "$CON_DIR" clean-pre-edit)"
    info "clean state: $CLEAN_STATE"
    stop_node "$CON_DIR"

    local chaindb="$CON_DIR/blockchain/chaindb"
    [[ -d "$chaindb" ]] || fail "consumer ChainDB missing: $chaindb"
    mutation="$("$MUTATOR" "$chaindb" --source-offset-back 1)" \
        || fail "failed to install a distinct valid checkpoint payload"
    printf '%s\n' "$mutation"
    cp -R "$CON_DIR" "$CTL_DIR"
    ck_pass "stopped datadir forked into byte-identical recovery and control legs"
}

recover_with_snapshot_file() {
    local h=0 recovered stable
    info "=== #369 R1: offline restart with trusted V4 snapshot path ==="
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon-recovery.log" \
        --p2p.offline=1 --listen=0 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
        --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    for _ in $(seq 1 180); do
        h="$(rpc_result "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null || echo 0)"
        [[ "$h" -ge "$NET_TIP" ]] && break
        sleep 1
    done
    [[ "$h" -ge "$NET_TIP" ]] || fail "snapshot-path recovery did not reach stored post-base tip"
    [[ "$(rpc_result "$CON_RPC" "$CON_DIR" getconnectioncount)" == "0" ]] \
        || fail "snapshot-path recovery was not offline"
    recovered="$(capture_consensus_state "$CON_RPC" "$CON_DIR" recovered)"
    assert_same_consensus_state "$CLEAN_STATE" "$recovered" "R1 first offline restart"
    if grep -qs "FOREST ROOT MISMATCH" "$CON_DIR/daemon-recovery.log"; then
        ck_pass "R1 detected the valid-payload/wrong-root checkpoint"
    else
        ck_fail "R1 did not log the injected root mismatch"
    fi
    if grep -qs "AUTO-RECOVERING: wiping corrupt forest checkpoints" "$CON_DIR/daemon-recovery.log"; then
        ck_pass "R1 reset the mismatched checkpoint set"
    else
        ck_fail "R1 did not log checkpoint reset"
    fi
    if grep -qs "Snapshot rehydrated from file" "$CON_DIR/daemon-recovery.log"; then
        ck_pass "R1 rehydrated the trusted V4 snapshot"
    else
        ck_fail "R1 did not rehydrate the trusted V4 snapshot"
    fi
    grep -qs "REBUILD LOOP DETECTED" "$CON_DIR/daemon-recovery.log" \
        && ck_fail "R1 entered a rebuild loop"

    stop_node "$CON_DIR"
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon-stable.log" \
        --p2p.offline=1 --listen=0 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
        --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    [[ "$(rpc_result "$CON_RPC" "$CON_DIR" getconnectioncount)" == "0" ]] \
        || fail "stable restart was not offline"
    stable="$(capture_consensus_state "$CON_RPC" "$CON_DIR" stable-second-restart)"
    assert_same_consensus_state "$recovered" "$stable" "R1 second offline restart"
    grep -qs "FOREST ROOT MISMATCH\|REBUILD LOOP DETECTED" "$CON_DIR/daemon-stable.log" \
        && ck_fail "R1 stable restart repeated recovery"
    stop_node "$CON_DIR"
}

reject_without_snapshot_file() {
    local pid ready=0 rc=0 inspect count
    info "=== #369 R2: identical stopped state without snapshot path ==="
    "$DINEROD" --regtest --datadir="$CTL_DIR" --rpcport="$CTL_RPC" \
        --port="$CTL_P2P" --wallet-socket-port="$CTL_WS" --p2p.offline=1 --listen=0 \
        --assumeutxo_bg_stall_timeout=3600 --assumeutxo_forward_connect=1 \
        --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL" \
        > "$CTL_DIR/daemon-no-snapshot.log" 2>&1 &
    pid=$!
    for _ in $(seq 1 120); do
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        if rpc "$CTL_RPC" "$CTL_DIR" getblockcount 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 0.5
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    else
        wait "$pid" || rc=$?
    fi

    [[ "$ready" == "0" ]] || ck_fail "R2 served RPC despite missing required snapshot path"
    [[ "$rc" != "0" ]] || ck_fail "R2 exited success after refusing chainstate initialization"
    grep -qs "no assumeutxo_snapshot path is configured" "$CTL_DIR/daemon-no-snapshot.log" \
        || ck_fail "R2 did not fail for the expected missing-snapshot reason"
    grep -qs "FOREST ROOT MISMATCH" "$CTL_DIR/daemon-no-snapshot.log" \
        || ck_fail "R2 did not reach the injected mismatch"
    inspect="$("$MUTATOR" "$CTL_DIR/blockchain/chaindb" --inspect)" \
        || fail "could not inspect control-leg checkpoints"
    printf '%s\n' "$inspect"
    count="$(awk -F= '$1=="checkpoint_count" {print $2}' <<<"$inspect")"
    if [[ "$ready" == "0" && "$rc" != "0" && "$count" == "0" ]] && \
       grep -qs "no assumeutxo_snapshot path is configured" "$CTL_DIR/daemon-no-snapshot.log" && \
       grep -qs "FOREST ROOT MISMATCH" "$CTL_DIR/daemon-no-snapshot.log"; then
        ck_pass "R2 refused startup and left no replacement checkpoint"
    else
        [[ "$count" == "0" ]] || ck_fail "R2 wrote a replacement checkpoint after refusal"
    fi
}

run_checkpoint_recovery() {
    prepare_checkpoint_mismatch
    recover_with_snapshot_file
    reject_without_snapshot_file
    stop_node "$SRC_DIR"
    if [[ "$FAILED" == "0" ]]; then
        echo "ALL #369 V4 CHECKPOINT/RESTART ASSERTIONS PASSED"
    else
        echo "#369 V4 CHECKPOINT/RESTART TEST FAILED" >&2
    fi
    exit "$FAILED"
}

# ═════════════════════════════════════════════════════════════════════════
# Setup: source mines to base, exports snapshot, then keeps mining K PAST base
# ═════════════════════════════════════════════════════════════════════════
info "=== Setup: source mines to base=$BASE_HEIGHT, exports snapshot, mines +$POST_BASE_K past base ==="
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log"

rpc "$SRC_RPC" "$SRC_DIR" generate "[$BASE_HEIGHT]" \
    | jq -e ".result.blocks | length == $BASE_HEIGHT" >/dev/null \
    || fail "source failed to mine $BASE_HEIGHT base blocks"

BASE="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
[[ "$BASE" -eq "$BASE_HEIGHT" ]] || fail "source height $BASE != expected base $BASE_HEIGHT"
info "source at base height $BASE"

# Pick a specific PRE-BASE coinbase outpoint for the core assertion, and prove
# it is UNSPENT on the source (no wallet spends in this test, so early
# coinbases are unspent by construction — but verify anyway).
PB_HASH="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$PREBASE_H]" | jq -r '.result')"
[[ "$PB_HASH" =~ ^[0-9a-fA-F]{64}$ ]] || fail "could not get block hash at pre-base height $PREBASE_H"
PB_TXID="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$PB_HASH\", 1]" \
    | jq -r '.result.tx[0] | if type=="string" then . else .txid end')"
[[ "$PB_TXID" =~ ^[0-9a-fA-F]{64}$ ]] || fail "could not get coinbase txid at height $PREBASE_H (got: $PB_TXID)"
SRC_UTXO="$(rpc "$SRC_RPC" "$SRC_DIR" gettxout "[\"$PB_TXID\", 0]")"
jq -e '.result != null and (.result | has("value"))' <<<"$SRC_UTXO" >/dev/null \
    || fail "chosen pre-base coinbase $PB_TXID:0 (h=$PREBASE_H) is not unspent on the source: $SRC_UTXO"
info "pre-base coinbase outpoint for core assertion: ${PB_TXID:0:16}...:0 (height $PREBASE_H, unspent on source)"

# Export the snapshot at base.
DUMP_RES="$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_written >= 1' <<<"$DUMP_RES" >/dev/null \
    || fail "dumptxoutset failed: $DUMP_RES"
[[ -s "$SNAP" ]] || fail "snapshot not written"
info "snapshot exported: $(jq -c '.result | {base_height, coins_written}' <<<"$DUMP_RES")"

# Consistent headers-at-base copy requires the source STOPPED.
stop_node "$SRC_DIR"
cp -R "$SRC_DIR/headers" "$HEADERS_AT_BASE"
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon2.log"

# THE DELIBERATE CHANGE vs Scenario A: keep mining K blocks PAST base and leave
# the source running + connectable, so the consumer forward-syncs past base.
rpc "$SRC_RPC" "$SRC_DIR" generate "[$POST_BASE_K]" \
    | jq -e ".result.blocks | length == $POST_BASE_K" >/dev/null \
    || fail "source failed to mine +$POST_BASE_K past base"
SRC_TIP="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
NET_TIP=$((BASE + POST_BASE_K))
[[ "$SRC_TIP" -eq "$NET_TIP" ]] || fail "source tip $SRC_TIP != base+K $NET_TIP"
info "source now at $SRC_TIP (base $BASE + K $POST_BASE_K), running + connectable"

# ═════════════════════════════════════════════════════════════════════════
# Consumer: seed headers-at-base, start WITH the replay delay, load snapshot,
# connect the running source -> forward sync races the tip past base while the
# slowed genesis->base replay is still running.
# ═════════════════════════════════════════════════════════════════════════
info "=== Consumer: load snapshot with slowed replay (${BG_DELAY_MS}ms/block), then race ==="
mkdir -p "$CON_DIR"
cp -R "$HEADERS_AT_BASE" "$CON_DIR/headers"

# Long stall timeout so the peerless->stall path never trips before backfill
# heals it; the delay is what slows replay, not the stall knob. Env delay only
# on the consumer (source already started without it).
export DINERO_DEBUG_BG_VALIDATION_DELAY_MS="$BG_DELAY_MS"
start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon.log" \
    --assumeutxo_bg_stall_timeout=3600 --assumeutxo_forward_connect=1 \
    --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"

LOAD_RES="$(rpc "$CON_RPC" "$CON_DIR" loadtxoutset "[\"$SNAP\"]")"
jq -e '.result.coins_loaded >= 1' <<<"$LOAD_RES" >/dev/null \
    || fail "loadtxoutset failed: $LOAD_RES"
info "snapshot loaded on consumer: $(jq -c '.result | {base_height, coins_loaded}' <<<"$LOAD_RES")"

# Sanity: before promotion the pre-base coin must be gettxout-NULL on the
# consumer (it lives only in the snapshot accumulator, not the coin CF). This
# documents the starting state the race+promotion must resolve.
PRE_UTXO="$(rpc "$CON_RPC" "$CON_DIR" gettxout "[\"$PB_TXID\", 0]")"
if jq -e '.result == null' <<<"$PRE_UTXO" >/dev/null 2>&1; then
    info "pre-promotion: consumer gettxout($PREBASE_H coinbase) is null (accumulator-only, expected)"
else
    info "pre-promotion: consumer gettxout already non-null: $(jq -c '.result' <<<"$PRE_UTXO")"
fi

# Restart the consumer with --assumeutxo_snapshot to REHYDRATE the snapshot
# lifecycle (mirrors Scenario A: its retirement-via-backfill happens in a
# restarted process). This is the proven flow through which P2P-backfilled
# pre-base bodies become readable by the background replay worker; a live
# RPC-loaded session that never restarts livelocks (bodies stored but the
# worker's RequireFlatfiles read misses them). Delay env stays exported.
stop_node "$CON_DIR"
start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon2.log" \
    --assumeutxo_bg_stall_timeout=3600 --assumeutxo_snapshot="$SNAP" \
    --assumeutxo_forward_connect=1 --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
wait_status "$CON_RPC" "$CON_DIR" '.assumeutxo_active == true' 60 "snapshot rehydrated after restart" \
    || fail "consumer did not rehydrate the snapshot lifecycle after restart"
info "consumer restarted with --assumeutxo_snapshot (lifecycle rehydrated, active)"

# Connect the running source: forward sync (base+1..base+K) + P2P backfill
# (genesis..base) now run concurrently; the slowed replay lets the tip race.
rpc "$CON_RPC" "$CON_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"add\"]" >/dev/null || true
rpc "$CON_RPC" "$CON_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"onetry\"]" >/dev/null || true
CONN_OK=0
for i in $(seq 1 30); do
    c="$(rpc "$CON_RPC" "$CON_DIR" getconnectioncount | jq -r '.result // 0')"
    [[ "$c" -ge 1 ]] && { CONN_OK=1; break; }
    sleep 1
done
[[ "$CONN_OK" == "1" ]] || fail "consumer could not connect to the running source peer"
info "consumer connected to source — race window open"

# ── Race + promotion observation loop ────────────────────────────────────
# Poll until assumeutxo_active flips false (promotion done) or timeout. While
# active, record: (a) the fix's hold line in the log, (b) the max ChainDB tip
# observed WHILE still active (in a neutered build the tip races PAST base ->
# this is the race evidence; in a fixed build the tip is held AT base).
HOLD_LINE="AssumeUTXO active — holding tip at snapshot base"
DEFER_LINE="AssumeUTXO active — deferring post-base header import until history promotion"
FWD_LINE="forward-connect profile: connecting past snapshot base"
ADV_PROMO_LINE="[Promotion] complete (advanced-tip)"
HOLD_SEEN=0
FWD_SEEN=0
MAX_TIP_OBSERVED=0
TIP_PAST_BASE_WHILE_ACTIVE=0
MODE_EXITED=0
RACE_START=$SECONDS
LAST_LOG=$SECONDS
while (( SECONDS - RACE_START < PROMO_TIMEOUT )); do
    { grep -qs "$HOLD_LINE" "$CON_DIR"/daemon*.log 2>/dev/null ||
      grep -qs "$DEFER_LINE" "$CON_DIR"/daemon*.log 2>/dev/null; } && HOLD_SEEN=1
    grep -qs "$FWD_LINE"  "$CON_DIR"/daemon*.log 2>/dev/null && FWD_SEEN=1
    ST="$(snap_status "$CON_RPC" "$CON_DIR")"
    ACTIVE="$(jq -r '.assumeutxo_active // empty' <<<"$ST")"
    TIP="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
    [[ "$TIP" -gt "$MAX_TIP_OBSERVED" ]] && MAX_TIP_OBSERVED="$TIP"
    if [[ "$ACTIVE" == "true" ]]; then
        [[ "$TIP" -gt "$BASE" ]] && TIP_PAST_BASE_WHILE_ACTIVE=1
        if [[ "$CORRUPTION_RECOVERY_MODE" == "1" && "$TIP" -ge "$NET_TIP" ]]; then
            break
        fi
    elif [[ "$ACTIVE" == "false" ]]; then
        MODE_EXITED=1
        break
    elif [[ -z "$ACTIVE" ]] && [[ "$TIP_PAST_BASE_WHILE_ACTIVE" == "1" ]]; then
        # status object torn down post-exit
        MODE_EXITED=1
        break
    fi
    if (( SECONDS - LAST_LOG >= 15 )); then
        LAST_LOG=$SECONDS
        info "  [t+$((SECONDS-RACE_START))s] active=$ACTIVE tip=$TIP fwd=$FWD_SEEN hold=$HOLD_SEEN vheight=$(jq -r '.history_validation_height // .validation_height // "?"' <<<"$ST")"
    fi
    sleep 2
done
{ grep -qs "$HOLD_LINE" "$CON_DIR"/daemon*.log 2>/dev/null ||
  grep -qs "$DEFER_LINE" "$CON_DIR"/daemon*.log 2>/dev/null; } && HOLD_SEEN=1
grep -qs "$FWD_LINE"  "$CON_DIR"/daemon*.log 2>/dev/null && FWD_SEEN=1
info "observation: fwd_seen=$FWD_SEEN hold_seen=$HOLD_SEEN tip_past_base_while_active=$TIP_PAST_BASE_WHILE_ACTIVE max_tip=$MAX_TIP_OBSERVED mode_exited=$MODE_EXITED"

# ── F1: no hold; forward-connect engaged ─────────────────────────────────
if [[ "$HOLD_SEEN" == "0" ]]; then
    ck_pass "F1a: neither hold nor deferred-import gate fired (forward-connect bypassed the cap)"
else
    ck_fail "F1a: a hold/deferred-import gate fired despite assumeutxo_forward_connect=1"
fi
if [[ "$FWD_SEEN" == "1" ]]; then
    ck_pass "F1b: forward-connect log line observed"
else
    ck_fail "F1b: forward-connect log line never observed"
fi

# ── F2 (THE PAYOFF): tip past base WHILE assumeutxo still active ─────────
if [[ "$TIP_PAST_BASE_WHILE_ACTIVE" == "1" ]]; then
    ck_pass "F2: tip advanced past base while background validation was still running (mobile UX: usable at the live tip)"
else
    ck_fail "F2: tip never passed base while active — forward-connect did not engage"
fi

if [[ "$CORRUPTION_RECOVERY_MODE" == "1" ]]; then
    run_checkpoint_recovery
fi

# ── Wait for mode exit ────────────────────────────────────────────────────
if [[ "$MODE_EXITED" != "1" ]]; then
    wait_status "$CON_RPC" "$CON_DIR" '(.assumeutxo_active // false) == false' 120 "mode exit" \
        && MODE_EXITED=1
fi
[[ "$MODE_EXITED" == "1" ]] || ck_fail "mode did not exit within timeout"

# ── F3 (CORE): pre-base coin materialized by ADVANCED promotion ──────────
POST_UTXO="$(rpc "$CON_RPC" "$CON_DIR" gettxout "[\"$PB_TXID\", 0]")"
if jq -e '.result != null and (.result | has("value"))' <<<"$POST_UTXO" >/dev/null 2>&1; then
    ck_pass "F3 (CORE): pre-base coinbase gettxout NON-null after advanced-tip promotion: $(jq -c '.result | {value, height, coinbase}' <<<"$POST_UTXO")"
else
    ck_fail "F3 (CORE): pre-base coinbase gettxout NULL — advanced promotion did not materialize the coin CF: $POST_UTXO"
fi

# ── F3b: post-base coin SURVIVED the live-set reconcile ───────────────────
PB2_HASH="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$((BASE + 1))]" | jq -r '.result')"
PB2_TXID="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$PB2_HASH\", 1]" \
    | jq -r '.result.tx[0] | if type=="string" then . else .txid end')"
POST2_UTXO="$(rpc "$CON_RPC" "$CON_DIR" gettxout "[\"$PB2_TXID\", 0]")"
if jq -e '.result != null and (.result | has("value"))' <<<"$POST2_UTXO" >/dev/null 2>&1; then
    ck_pass "F3b: post-base coinbase (h=$((BASE+1))) gettxout NON-null — live-set reconcile preserved post-base coins"
else
    ck_fail "F3b: post-base coinbase gettxout NULL — the reconcile deleted post-base coins (reverse #353 hazard): $POST2_UTXO"
fi

# ── F4: tip at network tip, never regressed ───────────────────────────────
CAUGHT_UP=0
for i in $(seq 1 120); do
    H="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
    [[ "$H" -ge "$NET_TIP" ]] && { CAUGHT_UP=1; break; }
    sleep 1
done
if [[ "$CAUGHT_UP" == "1" ]] && [[ "$H" -ge "$MAX_TIP_OBSERVED" ]]; then
    ck_pass "F4: final tip $H >= network tip $NET_TIP and >= max observed $MAX_TIP_OBSERVED (no setTip(base) regression)"
else
    ck_fail "F4: final tip ${H:-0} (network tip $NET_TIP, max observed $MAX_TIP_OBSERVED)"
fi

# ── F5: clean exit + advanced-tip promotion actually ran ──────────────────
FST="$(snap_status "$CON_RPC" "$CON_DIR")"
if jq -e '(.assumeutxo_active // false) == false and (.fatal // false) == false' \
        <<<"$FST" >/dev/null 2>&1; then
    ck_pass "F5a: assumeutxo mode exited cleanly (not active, not fatal)"
else
    ck_fail "F5a: expected not-active + not-fatal; got: $FST"
fi
if grep -qsF "$ADV_PROMO_LINE" "$CON_DIR"/daemon*.log 2>/dev/null; then
    ck_pass "F5b: advanced-tip promotion completion logged"
else
    # Legit alternative: replay finished before any forward connect -> classic
    # promotion ran (tip was still below base). Accept if classic completed.
    if grep -qsF "[Promotion] complete: ChainDB tip now at base" "$CON_DIR"/daemon*.log 2>/dev/null; then
        ck_pass "F5b: classic promotion ran (replay won the race) — acceptable, gate covered by race variant"
    else
        ck_fail "F5b: no promotion completion line found"
    fi
fi

stop_node "$CON_DIR"
stop_node "$SRC_DIR"

if [[ "$FAILED" == "0" ]]; then
    echo "ALL FORWARD-CONNECT ASSERTIONS PASSED"
else
    echo "FORWARD-CONNECT TEST FAILED (see [FAIL] lines above)" >&2
fi
exit "$FAILED"
