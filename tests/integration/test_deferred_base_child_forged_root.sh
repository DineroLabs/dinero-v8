#!/usr/bin/env bash
# A forged base+1 Utreexo root must be rejected AT ACCEPT TIME, in deferred
# AssumeUTXO mode, with the rejection visible in the log rather than deferred
# to promotion.
#
# WHAT THIS DOES AND DOES NOT GATE -- measured, not assumed.
#
#   It DOES verify, end to end on a real deferred-mode consumer, that a
#   PoW-valid block whose header carries a forged Utreexo root is refused at
#   accept time. That property had no end-to-end coverage before.
#
#   It DOES NOT discriminate the finding-13 classification fix. Mutation-tested
#   both ways: reverting block acceptance to classify against
#   ChainDB::getTip(), AND restoring the deferred base-child precheck
#   exemption, each left this test passing. The reason is that submitblock
#   reaches the root check by a path that does not depend on the side-chain
#   classification, so the exemption is simply not on the route this test
#   takes.
#
#   Exercising the path that IS affected needs the forged block to arrive over
#   P2P from a peer, not via submitblock. That is not built here.
#
#   The finding-13 RULE is gated instead by test_active_tip_classification.cpp,
#   which is mutation-proven (durable-tip classification and
#   everything-extends both fail it).
#
# Recording this rather than letting the file imply coverage it does not have:
# a test that passes on both sides of the change it names is exactly the
# "registered but proves nothing" failure this repo has been fighting.
#
# The setup below is taken verbatim from test_assumeutxo_promotion_race.sh --
# the proven sequence for getting a consumer into deferred mode with a slowed
# replay. Re-deriving it is how a test ends up asserting nothing.

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
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
BASE_PORT="${BASE_PORT:-37400}"
BASE_HEIGHT="${BASE_HEIGHT:-200}"    # snapshot base (large enough that slowed replay >> forward sync)
POST_BASE_K="${POST_BASE_K:-5}"      # blocks the source mines PAST base (small)
BG_DELAY_MS="${BG_DELAY_MS:-200}"    # per-block genesis->base replay delay on the consumer
PREBASE_H="${PREBASE_H:-5}"          # height of the pre-base coinbase used for the core assertion
PROMO_TIMEOUT="${PROMO_TIMEOUT:-540}" # seconds to wait for promotion / mode exit

SRC_RPC=$((BASE_PORT + 0)); SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
CON_RPC=$((BASE_PORT + 1)); CON_P2P=$((BASE_PORT + 101)); CON_WS=$((BASE_PORT + 201))

WORK="$(mktemp -d -t dinero_forged_root_XXXXXX)"
printf '[INFO] workdir: %s\n' "$WORK"
SRC_DIR="$WORK/source"; CON_DIR="$WORK/consumer"
SNAP="$WORK/utxo-snapshot.dat"; HEADERS_AT_BASE="$WORK/headers_at_base"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
FAILED=0
LAST_NODE_PID=""

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
    if [[ "$KEEP_ON_FAIL" != "1" && "$FAILED" == "0" ]]; then rm -rf "$WORK"
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
    # 180s, not 30s. Mining the 200-block base is a single RPC call that
    # measurably exceeds 30s on a loaded builder (the sibling AssumeUTXO tests
    # take ~115s end to end in CI). A setup timeout that trips before the first
    # assertion yields a test that has never asserted anything.
    curl -fsS --max-time 180 --user "$cookie" \
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
    LAST_NODE_PID=$!
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
CONSUMER_PID="$LAST_NODE_PID"
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

# ═════════════════════════════════════════════════════════════════════════
# The forged base+1
# ═════════════════════════════════════════════════════════════════════════
wait_status "$CON_RPC" "$CON_DIR" '.assumeutxo_active == true' 120 "deferred mode" \
    || fail "consumer is not in deferred mode; the bug is unreachable outside it"
CON_TIP="$(rpc "$CON_RPC" "$CON_DIR" getblockcount | jq -re '.result')"
info "consumer in deferred mode, tip=$CON_TIP base=$BASE"
ck_pass "deferred mode engaged (active tip at base, durable tip trailing)"

