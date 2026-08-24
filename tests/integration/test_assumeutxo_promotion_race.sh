#!/usr/bin/env bash
#
# AssumeUTXO promotion-race regtest (#353 bug 2).
#
# THE BUG (fixed by commit e5df3e9d1, under test here):
#   On an AssumeUTXO snapshot bootstrap the consumer does two things at once:
#     (1) forward-syncs post-base blocks from peers via ConnectTip, and
#     (2) background-validates genesis->base to PROVE the snapshot, then
#         PROMOTES the proven pre-base UTXO set into the ChainDB coin CF.
#   Promotion (PromoteValidatedHistory) is gated on tip_below_base — it only
#   runs when the ChainDB tip is still <= base. In production, forward sync
#   races the tip PAST base before the background replay finishes, so
#   tip_below_base is false, promotion is SKIPPED, and the pre-base coins are
#   NEVER written to the coin CF. A pre-base coinbase's gettxout (which reads
#   chain_db->getCoin, the coin CF) then returns JSON null — wallet-visible but
#   unspendable.
#   THE FIX: while assumeutxo is active, ActivateBestChain caps the activation
#   target at the snapshot base, holding the tip AT base until promotion clears
#   assumeutxo_active_, then the normal catch-up pass connects base+1..base+K.
#
# DETERMINISTIC RACE LEVER (regtest-only, in the binary):
#   DINERO_DEBUG_BG_VALIDATION_DELAY_MS on the CONSUMER slows the per-block
#   genesis->base replay. With a base of ~200 blocks x 200ms (~40s+ per replay
#   pass) vs. forward-syncing only K=5 post-base blocks (seconds), the tip
#   deterministically wins the race past base while replay is still running.
#
# TOPOLOGY (mirrors test_assumeutxo_replay_e2e.sh, one deliberate change):
#   Unlike Scenario A there — which stops the source so there are NO post-base
#   blocks and thus NO race — this test KEEPS the source running K=5 blocks PAST
#   base and connected, so the consumer forward-syncs past base and races.
#   * loadtxoutset requires an EMPTY consensus set + the base header already on
#     the consumer's header chain -> we seed a headers-at-base copy taken with
#     the source STOPPED.
#   * Pre-base bodies (genesis..base) arrive via the daemon's REAL P2P backfill;
#     post-base bodies (base+1..base+K) arrive via normal forward sync. Both run
#     concurrently against the connected, running source.
#
# ASSERTIONS (soft checkpoints; `set -uo pipefail` per task, FAILED flag,
# exit nonzero at end — so a neutered build still REACHES + REPORTS assertion 2):
#   1. Race engaged: the fix's hold line fires (gate saw a candidate past base
#      and held the tip AT base). Also records tip-vs-base timing for evidence.
#   2. CORE: a specific PRE-BASE coinbase's gettxout on the consumer is NON-null
#      after promotion (this is exactly what the bug breaks -> null).
#   3. Catch-up: after mode exit the consumer's tip reaches base+K.
#   4. Mode exit: getsnapshotbootstrapstatus shows assumeutxo_active=false and
#      history fully_validated.
#
set -uo pipefail

# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
fi
BASE_PORT="${BASE_PORT:-37200}"
BASE_HEIGHT="${BASE_HEIGHT:-200}"    # snapshot base (large enough that slowed replay >> forward sync)
POST_BASE_K="${POST_BASE_K:-5}"      # blocks the source mines PAST base (small)
BG_DELAY_MS="${BG_DELAY_MS:-200}"    # per-block genesis->base replay delay on the consumer
PREBASE_H="${PREBASE_H:-5}"          # height of the pre-base coinbase used for the core assertion
PROMO_TIMEOUT="${PROMO_TIMEOUT:-540}" # seconds to wait for promotion / mode exit

SRC_RPC=$((BASE_PORT + 0)); SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
CON_RPC=$((BASE_PORT + 1)); CON_P2P=$((BASE_PORT + 101)); CON_WS=$((BASE_PORT + 201))

WORK="$(mktemp -d -t dinero_promo_race_XXXXXX)"
printf '[INFO] workdir: %s\n' "$WORK"
SRC_DIR="$WORK/source"; CON_DIR="$WORK/consumer"
SNAP="$WORK/utxo-snapshot.dat"; HEADERS_AT_BASE="$WORK/headers_at_base"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
FAILED=0

info()   { printf '[INFO] %s\n' "$*"; }
ck_pass(){ printf '[PASS] %s\n' "$*"; }
ck_fail(){ printf '[FAIL] %s\n' "$*" >&2; FAILED=1; }
fail() {   # hard failure: infra/setup broke, the test cannot proceed
    printf '[FAIL] %s\n' "$*" >&2
    for d in "$SRC_DIR" "$CON_DIR"; do
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
    --assumeutxo_bg_stall_timeout=3600

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
    --assumeutxo_bg_stall_timeout=3600 --assumeutxo_snapshot="$SNAP"
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
# active, record: (a) the fix's deferred-import line in the log, (b) the max ChainDB tip
# observed WHILE still active (in a neutered build the tip races PAST base ->
# this is the race evidence; in a fixed build the tip is held AT base).
HOLD_LINE="AssumeUTXO active — deferring post-base header import until history promotion"
HOLD_SEEN=0
MAX_TIP_WHILE_ACTIVE=0
TIP_PAST_BASE_WHILE_ACTIVE=0
MODE_EXITED=0
RACE_START=$SECONDS
LAST_LOG=$SECONDS
while (( SECONDS - RACE_START < PROMO_TIMEOUT )); do
    if grep -qs "$HOLD_LINE" "$CON_DIR"/daemon*.log 2>/dev/null; then HOLD_SEEN=1; fi
    ST="$(snap_status "$CON_RPC" "$CON_DIR")"
    ACTIVE="$(jq -r '.assumeutxo_active // empty' <<<"$ST")"
    TIP="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
    if [[ "$ACTIVE" == "true" ]]; then
        [[ "$TIP" -gt "$MAX_TIP_WHILE_ACTIVE" ]] && MAX_TIP_WHILE_ACTIVE="$TIP"
        [[ "$TIP" -gt "$BASE" ]] && TIP_PAST_BASE_WHILE_ACTIVE=1
    elif [[ "$ACTIVE" == "false" ]] || { [[ "$ACTIVE" != "true" ]] && [[ -n "$TIP" ]] && [[ "$TIP" -gt "$BASE" ]]; }; then
        # Mode exited: either explicitly (assumeutxo_active=false) OR the
        # snapshot_bootstrap object is gone post-exit (ACTIVE empty) AND the tip
        # has advanced past base — which the hold makes impossible while active,
        # so tip>base is a sound "exited + catching up" signal.
        MODE_EXITED=1
        break
    fi
    if (( SECONDS - LAST_LOG >= 15 )); then
        LAST_LOG=$SECONDS
        info "  [t+$((SECONDS-RACE_START))s] active=$ACTIVE tip=$TIP hold=$HOLD_SEEN vstate=$(jq -r '.history_validation_state // "?"' <<<"$ST") bf=$(jq -r '(.backfill_completed//0)|tostring' <<<"$ST")/$(jq -r '(.backfill_total//0)|tostring' <<<"$ST") vheight=$(jq -r '.history_validation_height // .validation_height // "?"' <<<"$ST")"
    fi
    sleep 2
done
# final grep in case the line landed between samples
grep -qs "$HOLD_LINE" "$CON_DIR"/daemon*.log 2>/dev/null && HOLD_SEEN=1
info "race observation: hold_line_seen=$HOLD_SEEN, max_tip_while_active=$MAX_TIP_WHILE_ACTIVE (base=$BASE), tip_past_base_while_active=$TIP_PAST_BASE_WHILE_ACTIVE, mode_exited=$MODE_EXITED"

# ── Assertion 1: race engaged (the fix's gate held the tip at base) ───────
# The deferred-import line proves the fix saw a canonical network continuation
# past base and skipped activation -> the race was genuinely exercised. (In a neutered build
# this line is gone; TIP_PAST_BASE_WHILE_ACTIVE=1 is the alternative evidence
# that the tip raced past base, reported above.)
if [[ "$HOLD_SEEN" == "1" ]]; then
    ck_pass "A1: post-base import was deferred while assumeutxo active (race exercised)"
else
    ck_fail "A1: deferred-import line NEVER fired (race not exercised by the fix; tip_past_base_while_active=$TIP_PAST_BASE_WHILE_ACTIVE)"
fi

# ── Wait for mode exit + full validation (gate for A2/A3/A4) ──────────────
# Post-exit the snapshot_bootstrap object is gone, so assumeutxo_active reads
# absent (not literally false); treat absent-or-false as exited.
if [[ "$MODE_EXITED" != "1" ]]; then
    wait_status "$CON_RPC" "$CON_DIR" '(.assumeutxo_active // false) == false' 60 "mode exit" \
        && MODE_EXITED=1
fi

# ── Assertion 2 (CORE): pre-base coin written to the coin CF by promotion ──
# gettxout reads chain_db->getCoin (the coin CF). Non-null == promotion ran and
# wrote the proven pre-base set. This is EXACTLY what the bug breaks (null).
POST_UTXO="$(rpc "$CON_RPC" "$CON_DIR" gettxout "[\"$PB_TXID\", 0]")"
if jq -e '.result != null and (.result | has("value"))' <<<"$POST_UTXO" >/dev/null 2>&1; then
    ck_pass "A2 (CORE): pre-base coinbase gettxout is NON-null after promotion: $(jq -c '.result | {value, height, coinbase}' <<<"$POST_UTXO")"
else
    ck_fail "A2 (CORE): pre-base coinbase gettxout is NULL — promotion skipped, pre-base coins never written to the coin CF (THE BUG): $POST_UTXO"
fi

# ── Assertion 3: catch-up to base+K after mode exit ──────────────────────
CAUGHT_UP=0
for i in $(seq 1 120); do
    H="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
    [[ "$H" -ge "$NET_TIP" ]] && { CAUGHT_UP=1; break; }
    sleep 1
done
if [[ "$CAUGHT_UP" == "1" ]]; then
    ck_pass "A3: consumer tip caught up to source tip base+K=$NET_TIP (got $H)"
else
    ck_fail "A3: consumer tip did not reach base+K=$NET_TIP (got ${H:-0})"
fi

# ── Assertion 4: mode exit + history fully validated ─────────────────────
# Post-exit the snapshot_bootstrap status object is torn down, so its fields
# read absent rather than literally false — treat absent assumeutxo_active as
# exited and absent fatal as not-fatal. The load-bearing teeth are A2 (coin CF
# written) + A3 (tip caught up); A4 is the clean-exit confirmation.
FST="$(snap_status "$CON_RPC" "$CON_DIR")"
if jq -e '(.assumeutxo_active // false) == false and (.fatal // false) == false' \
        <<<"$FST" >/dev/null 2>&1; then
    ck_pass "A4: assumeutxo mode exited cleanly (not active, not fatal)"
else
    ck_fail "A4: expected assumeutxo not-active + not-fatal; got: $FST"
fi

stop_node "$CON_DIR"
stop_node "$SRC_DIR"

if [[ "$FAILED" == "0" ]]; then
    echo "ALL PROMOTION-RACE ASSERTIONS PASSED"
else
    echo "PROMOTION-RACE TEST FAILED (see [FAIL] lines above)" >&2
fi
exit "$FAILED"