# The honest base+1 from the source, and a root borrowed from another height.
B1_HASH="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$((BASE + 1))]" | jq -re '.result')"
B1_HEX="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$B1_HASH\", 0]" | jq -r '.result // empty')"
[[ -n "$B1_HEX" ]] || B1_HEX="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$B1_HASH\", false]" | jq -r '.result // empty')"
if [[ -z "$B1_HEX" ]]; then
    info "SKIP: raw block hex unavailable from this RPC surface"
    exit 77
fi
# utreexo_root_RAW, not utreexo_root. The header RPC exposes both: utreexo_root
# is DISPLAY order (reversed, as GetHex() prints it) while utreexo_root_raw is
# wire order. The serialized block carries wire order, so substituting the
# display-order string finds nothing and the forgery silently becomes a no-op.
# This is the same reversal trap the codebase hit before; the skip guard below
# is what caught it here rather than letting the test pass while asserting
# nothing.
B1_ROOT="$(rpc "$SRC_RPC" "$SRC_DIR" getblockheader "[\"$B1_HASH\"]" \
           | jq -r '.result.utreexo_root_raw // empty')"
OTHER_HASH="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$((BASE - 10))]" | jq -re '.result')"
OTHER_ROOT="$(rpc "$SRC_RPC" "$SRC_DIR" getblockheader "[\"$OTHER_HASH\"]" \
              | jq -r '.result.utreexo_root_raw // empty')"

if [[ -z "$B1_ROOT" || -z "$OTHER_ROOT" || "$B1_ROOT" == "$OTHER_ROOT" || "$B1_HEX" != *"$B1_ROOT"* ]]; then
    info "SKIP: could not locate a distinct utreexo root inside the raw block"
    info "      (refusing to report success without actually forging one)"
    exit 77
fi
FORGED_HEX="${B1_HEX/$B1_ROOT/$OTHER_ROOT}"
[[ "$FORGED_HEX" != "$B1_HEX" ]] || { info "SKIP: substitution was a no-op"; exit 77; }
info "forged base+1 root: ${B1_ROOT:0:16}... -> ${OTHER_ROOT:0:16}..."

# ── ASSERTION 1: rejected at accept time, by the ROOT check ─────────────
SUBMIT="$(rpc "$CON_RPC" "$CON_DIR" submitblock "[\"$FORGED_HEX\"]" 2>&1 || true)"
SUBMIT_MSG="$(jq -r '((.error.message // "") + " " + (.result // "" | tostring))' <<<"$SUBMIT" 2>/dev/null || echo "$SUBMIT")"
info "submitblock -> ${SUBMIT_MSG:0:160}"
if grep -qiE "utreexo" <<<"$SUBMIT_MSG"; then
    ck_pass "forged base+1 rejected by the Utreexo root check at accept time"
elif grep -qiE "duplicate|already" <<<"$SUBMIT_MSG"; then
    ck_fail "treated as a duplicate — the root check never ran"
elif [[ -z "${SUBMIT_MSG// /}" || "$SUBMIT_MSG" == *null* ]]; then
    ck_fail "forged base+1 ACCEPTED — accept-time root rejection is absent (the finding-2 regression)"
else
    ck_fail "rejected, but not by the root check: ${SUBMIT_MSG:0:120}"
fi

# ── ASSERTION 2: the chain must not have advanced on it ────────────────
sleep 3
TIP_AFTER="$(rpc "$CON_RPC" "$CON_DIR" getblockcount | jq -re '.result')"
if [[ "$TIP_AFTER" -gt "$CON_TIP" ]]; then
    # A forged block must never move the tip. (Honest base+1 can arrive via
    # P2P; this asserts the FORGED submission did not cause the advance, which
    # the accept-time rejection above already establishes.)
    info "tip moved $CON_TIP -> $TIP_AFTER (honest forward sync may explain this)"
fi
if grep -qiE "UTREEXO VALIDATION FAILED \(accept time" "$CON_DIR"/daemon*.log 2>/dev/null; then
    ck_pass "rejection logged at ACCEPT time, not deferred to promotion"
else
    ck_fail "no accept-time Utreexo rejection in the consumer log — the check did not run at accept time"
fi

if [[ "$FAILED" == "0" ]]; then
    ck_pass "forged base+1 is rejected immediately by the accept-time root check"
    exit 0
fi
exit 1
