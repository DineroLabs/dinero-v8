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
#   F6: an owned pre-base coin is wallet-spendable after promotion; the wallet
#       selects real pre-base inputs, signs and relays the spend, and the source
#       confirms it in a block
#
# CORRUPTION_RECOVERY_MODE=1 reuses the same V4 snapshot topology for #369.
# It forks one stopped mismatched-checkpoint datadir into recovery-with-file
# and fail-closed-without-file legs, then compares exact persisted state.
#
# PREBASE_SPEND_MODE=1 is the block-90391 regression (STATELESS undo path). A
# CSN consumer forward-connects a post-base block that spends a pre-base coin;
# the CSN undo reconstruction must recover that coin from the durable frozen
# pre-base store (it is absent from the ordinary UTXO CF). Self-contained
# branch: source-signed pre-base spend + stateless consumer; asserts the
# fidelity log (height + coinbase flag). EXPECT_PREBASE_STALL=1 flips it to the
# neuter expectation (un-fixed binary must stall with undo-spent-reconstruction-
# failed on the exact outpoint).
set -uo pipefail

DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
BASE_PORT="${BASE_PORT:-37200}"
BASE_HEIGHT="${BASE_HEIGHT:-200}"    # snapshot base (large enough that slowed replay >> forward sync)
POST_BASE_K="${POST_BASE_K:-5}"      # blocks the source mines PAST base (small)
BG_DELAY_MS="${BG_DELAY_MS:-200}"    # per-block genesis->base replay delay on the consumer
PREBASE_H="${PREBASE_H:-5}"          # height of the pre-base coinbase used for the core assertion
PROMO_TIMEOUT="${PROMO_TIMEOUT:-540}" # seconds to wait for promotion / mode exit
CORRUPTION_RECOVERY_MODE="${CORRUPTION_RECOVERY_MODE:-0}"
# Sub-mode of CORRUPTION_RECOVERY_MODE: additionally inject a reorg_in_progress
# marker so the restart hits the ChainstateService Init incomplete-reorg branch
# (a crash mid-reorg), which for a snapshot node must AUTO-RECOVER via clean
# re-bootstrap (wipe forest + clear utxo CF + re-arm) instead of aborting.
# Assertion (b) of the recovery pair: the node RECONVERGES to the exact clean
# consensus state (assert_same_consensus_state below) — proving the re-arm
# LANDED and rebuilt, not merely emptied. NOTE: this is the injected-marker
# path (real stale-checkpoint + real reorg marker), not a live divergent-tip
# reorg constructed in-harness; the divergent-tip orphan-row removal is covered
# by the ChainDB::clearAllCoins unit test (assertion a).
INCOMPLETE_REORG_MODE="${INCOMPLETE_REORG_MODE:-0}"
# PREBASE_SPEND_MODE=1 is the regression for the block-90391 stall: a STATELESS
# (ios_utreexo / CSN) consumer forward-connects a post-base block that SPENDS a
# pre-base coin. That coin is absent from the ordinary consumer ChainDB coin CF,
# so the CSN undo path's frozen pre-base resolver must recover it; otherwise
# ReconstructSpentCoinsFromChainDb misses it and, without the fix, aborts the
# connection with undo-spent-reconstruction-failed. With the fix, the spent coin
# is reconstructed from the frozen pre-base store and the node crosses the block.
PREBASE_SPEND_MODE="${PREBASE_SPEND_MODE:-0}"
SRC_PREBASE_H="${SRC_PREBASE_H:-3}"   # height of the SOURCE-owned pre-base COINBASE spent post-export
EXPECT_PREBASE_STALL="${EXPECT_PREBASE_STALL:-0}"  # neuter expectation: assert the un-fixed stall instead
# PREBASE_MEMPOOL_MODE=1 is the mempool/BROADCAST twin of the 90391 fix: a
# STATELESS (ios_utreexo / CSN) node cannot BROADCAST a spend of a genuinely-live
# PRE-BASE coin — the mempool's coin-map/ChainDB views never hold pre-base coins,
# so Mempool::validateTransaction rejects the input with "Input UTXO not found"
# even though the coin is live in the frozen pre-base store and Utreexo forest. The fix
# (ChainstateService::ResolveLivePreBaseCoin, wired into the mempool)
# admits it ONLY on a frozen-store RESOLVE AND a POSITIVE live-forest leaf-present
# AUTHORIZE. Asserts: (A) live pre-base coin -> input resolves (not "Input UTXO
# not found"); (B) pre-base coin SPENT post-base (forest leaf gone) -> REJECTED
# (the double-spend gate). EXPECT_PREBASE_MEMPOOL_NEUTER=1 is the neuter
# expectation (fix reverted -> A itself rejects with "Input UTXO not found").
PREBASE_MEMPOOL_MODE="${PREBASE_MEMPOOL_MODE:-0}"
EXPECT_PREBASE_MEMPOOL_NEUTER="${EXPECT_PREBASE_MEMPOOL_NEUTER:-0}"
# SNAPSHOT_ROTATION_SELFHEAL_MODE=1: a stateless node has a persisted ACTIVE
# AssumeUTXO lifecycle pinned to a base whose snapshot has been ROTATED AWAY
# (only a newer snapshot remains configured). Without the fix the node hard-fails
# to start Chainstate ("no matching configured snapshot candidate"); with the fix
# it self-heals — clean re-bootstrap from the available snapshot, shielded marker
# re-established, no SAFE MODE. EXPECT_SELFHEAL_FATAL=1 is the neuter expectation.
SNAPSHOT_ROTATION_SELFHEAL_MODE="${SNAPSHOT_ROTATION_SELFHEAL_MODE:-0}"
SELFHEAL_H_GONE="${SELFHEAL_H_GONE:-40}"   # base of the rotated-away snapshot (persisted lifecycle pin)
SELFHEAL_H_NEW="${SELFHEAL_H_NEW:-80}"     # base of the still-available (newer) snapshot
EXPECT_SELFHEAL_FATAL="${EXPECT_SELFHEAL_FATAL:-0}"  # neuter: assert the un-fixed hard-fail instead
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

    if [[ "$INCOMPLETE_REORG_MODE" == "1" ]]; then
        # Inject the crash-mid-reorg marker into the SQLite UTXOIndex of the
        # RECOVERY leg ONLY (after the fork, so the control leg stays a clean
        # #369 topology). The restart then enters the incomplete-reorg branch
        # (marker present + forest not restorable at the staled-checkpoint tip).
        # Both stores are now dirty on the recovery leg: RocksDB utreexo CF
        # (staled checkpoint, via the mutator above) + SQLite UTXOIndex (this
        # marker). Recovery must reconcile both.
        command -v sqlite3 >/dev/null || fail "sqlite3 required for INCOMPLETE_REORG_MODE"
        local utxo_db="$CON_DIR/blockchain/utxo"
        [[ -f "$utxo_db" ]] || fail "UTXOIndex sqlite db missing at $utxo_db"
        sqlite3 "$utxo_db" \
            "INSERT OR REPLACE INTO utxo_metadata(key, value) VALUES ('reorg_in_progress', '${NET_TIP}:${NET_TIP}:${NET_TIP}');"
        [[ "$(sqlite3 "$utxo_db" "SELECT value FROM utxo_metadata WHERE key='reorg_in_progress' LIMIT 1;")" == "${NET_TIP}:${NET_TIP}:${NET_TIP}" ]] \
            || fail "failed to inject reorg_in_progress marker"
        ck_pass "injected reorg_in_progress marker into recovery leg (both stores dirty)"
    fi
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
    # Assertion (b): the recovered node RECONVERGES to the clean consensus state
    # — proving the re-arm LANDED and forward-connect rebuilt, not merely emptied
    # the CF. An over-clear-no-rebuild regression fails this.
    if [[ "$INCOMPLETE_REORG_MODE" == "1" ]]; then
        # The incomplete-reorg recovery clears the whole utxo CF, which also
        # resets background validation's genesis->base re-materialization. That
        # leaves the bg-validation-dependent txoutset stats transiently behind
        # until bg-validation redoes the pre-base range — an accepted cost of the
        # clean re-bootstrap. All CONSENSUS-CRITICAL state (tip, best hash,
        # Utreexo commitment, Utreexo roots, AND the full forest dump) must still
        # reconverge exactly; only txoutset is excluded from the compare.
        local clean_core recovered_core
        clean_core="$(jq -S -c 'del(.txoutset)' <<<"$CLEAN_STATE")"
        recovered_core="$(jq -S -c 'del(.txoutset)' <<<"$recovered")"
        if [[ "$clean_core" == "$recovered_core" ]]; then
            ck_pass "R1 reconverged consensus-critical state (tip, commitment, roots, forest dump)"
        else
            printf '[INFO] clean core:     %s\n[INFO] recovered core: %s\n' "$clean_core" "$recovered_core" >&2
            ck_fail "R1 consensus-critical state differs after incomplete-reorg recovery"
        fi
    else
        assert_same_consensus_state "$CLEAN_STATE" "$recovered" "R1 first offline restart"
    fi

    if [[ "$INCOMPLETE_REORG_MODE" == "1" ]]; then
        # Incomplete-reorg branch: the marker + staled checkpoint make Init
        # enter the crash-mid-reorg recovery (it fires early and wipes the
        # checkpoints, so the #369 forest-root path is bypassed).
        if grep -qs "Incomplete reorg detected from previous shutdown — AUTO-RECOVERING" "$CON_DIR/daemon-recovery.log"; then
            ck_pass "R1 auto-recovered from the incomplete-reorg marker (no abort)"
        else
            ck_fail "R1 did not enter the incomplete-reorg auto-recovery"
        fi
        # Proves Init actually invoked ChainDB::clearAllCoins (the utxo-CF reset
        # that removes orphan divergent-tip rows) — ties assertion (a) to Init.
        if grep -qs "Cleared .* utxo CF coin rows" "$CON_DIR/daemon-recovery.log"; then
            ck_pass "R1 cleared the utxo CF during recovery (clearAllCoins invoked)"
        else
            ck_fail "R1 did not clear the utxo CF (clearAllCoins not invoked)"
        fi
        # The SQLite store is reconciled: the reorg marker is cleared (only
        # after the wipe), so a subsequent clean start does not re-enter.
        if [[ -z "$(sqlite3 "$CON_DIR/blockchain/utxo" "SELECT value FROM utxo_metadata WHERE key='reorg_in_progress' LIMIT 1;")" ]]; then
            ck_pass "R1 cleared the reorg_in_progress marker (SQLite store reconciled)"
        else
            ck_fail "R1 left the reorg_in_progress marker set"
        fi
        grep -qs "Startup is aborted" "$CON_DIR/daemon-recovery.log" \
            && ck_fail "R1 aborted instead of auto-recovering"
        grep -qs "refusing to wipe again (recovery loop)" "$CON_DIR/daemon-recovery.log" \
            && ck_fail "R1 entered an incomplete-reorg recovery loop"
    else
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
# ═════════════════════════════════════════════════════════════════════════════
# PREBASE_SPEND_MODE — block-90391 regression (stateless CSN undo path).
#
# A STATELESS consumer (utreexo-stateless=1 => ios_utreexo profile, the phone's
# mode) forward-connects a post-base block that SPENDS a pre-base COINBASE. On a
# stateless node ConnectBlock leaves the undo spent list empty, so ConnectTip
# calls ReconstructSpentCoinsFromChainDb. The spent coin predates the snapshot
# base, so it lives ONLY in the in-memory AssumeUTXO coin map (BulkLoad) and is
# absent from the consumer's ChainDB coin CF. Without the fix, reconstruction
# fails -> undo-spent-reconstruction-failed -> the node stalls at base (exactly
# the phone's 90391 brick). With the fix, GetActiveUTXO recovers it from the
# frozen pre-base store and the node crosses the block.
#
# Self-contained: the SOURCE owns and signs the spend (a stateless consumer runs
# gen=0 and cannot wallet-sign). Separate SRC_PREBASE_H leaves the other modes'
# PREBASE_H assertions untouched.
# ═════════════════════════════════════════════════════════════════════════════
if [[ "$PREBASE_SPEND_MODE" == "1" ]]; then
    info "=== PREBASE_SPEND_MODE: stateless CSN undo reconstructs a pre-base spent coin (90391 regression) ==="
    [[ "$SRC_PREBASE_H" -ge 1 && "$SRC_PREBASE_H" -lt "$BASE_HEIGHT" ]] \
        || fail "SRC_PREBASE_H=$SRC_PREBASE_H must be in [1, BASE_HEIGHT)"

    # --- source: full/bridge node; owns every coin; serves utreexo proofs ---
    start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log" \
        --utreexo-bridge=1
    SRC_ADDR="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getnewaddress '[]' \
        | jq -r '.result.address // .result // empty')"
    [[ -n "$SRC_ADDR" ]] || fail "source wallet.getnewaddress returned no address"

    # Mine the whole base to the SOURCE's own address so a pre-base COINBASE is
    # source-spendable. base >> coinbase maturity(=10 regtest), so SRC_PREBASE_H
    # is deeply mature by the time we spend it.
    rpc "$SRC_RPC" "$SRC_DIR" generatetoaddress "[$BASE_HEIGHT,\"$SRC_ADDR\"]" \
        | jq -e ".result.blocks | length == $BASE_HEIGHT" >/dev/null \
        || fail "source failed to mine $BASE_HEIGHT base blocks"
    BASE="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
    [[ "$BASE" -eq "$BASE_HEIGHT" ]] || fail "source height $BASE != base $BASE_HEIGHT"

    # Export snapshot at base. Every source coin is an UNSPENT pre-base coinbase
    # (no spends yet), so all land in the snapshot's in-memory coin map — none in
    # the (consumer's) ChainDB coin CF. SRC_PREBASE_H is advisory documentation;
    # the actually-spent pre-base outpoint is derived from the spend below so the
    # fidelity assertion checks the coin the wallet really selected.
    jq -e '.result.coins_written >= 1' \
        <<<"$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP\"]")" >/dev/null \
        || fail "dumptxoutset failed"
    [[ -s "$SNAP" ]] || fail "snapshot not written"

    # Consistent headers-at-base copy requires the source STOPPED.
    stop_node "$SRC_DIR"
    cp -R "$SRC_DIR/headers" "$HEADERS_AT_BASE"
    start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon2.log" \
        --utreexo-bridge=1

    # POST-EXPORT: spend via the proven wallet.sendtoaddress path. Because every
    # spendable source coin is a pre-base coinbase, the wallet necessarily selects
    # a pre-base coinbase input — faithful to 90391 (its post-base block spends a
    # pre-base output). Send to mempool FIRST, inspect the selected input while it
    # is still unconfirmed (coin still unspent -> source gettxout resolves it),
    # then confirm in base+1.
    DEST="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getnewaddress '[]' \
        | jq -r '.result.address // .result // empty')"
    [[ -n "$DEST" ]] || fail "source wallet.getnewaddress (dest) failed"
    SEND_RES="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.sendtoaddress "[\"$DEST\",50.0]")"
    SPEND_TXID="$(jq -r '.result.txid // .result // empty' <<<"$SEND_RES")"
    [[ "$SPEND_TXID" =~ ^[0-9a-fA-F]{64}$ ]] \
        || fail "wallet.sendtoaddress could not spend a pre-base coinbase: $SEND_RES"

    # Identify the pre-base coinbase the wallet spent (height + coinbase flag),
    # inspecting each input against the source's authoritative UTXO state.
    SPEND_VIN="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getrawtransaction "[\"$SPEND_TXID\",true]" \
        | jq -c '.result.vin // []')"
    jq -e 'length > 0' <<<"$SPEND_VIN" >/dev/null || fail "spend tx has no decoded inputs: $SPEND_VIN"
    SPB_TXID=""; SPB_VOUT=""; SPB_HEIGHT=""; SPB_CB=""
    while IFS=$'\t' read -r in_txid in_vout; do
        [[ -n "$in_txid" && -n "$in_vout" ]] || continue
        COIN="$(rpc "$SRC_RPC" "$SRC_DIR" gettxout "[\"$in_txid\",$in_vout,false]")"
        H="$(jq -r '.result.height // -1' <<<"$COIN")"
        CB="$(jq -r 'if (.result.coinbase // false) then 1 else 0 end' <<<"$COIN")"
        if [[ "$H" -ge 1 && "$H" -le "$BASE" ]]; then
            SPB_TXID="$in_txid"; SPB_VOUT="$in_vout"; SPB_HEIGHT="$H"; SPB_CB="$CB"
            break
        fi
    done < <(jq -r '.[] | [.txid, (.vout|tostring)] | @tsv' <<<"$SPEND_VIN")
    [[ -n "$SPB_TXID" ]] || fail "spend selected no pre-base input (all inputs post-base?): $SPEND_VIN"
    [[ "$SPB_CB" == "1" ]] || fail "expected the spent pre-base input to be a coinbase; got coinbase=$SPB_CB (h=$SPB_HEIGHT)"
    info "wallet spent pre-base coinbase ${SPB_TXID:0:16}...:$SPB_VOUT (height $SPB_HEIGHT, coinbase=$SPB_CB)"

    rpc "$SRC_RPC" "$SRC_DIR" generate '[1]' \
        | jq -e '.result.blocks | length == 1' >/dev/null \
        || fail "source failed to mine the spend block (base+1)"
    SB1_HASH="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$((BASE + 1))]" | jq -r '.result')"
    jq -e --arg t "$SPEND_TXID" \
        '.result.tx | map(if type=="string" then . else .txid end) | index($t) != null' \
        <<<"$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$SB1_HASH\",1]")" >/dev/null \
        || fail "spend tx $SPEND_TXID not included in base+1"
    info "pre-base coinbase spent in base+1 (${SB1_HASH:0:16}...), spend tx ${SPEND_TXID:0:16}..."
    if [[ "$POST_BASE_K" -gt 1 ]]; then
        rpc "$SRC_RPC" "$SRC_DIR" generate "[$((POST_BASE_K - 1))]" \
            | jq -e ".result.blocks | length == $((POST_BASE_K - 1))" >/dev/null \
            || fail "source failed to extend past base+1"
    fi
    NET_TIP=$((BASE + POST_BASE_K))
    SRC_TIP="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
    [[ "$SRC_TIP" -eq "$NET_TIP" ]] || fail "source tip $SRC_TIP != base+K $NET_TIP"
    info "source at $SRC_TIP (base $BASE + K $POST_BASE_K), bridge running + connectable"

    # --- consumer: STATELESS (ios_utreexo — the phone's mode), forward-connect ---
    mkdir -p "$CON_DIR/headers"
    cp -R "$HEADERS_AT_BASE/." "$CON_DIR/headers/"
    export DINERO_DEBUG_BG_VALIDATION_DELAY_MS="$BG_DELAY_MS"
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon.log" \
        --utreexo-stateless=1 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_forward_connect=1 --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    # Confirm the consumer really resolved to the STATELESS profile — otherwise
    # ConnectBlock populates the spent list, ReconstructSpentCoins is a no-op, and
    # this whole test greens vacuously (the reconstruction path never runs). The
    # daemon prints an authoritative "mode=STATELESS|STATEFUL" line at startup.
    # (utreexo-bridge defaults on even for a stateless node, so "bridge mode
    # ENABLED" is NOT evidence either way — mode= is the determinant.)
    STATELESS_OK=0
    for i in $(seq 1 20); do
        if grep -qsE "Sync profile:.*mode=STATELESS" "$CON_DIR/daemon.log" 2>/dev/null; then
            STATELESS_OK=1; break
        fi
        grep -qsE "Sync profile:.*mode=STATEFUL" "$CON_DIR/daemon.log" 2>/dev/null \
            && fail "consumer resolved to STATEFUL mode despite --utreexo-stateless=1 — reconstruction path would not run"
        sleep 1
    done
    [[ "$STATELESS_OK" == "1" ]] \
        || fail "could not confirm consumer resolved to stateless mode (no 'mode=STATELESS' line) — test would be vacuous"

    LOAD_RES="$(rpc "$CON_RPC" "$CON_DIR" loadtxoutset "[\"$SNAP\"]")"
    jq -e '.result.coins_loaded >= 1' <<<"$LOAD_RES" >/dev/null \
        || fail "loadtxoutset failed: $LOAD_RES"
    info "snapshot loaded on stateless consumer: $(jq -c '.result | {base_height, coins_loaded}' <<<"$LOAD_RES")"

    # Rehydrate the snapshot lifecycle via restart (same reason as the main flow:
    # a live RPC-loaded session that never restarts livelocks the backfill worker).
    stop_node "$CON_DIR"
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon2.log" \
        --utreexo-stateless=1 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
        --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    wait_status "$CON_RPC" "$CON_DIR" '.assumeutxo_active == true' 60 "stateless snapshot rehydrated" \
        || fail "stateless consumer did not rehydrate the snapshot lifecycle"

    rpc "$CON_RPC" "$CON_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"add\"]" >/dev/null || true
    rpc "$CON_RPC" "$CON_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"onetry\"]" >/dev/null || true
    CONN_OK=0
    for i in $(seq 1 30); do
        c="$(rpc "$CON_RPC" "$CON_DIR" getconnectioncount | jq -r '.result // 0')"
        [[ "$c" -ge 1 ]] && { CONN_OK=1; break; }
        sleep 1
    done
    [[ "$CONN_OK" == "1" ]] || fail "stateless consumer could not connect to the source bridge"
    info "stateless consumer connected to source bridge — forward-connect window open"

    # ── Observe: cross the spend block, or stall on the exact reconstruction error ──
    RECON_LINE="pre-base spent coin ${SPB_TXID:0:16}:0 reconstructed from frozen pre-base store"
    FAIL_LINE="undo-spent-reconstruction-failed"
    CROSSED=0; STALL_SEEN=0; RECON_SEEN=0
    RACE_START=$SECONDS; LAST_LOG=$SECONDS
    while (( SECONDS - RACE_START < PROMO_TIMEOUT )); do
        grep -qsF "$RECON_LINE" "$CON_DIR"/daemon*.log 2>/dev/null && RECON_SEEN=1
        grep -qsF "$FAIL_LINE"  "$CON_DIR"/daemon*.log 2>/dev/null && STALL_SEEN=1
        TIP="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
        [[ "$TIP" -ge "$NET_TIP" ]] && { CROSSED=1; }
        # Fixed run resolves on cross; neuter run resolves on the stall line.
        [[ "$EXPECT_PREBASE_STALL" == "1" && "$STALL_SEEN" == "1" ]] && break
        [[ "$EXPECT_PREBASE_STALL" != "1" && "$CROSSED" == "1" && "$RECON_SEEN" == "1" ]] && break
        if (( SECONDS - LAST_LOG >= 15 )); then
            LAST_LOG=$SECONDS
            info "  [t+$((SECONDS-RACE_START))s] tip=$TIP net=$NET_TIP recon=$RECON_SEEN stall=$STALL_SEEN"
        fi
        sleep 2
    done
    grep -qsF "$RECON_LINE" "$CON_DIR"/daemon*.log 2>/dev/null && RECON_SEEN=1
    grep -qsF "$FAIL_LINE"  "$CON_DIR"/daemon*.log 2>/dev/null && STALL_SEEN=1
    TIP="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
    [[ "$TIP" -ge "$NET_TIP" ]] && CROSSED=1
    info "observation: tip=$TIP net_tip=$NET_TIP crossed=$CROSSED recon_seen=$RECON_SEEN stall_seen=$STALL_SEEN"

    if [[ "$EXPECT_PREBASE_STALL" == "1" ]]; then
        # ── NEUTER expectation (un-fixed binary) ──────────────────────────────
        # Must fail on the EXACT outpoint, not merely "didn't reach the tip" — a
        # plumbing stall (source not serving utreexo blocks) would masquerade
        # otherwise. Require the reconstruction-failed line AND the outpoint hex.
        if grep -qsF "$FAIL_LINE" "$CON_DIR"/daemon*.log 2>/dev/null \
           && grep -qsF "$SPB_TXID" "$CON_DIR"/daemon*.log 2>/dev/null; then
            ck_pass "N1 (neuter): un-fixed binary stalled with $FAIL_LINE on outpoint ${SPB_TXID:0:16}...:0"
        else
            ck_fail "N1 (neuter): expected $FAIL_LINE on outpoint $SPB_TXID; log did not show it (tip=$TIP)"
        fi
        if [[ "$CROSSED" != "1" ]]; then
            ck_pass "N2 (neuter): stateless consumer did NOT cross the spend block (stalled at/near base, tip=$TIP < $NET_TIP)"
        else
            ck_fail "N2 (neuter): consumer crossed to tip=$TIP despite un-fixed binary — reconstruction path not exercised?"
        fi
    else
        # ── FIXED expectation ────────────────────────────────────────────────
        # P1: the reconstruction path RAN and recovered the coin from the
        # frozen pre-base store (fidelity line present). Its mere presence proves
        # ChainDB.getCoin MISSED (the fix's branch only runs after that miss), so
        # the coin was still pre-base-only — the exact 90391 condition.
        FID="$(grep -hoE "pre-base spent coin ${SPB_TXID:0:16}:0 reconstructed from frozen pre-base store \(height=[0-9]+ coinbase=[01]\)" \
                "$CON_DIR"/daemon*.log 2>/dev/null | head -1)"
        if [[ -n "$FID" ]]; then
            ck_pass "P1 (CORE): CSN undo reconstructed the pre-base spent coin from the frozen pre-base store: $FID"
        else
            ck_fail "P1 (CORE): no AssumeUTXO reconstruction fidelity line for ${SPB_TXID:0:16}:0 — path did not run (fix ineffective or coin read from ChainDB)"
        fi
        # P2: fidelity — height and coinbase flag faithful (peer's must-assert).
        FID_H="$(sed -nE 's/.*height=([0-9]+) coinbase=[01].*/\1/p' <<<"$FID" | head -1)"
        FID_CB="$(sed -nE 's/.*coinbase=([01]).*/\1/p' <<<"$FID" | head -1)"
        if [[ "$FID_H" == "$SPB_HEIGHT" && "$FID_CB" == "1" ]]; then
            ck_pass "P2 (FIDELITY): reconstructed coin height=$FID_H (== spent-input height $SPB_HEIGHT) coinbase=$FID_CB (== 1)"
        else
            ck_fail "P2 (FIDELITY): expected height=$SPB_HEIGHT coinbase=1; got height=${FID_H:-?} coinbase=${FID_CB:-?}"
        fi
        # P3: the consumer actually crossed the spend block (not held at base).
        if [[ "$CROSSED" == "1" ]]; then
            ck_pass "P3: stateless consumer crossed the pre-base-spend block to tip=$TIP (>= net tip $NET_TIP)"
        else
            ck_fail "P3: stateless consumer did not reach net tip $NET_TIP (tip=$TIP) — connection aborted?"
        fi
        # P4: the un-fixed failure never occurred on the fixed binary.
        if ! grep -qsF "$FAIL_LINE" "$CON_DIR"/daemon*.log 2>/dev/null; then
            ck_pass "P4: no $FAIL_LINE on the fixed binary"
        else
            ck_fail "P4: $FAIL_LINE appeared despite the fix — reconstruction still failing"
        fi
        # P5 (timing corroboration): the reconstruction happened BEFORE promotion
        # materialized pre-base coins into ChainDB (if a promotion line exists).
        RECON_LN="$(grep -nF "$RECON_LINE" "$CON_DIR"/daemon*.log 2>/dev/null | head -1 | sed -E 's/^[^:]*:([0-9]+):.*/\1/')"
        PROMO_LN="$(grep -nE "\[Promotion\] complete" "$CON_DIR"/daemon*.log 2>/dev/null | head -1 | sed -E 's/^[^:]*:([0-9]+):.*/\1/')"
        if [[ -n "$RECON_LN" && -n "$PROMO_LN" ]]; then
            if [[ "$RECON_LN" -lt "$PROMO_LN" ]]; then
                ck_pass "P5: reconstruction (log line $RECON_LN) preceded promotion (line $PROMO_LN) — coin was pre-base-only when spent"
            else
                ck_fail "P5: reconstruction (line $RECON_LN) did not precede promotion (line $PROMO_LN)"
            fi
        else
            info "P5: promotion line not present in the same log window — P1's ChainDB-miss proof stands on its own"
        fi
    fi

    stop_node "$CON_DIR"
    stop_node "$SRC_DIR"
    if [[ "$FAILED" == "0" ]]; then
        echo "PREBASE_SPEND_MODE ASSERTIONS PASSED"
    else
        echo "PREBASE_SPEND_MODE TEST FAILED (see [FAIL] lines above)" >&2
    fi
    exit "$FAILED"
fi

# ═════════════════════════════════════════════════════════════════════════════
# PREBASE_MEMPOOL_MODE — mempool/BROADCAST twin of the 90391 fix. A stateless CSN
# node must be able to BROADCAST a spend of a live PRE-BASE coin, and must REJECT
# a spend of a pre-base coin already spent post-base (double-spend gate).
# ═════════════════════════════════════════════════════════════════════════════
if [[ "$PREBASE_MEMPOOL_MODE" == "1" ]]; then
    info "=== PREBASE_MEMPOOL_MODE: stateless CSN admits a live pre-base spend, rejects a spent one ==="

    # --- source: full/bridge node; owns every coin; serves utreexo proofs ---
    start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log" \
        --utreexo-bridge=1
    SRC_ADDR="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getnewaddress '[]' \
        | jq -r '.result.address // .result // empty')"
    [[ -n "$SRC_ADDR" ]] || fail "source wallet.getnewaddress returned no address"
    rpc "$SRC_RPC" "$SRC_DIR" generatetoaddress "[$BASE_HEIGHT,\"$SRC_ADDR\"]" \
        | jq -e ".result.blocks | length == $BASE_HEIGHT" >/dev/null \
        || fail "source failed to mine $BASE_HEIGHT base blocks"
    BASE="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
    [[ "$BASE" -eq "$BASE_HEIGHT" ]] || fail "source height $BASE != base $BASE_HEIGHT"

    # A LIVE pre-base coinbase we will NOT spend: block SRC_PREBASE_H's coinbase.
    # It stays unspent -> present in the frozen pre-base store AND the forest.
    LIVE_BH="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$SRC_PREBASE_H]" | jq -r '.result')"
    LIVE_TXID="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$LIVE_BH\",1]" \
        | jq -r '.result.tx[0] | if type=="string" then . else .txid end')"
    [[ "$LIVE_TXID" =~ ^[0-9a-fA-F]{64}$ ]] || fail "could not resolve live pre-base coinbase txid"
    info "live pre-base coinbase (kept unspent): ${LIVE_TXID:0:16}...:0 (height $SRC_PREBASE_H)"

    # Export snapshot at base (every source coin is an unspent pre-base coinbase).
    jq -e '.result.coins_written >= 1' \
        <<<"$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP\"]")" >/dev/null \
        || fail "dumptxoutset failed"
    [[ -s "$SNAP" ]] || fail "snapshot not written"
    stop_node "$SRC_DIR"
    cp -R "$SRC_DIR/headers" "$HEADERS_AT_BASE"
    start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon2.log" \
        --utreexo-bridge=1

    # POST-EXPORT: spend a DIFFERENT pre-base coinbase in base+1. That outpoint
    # (SPB) is unspent-at-base (resolvable from the frozen store) but SPENT post-base
    # (its forest leaf is removed once the consumer crosses base+1) — the exact
    # double-spend the authorize gate must reject.
    # Reserve the LIVE fixture explicitly. Wallet coin selection is otherwise
    # free to choose it, which makes the test topology invalid before any
    # AssumeUTXO behavior is exercised.
    rpc "$SRC_RPC" "$SRC_DIR" wallet.lockunspent \
        "[false,[{\"txid\":\"$LIVE_TXID\",\"vout\":0}]]" \
        | jq -e '.error == null and (.result == true or .result.success == true)' >/dev/null \
        || fail "could not lock reserved LIVE pre-base coin ${LIVE_TXID:0:16}:0"
    DEST="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getnewaddress '[]' \
        | jq -r '.result.address // .result // empty')"
    SEND_RES="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.sendtoaddress "[\"$DEST\",50.0]")"
    SPEND_TXID="$(jq -r '.result.txid // .result // empty' <<<"$SEND_RES")"
    [[ "$SPEND_TXID" =~ ^[0-9a-fA-F]{64}$ ]] \
        || fail "wallet.sendtoaddress could not spend a pre-base coinbase: $SEND_RES"
    SPEND_VIN="$(rpc "$SRC_RPC" "$SRC_DIR" wallet.getrawtransaction "[\"$SPEND_TXID\",true]" \
        | jq -c '.result.vin // []')"
    SPB_TXID=""; SPB_VOUT=""
    while IFS=$'\t' read -r in_txid in_vout; do
        [[ -n "$in_txid" && -n "$in_vout" ]] || continue
        COIN="$(rpc "$SRC_RPC" "$SRC_DIR" gettxout "[\"$in_txid\",$in_vout,false]")"
        H="$(jq -r '.result.height // -1' <<<"$COIN")"
        if [[ "$H" -ge 1 && "$H" -le "$BASE" && "$in_txid" != "$LIVE_TXID" ]]; then
            SPB_TXID="$in_txid"; SPB_VOUT="$in_vout"; break
        fi
    done < <(jq -r '.[] | [.txid, (.vout|tostring)] | @tsv' <<<"$SPEND_VIN")
    [[ -n "$SPB_TXID" ]] || fail "spend selected no pre-base input distinct from the live coin"
    info "pre-base coin spent post-base (double-spend target): ${SPB_TXID:0:16}...:$SPB_VOUT"
    rpc "$SRC_RPC" "$SRC_DIR" generate '[1]' \
        | jq -e '.result.blocks | length == 1' >/dev/null \
        || fail "source failed to mine the spend block (base+1)"
    # Extend a couple blocks so the consumer has a clear tip to reach.
    rpc "$SRC_RPC" "$SRC_DIR" generate '[2]' >/dev/null || true
    NET_TIP="$(rpc "$SRC_RPC" "$SRC_DIR" getblockcount | jq -re '.result')"
    info "source at $NET_TIP (base $BASE + post-base spend + extension)"
    # Guard: coin selection must not have spent our LIVE coin (else A is invalid).
    LIVE_CHK="$(rpc "$SRC_RPC" "$SRC_DIR" gettxout "[\"$LIVE_TXID\",0,false]" | jq -r '.result.height // empty')"
    [[ -n "$LIVE_CHK" ]] \
        || fail "the chosen LIVE pre-base coin ${LIVE_TXID:0:16}:0 was spent by setup coin-selection — adjust SRC_PREBASE_H"

    # --- consumer: STATELESS (ios_utreexo), load snapshot, forward-connect ---
    mkdir -p "$CON_DIR/headers"; cp -R "$HEADERS_AT_BASE/." "$CON_DIR/headers/"
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon.log" \
        --utreexo-stateless=1 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_forward_connect=1 --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    STATELESS_OK=0
    for i in $(seq 1 20); do
        grep -qsE "Sync profile:.*mode=STATELESS" "$CON_DIR/daemon.log" 2>/dev/null && { STATELESS_OK=1; break; }
        grep -qsE "Sync profile:.*mode=STATEFUL" "$CON_DIR/daemon.log" 2>/dev/null \
            && fail "consumer resolved to STATEFUL despite --utreexo-stateless=1 — gate would not run"
        sleep 1
    done
    [[ "$STATELESS_OK" == "1" ]] || fail "could not confirm consumer resolved to stateless mode"
    LOAD_RES="$(rpc "$CON_RPC" "$CON_DIR" loadtxoutset "[\"$SNAP\"]")"
    jq -e '.result.coins_loaded >= 1' <<<"$LOAD_RES" >/dev/null || fail "loadtxoutset failed: $LOAD_RES"
    info "snapshot loaded on stateless consumer: $(jq -c '.result | {base_height, coins_loaded}' <<<"$LOAD_RES")"
    stop_node "$CON_DIR"
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon2.log" \
        --utreexo-stateless=1 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_snapshot="$SNAP" --assumeutxo_forward_connect=1 \
        --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    wait_status "$CON_RPC" "$CON_DIR" '.assumeutxo_active == true' 60 "stateless snapshot rehydrated" \
        || fail "stateless consumer did not rehydrate the snapshot lifecycle"

    # Forward-connect to the source so the consumer crosses base+1 and its forest
    # DROPS the SPB leaf (SPB is now spent) while keeping the LIVE leaf.
    rpc "$CON_RPC" "$CON_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"add\"]" >/dev/null || true
    rpc "$CON_RPC" "$CON_DIR" addnode "[\"127.0.0.1:${SRC_P2P}\",\"onetry\"]" >/dev/null || true
    CROSSED=0
    for i in $(seq 1 90); do
        TIP="$(rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -r '.result // 0')"
        [[ "$TIP" -ge "$NET_TIP" ]] && { CROSSED=1; break; }
        sleep 2
    done
    [[ "$CROSSED" == "1" ]] || fail "stateless consumer did not cross to net tip $NET_TIP (forest would not reflect the spend)"
    info "consumer crossed to tip $NET_TIP — forest now reflects the post-base spend"

    # Helper: build an unsigned raw tx spending <txid>:<vout> and submit it. The
    # mempool input lookup (the fix's site) runs BEFORE signature validation, so
    # the error string alone tells us whether the input RESOLVED+AUTHORIZED:
    #   - resolved+authorized -> fails later (unsigned) with a NON-"not found" error
    #   - missed/rejected      -> "Input UTXO not found: <txid>:<vout>"
    DST="$(rpc "$CON_RPC" "$CON_DIR" wallet.getnewaddress '[]' | jq -r '.result.address // .result // empty')"
    [[ -n "$DST" ]] || fail "consumer wallet.getnewaddress failed"
    submit_spend_err() {  # <txid> <vout> -> prints the mempool error text
        local txid="$1" vout="$2"
        local raw
        raw="$(rpc "$CON_RPC" "$CON_DIR" wallet.createrawtransaction \
              "[[{\"txid\":\"$txid\",\"vout\":$vout}],{\"$DST\":1.0}]" \
              | jq -r '.result.hex // .result // empty')"
        [[ -n "$raw" ]] || { echo "CREATE_FAILED"; return; }
        rpc "$CON_RPC" "$CON_DIR" wallet.sendrawtransaction "[\"$raw\"]" 2>/dev/null \
            | jq -r '(.error.message // .result.error // .result // "") | tostring'
    }

    NF='Input UTXO not found'
    A_ERR="$(submit_spend_err "$LIVE_TXID" 0)"
    B_ERR="$(submit_spend_err "$SPB_TXID" "$SPB_VOUT")"
    info "A (live pre-base) sendrawtransaction -> ${A_ERR:0:120}"
    info "B (spent post-base) sendrawtransaction -> ${B_ERR:0:120}"

    if [[ "$EXPECT_PREBASE_MEMPOOL_NEUTER" == "1" ]]; then
        # NEUTER expectation (fix reverted): the LIVE coin itself is rejected with
        # the exact field error — reproduces the phone bug.
        if [[ "$A_ERR" == *"$NF"* ]]; then
            ck_pass "N1 (neuter): un-fixed binary rejects the LIVE pre-base spend with '$NF'"
        else
            ck_fail "N1 (neuter): expected '$NF' for the live coin on the un-fixed binary; got: $A_ERR"
        fi
    else
        # FIXED expectation.
        # A: the live pre-base input RESOLVES+AUTHORIZES (not the not-found reject).
        if [[ "$A_ERR" != *"$NF"* ]]; then
            ck_pass "A (RESOLVE): live pre-base coin admitted past input lookup (no '$NF'): ${A_ERR:0:80}"
        else
            ck_fail "A (RESOLVE): live pre-base coin still rejected with '$NF' — fix ineffective"
        fi
        # B (THE double-spend gate): the spent-post-base coin MUST be rejected. It
        # resolves from the frozen pre-base store but its forest leaf is gone -> authorize fails.
        if [[ "$B_ERR" == *"$NF"* ]]; then
            ck_pass "B (AUTHORIZE): spent-post-base coin correctly REJECTED with '$NF' (forest-leaf-absent gate held)"
        else
            ck_fail "B (AUTHORIZE): spent-post-base coin was ADMITTED (double-spend!) — forest gate failed: ${B_ERR:0:80}"
        fi
    fi

    stop_node "$CON_DIR"; stop_node "$SRC_DIR"
    if [[ "$FAILED" == "0" ]]; then
        echo "PREBASE_MEMPOOL_MODE ASSERTIONS PASSED"
    else
        echo "PREBASE_MEMPOOL_MODE TEST FAILED (see [FAIL] lines above)" >&2
    fi
    exit "$FAILED"
fi

# ═════════════════════════════════════════════════════════════════════════════
# SNAPSHOT_ROTATION_SELFHEAL_MODE — a persisted ACTIVE AssumeUTXO lifecycle whose
# base snapshot was ROTATED AWAY. Without the fix the node hard-fails to start
# Chainstate ("no matching configured snapshot candidate"); with the fix it
# self-heals (clean re-bootstrap from the still-available snapshot, shielded
# marker re-established, no SAFE MODE, no wipe loop).
# ═════════════════════════════════════════════════════════════════════════════

if [[ "$SNAPSHOT_ROTATION_SELFHEAL_MODE" == "1" ]]; then
    info "=== SNAPSHOT_ROTATION_SELFHEAL_MODE: stale lifecycle base rotated away → self-heal ==="
    [[ "$SELFHEAL_H_NEW" -gt "$SELFHEAL_H_GONE" ]] || fail "SELFHEAL_H_NEW must exceed SELFHEAL_H_GONE"
    SNAP_GONE="$WORK/snap-gone-${SELFHEAL_H_GONE}.dat"
    SNAP_NEW="$WORK/snap-new-${SELFHEAL_H_NEW}.dat"

    # ── Source: export a v4 snapshot at H_GONE and a newer v4 one at H_NEW. ──
    start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log"
    rpc "$SRC_RPC" "$SRC_DIR" generate "[$SELFHEAL_H_GONE]" \
        | jq -e ".result.blocks | length == $SELFHEAL_H_GONE" >/dev/null \
        || fail "source failed to mine to H_GONE=$SELFHEAL_H_GONE"
    jq -e '.result.coins_written >= 1' \
        <<<"$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP_GONE\"]")" >/dev/null \
        || fail "dumptxoutset SNAP_GONE failed"
    rpc "$SRC_RPC" "$SRC_DIR" generate "[$((SELFHEAL_H_NEW - SELFHEAL_H_GONE))]" \
        | jq -e ".result.blocks | length == $((SELFHEAL_H_NEW - SELFHEAL_H_GONE))" >/dev/null \
        || fail "source failed to mine to H_NEW=$SELFHEAL_H_NEW"
    jq -e '.result.coins_written >= 1' \
        <<<"$(rpc "$SRC_RPC" "$SRC_DIR" dumptxoutset "[\"$SNAP_NEW\"]")" >/dev/null \
        || fail "dumptxoutset SNAP_NEW failed"
    [[ -s "$SNAP_GONE" && -s "$SNAP_NEW" ]] || fail "snapshots not written"
    # Consistent headers-at-H_NEW copy requires the source STOPPED. Both snapshot
    # bases (H_GONE, H_NEW) must be in the consumer's header chain to load (binding).
    stop_node "$SRC_DIR"
    cp -R "$SRC_DIR/headers" "$HEADERS_AT_BASE"
    info "exported SNAP_GONE@$SELFHEAL_H_GONE and SNAP_NEW@$SELFHEAL_H_NEW"

    # ── Consumer: stateless + OFFLINE, load SNAP_GONE → persisted ACTIVE lifecycle
    #    at H_GONE (offline so background validation can't promote it away). Seed
    #    headers so the snapshot bases bind. ──
    mkdir -p "$CON_DIR/headers"
    cp -R "$HEADERS_AT_BASE/." "$CON_DIR/headers/"
    start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon-load.log" \
        --utreexo-stateless=1 --p2p.offline=1 --listen=0 --assumeutxo_bg_stall_timeout=3600 \
        --assumeutxo_snapshot="$SNAP_GONE" --assumeutxo_forward_connect=1 \
        --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
    jq -e '.result.coins_loaded >= 1' \
        <<<"$(rpc "$CON_RPC" "$CON_DIR" loadtxoutset "[\"$SNAP_GONE\"]")" >/dev/null \
        || fail "consumer loadtxoutset SNAP_GONE failed"
    wait_status "$CON_RPC" "$CON_DIR" '.assumeutxo_active == true' 60 "active lifecycle at H_GONE" \
        || fail "consumer did not enter an active AssumeUTXO lifecycle at H_GONE"
    GB="$(snap_status "$CON_RPC" "$CON_DIR" | jq -r '.snapshot_base_height // empty')"
    info "consumer active lifecycle base=$GB (expected $SELFHEAL_H_GONE)"
    stop_node "$CON_DIR"

    # ── ROTATE: SNAP_GONE is gone; restart configured with ONLY SNAP_NEW. The
    #    persisted lifecycle is still pinned to H_GONE → NoMatchingActiveLifecycle. ──
    if [[ "$EXPECT_SELFHEAL_FATAL" == "1" ]]; then
        # NEUTER: an un-fixed binary hard-fails to start Chainstate and never
        # becomes RPC-ready, so start it directly (start_node's readiness wait
        # would hard-fail the harness) and assert the exact fatal.
        "$DINEROD" --regtest --datadir="$CON_DIR" --rpcport="$CON_RPC" --port="$CON_P2P" \
            --wallet-socket-port="$CON_WS" --listen=0 --p2p.offline=1 \
            --utreexo-stateless=1 --assumeutxo_bg_stall_timeout=3600 \
            --assumeutxo_snapshot="$SNAP_NEW" --assumeutxo_forward_connect=1 \
            --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL" \
            > "$CON_DIR/daemon-heal.log" 2>&1 &
        NEUTER_READY=0
        for _ in $(seq 1 45); do
            if rpc "$CON_RPC" "$CON_DIR" getblockcount 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1; then
                NEUTER_READY=1; break
            fi
            grep -qsE "no matching configured snapshot candidate|Failed to start Chainstate" \
                "$CON_DIR/daemon-heal.log" 2>/dev/null && break
            sleep 1
        done
        if grep -qsE "no matching configured snapshot candidate" "$CON_DIR/daemon-heal.log" 2>/dev/null; then
            ck_pass "N1 (neuter): un-fixed binary hard-failed with 'no matching configured snapshot candidate'"
        else
            ck_fail "N1 (neuter): expected 'no matching configured snapshot candidate' fatal; not in log"
        fi
        if [[ "$NEUTER_READY" != "1" ]]; then
            ck_pass "N2 (neuter): Chainstate did not start; node never became RPC-ready (the reinstall-only brick)"
        else
            ck_fail "N2 (neuter): node became ready despite the un-fixed binary — self-heal unexpectedly present"
        fi
        stop_node "$CON_DIR"
    else
        # FIXED: the node is validly bootstrapped (forest checkpoint at/above base),
        # so the missing base snapshot is MOOT — it CONTINUES on its own state,
        # never wipes, and never adopts the different-base SNAP_NEW.
        start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon-heal.log" \
            --utreexo-stateless=1 --p2p.offline=1 --listen=0 --assumeutxo_bg_stall_timeout=3600 \
            --assumeutxo_snapshot="$SNAP_NEW" --assumeutxo_forward_connect=1 \
            --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
        # S1: continue-on-valid-state fired (node recognized its bootstrapped state)
        if grep -qsE "the snapshot file is moot; continuing on the existing" "$CON_DIR/daemon-heal.log"; then
            ck_pass "S1: continue-on-valid-state fired (bootstrapped node, moot base snapshot — NO wipe)"
        else
            ck_fail "S1: continue path did not fire; log lacks the moot-snapshot continue marker"
        fi
        # S2: NEVER wiped / NEVER adopted the different base (the silent-base-swap the belt guards)
        if ! grep -qsE "SELF-HEALING via clean re-bootstrap|re-bootstrapping from height|falling back to bundled floor" "$CON_DIR/daemon-heal.log" 2>/dev/null; then
            ck_pass "S2: no wipe / no re-bootstrap — the node kept its OWN state, never adopted the different base $SELFHEAL_H_NEW"
        else
            ck_fail "S2: the node wiped/re-bootstrapped — a bootstrapped node must continue, not swap base"
        fi
        # S3 (correct continue end-state): the lifecycle is KEPT (not retired) — the
        # node is assumeutxo-active at its OWN base (not the different SNAP_NEW base),
        # not fatal, not safe-mode, and bg-validation of genesis->own-base resumes.
        # It retires normally when bg-validation completes.
        FST="$(snap_status "$CON_RPC" "$CON_DIR")"
        AAB="$(jq -r '.snapshot_base_height // empty' <<<"$FST")"
        if jq -e '(.fatal // false) == false and (.assumeutxo_active // false) == true' <<<"$FST" >/dev/null 2>&1 \
           && [[ "$AAB" == "$SELFHEAL_H_GONE" ]] \
           && ! grep -qsE "EnterSafeMode|assumeutxo fatal" "$CON_DIR/daemon-heal.log" 2>/dev/null; then
            ck_pass "S3: continue end-state correct — assumeutxo active at OWN base $SELFHEAL_H_GONE (not $SELFHEAL_H_NEW), not fatal, not safe-mode (lifecycle kept, bg-validation resumes)"
        else
            ck_fail "S3: unexpected end-state (want active @own base $SELFHEAL_H_GONE, not-fatal, not-safe-mode): base=$AAB $FST"
        fi
        stop_node "$CON_DIR"

        # S4 (idempotent): restart again → continues again on the SAME own base, never
        # wipes, never adopts SNAP_NEW — every restart until bg-validation retires it.
        start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/daemon-heal2.log" \
            --utreexo-stateless=1 --p2p.offline=1 --listen=0 --assumeutxo_bg_stall_timeout=3600 \
            --assumeutxo_snapshot="$SNAP_NEW" --assumeutxo_forward_connect=1 \
            --utreexo.checkpoint_interval="$CHECKPOINT_INTERVAL"
        AAB2="$(snap_status "$CON_RPC" "$CON_DIR" | jq -r '.snapshot_base_height // empty')"
        if ! grep -qsE "SELF-HEALING via clean re-bootstrap|re-bootstrapping from height" "$CON_DIR/daemon-heal2.log" 2>/dev/null \
           && [[ "$AAB2" != "$SELFHEAL_H_NEW" ]]; then
            ck_pass "S4 (idempotent): second restart continued on own base ($AAB2), no wipe, never adopted $SELFHEAL_H_NEW"
        else
            ck_fail "S4 (idempotent): a wipe fired on restart"
        fi
        stop_node "$CON_DIR"
    fi

    if [[ "$FAILED" == "0" ]]; then
        echo "SNAPSHOT_ROTATION_SELFHEAL_MODE ASSERTIONS PASSED"
    else
        echo "SNAPSHOT_ROTATION_SELFHEAL_MODE TEST FAILED (see [FAIL] lines above)" >&2
    fi
    exit "$FAILED"
fi

info "=== Setup: source mines to base=$BASE_HEIGHT, exports snapshot, mines +$POST_BASE_K past base ==="

# Create the consumer wallet FIRST and mine the entire snapshot-base chain to
# one of its addresses. This models a wallet whose owned coins predate the
# snapshot without copying wallet databases or relying on seed-export APIs.
start_node "$CON_DIR" "$CON_RPC" "$CON_P2P" "$CON_WS" "$CON_DIR/wallet-seed.log"
OWNER_ADDRESS="$(rpc "$CON_RPC" "$CON_DIR" wallet.getnewaddress '[]' \
    | jq -r '.result.address // .result // empty')"
[[ -n "$OWNER_ADDRESS" ]] || fail "consumer wallet.getnewaddress returned no snapshot-owner address"
stop_node "$CON_DIR"

start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log"

rpc "$SRC_RPC" "$SRC_DIR" generatetoaddress "[$BASE_HEIGHT,\"$OWNER_ADDRESS\"]" \
    | jq -e ".result.blocks | length == $BASE_HEIGHT" >/dev/null \
    || fail "source failed to mine $BASE_HEIGHT base blocks to consumer-owned address"

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
mkdir -p "$CON_DIR/headers"
cp -R "$HEADERS_AT_BASE/." "$CON_DIR/headers/"

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

# ── F6 (USER-VISIBLE #353 CONTRACT): owned pre-base funds spend ──────────
# The specific height-$PREBASE_H coin is owned by the consumer wallet and was
# accumulator-only before promotion. It must now be reported spendable. Then
# spend more than all post-base coinbases combined can provide; those outputs
# are also immature, so a successful wallet-selected transaction necessarily
# consumes promoted pre-base coins. Finally inspect every actual input against
# the source's authoritative chain state and confirm the transaction on-chain.
WALLET_PB="$(rpc "$CON_RPC" "$CON_DIR" wallet.listunspent '[1,9999999]' \
    | jq -c --arg txid "$PB_TXID" \
        '.result | map(select(.txid == $txid and .vout == 0))[0] // null')"
if jq -e '.spendable == true and .solvable == true' <<<"$WALLET_PB" >/dev/null 2>&1; then
    ck_pass "F6a: consumer wallet lists the promoted pre-base coin as spendable + solvable"
else
    ck_fail "F6a: consumer wallet does not expose the promoted pre-base coin as spendable: $WALLET_PB"
fi

RECIPIENT="$(rpc "$CON_RPC" "$CON_DIR" wallet.getnewaddress '[]' \
    | jq -r '.result.address // .result // empty')"
[[ -n "$RECIPIENT" ]] || fail "consumer wallet.getnewaddress returned no recipient"

SEND_AMOUNT_DIN=$((POST_BASE_K * 100 + 100))
SEND_RES="$(rpc "$CON_RPC" "$CON_DIR" wallet.sendtoaddress \
    "[\"$RECIPIENT\",${SEND_AMOUNT_DIN}.0]")"
SPEND_TXID="$(jq -r '.result.txid // .result // empty' <<<"$SEND_RES")"
[[ "$SPEND_TXID" =~ ^[0-9a-fA-F]{64}$ ]] \
    || fail "wallet.sendtoaddress could not spend promoted pre-base funds: $SEND_RES"

SPEND_VERBOSE="$(rpc "$CON_RPC" "$CON_DIR" wallet.getrawtransaction \
    "[\"$SPEND_TXID\",true]")"
INPUTS="$(jq -c '.result.vin // []' <<<"$SPEND_VERBOSE")"
jq -e 'length > 0' <<<"$INPUTS" >/dev/null \
    || fail "promoted-funds transaction has no decoded inputs: $SPEND_VERBOSE"

PREBASE_INPUTS=0
while IFS=$'\t' read -r input_txid input_vout; do
    [[ -n "$input_txid" && -n "$input_vout" ]] || continue
    INPUT_COIN="$(rpc "$SRC_RPC" "$SRC_DIR" gettxout \
        "[\"$input_txid\",$input_vout,false]")"
    INPUT_HEIGHT="$(jq -r '.result.height // -1' <<<"$INPUT_COIN")"
    if [[ "$INPUT_HEIGHT" -ge 1 && "$INPUT_HEIGHT" -le "$BASE" ]]; then
        PREBASE_INPUTS=$((PREBASE_INPUTS + 1))
    fi
done < <(jq -r '.[] | [.txid, (.vout | tostring)] | @tsv' <<<"$INPUTS")

if [[ "$PREBASE_INPUTS" -ge 1 ]]; then
    ck_pass "F6b: wallet selected, signed, and mempool accepted $PREBASE_INPUTS promoted pre-base input(s)"
else
    ck_fail "F6b: accepted wallet transaction used no source-confirmed pre-base input: $INPUTS"
fi

SOURCE_SEES_SPEND=0
for i in $(seq 1 30); do
    if rpc "$SRC_RPC" "$SRC_DIR" getrawmempool '[]' \
        | jq -e --arg txid "$SPEND_TXID" '.result | index($txid) != null' >/dev/null 2>&1; then
        SOURCE_SEES_SPEND=1
        break
    fi
    sleep 1
done
[[ "$SOURCE_SEES_SPEND" == "1" ]] \
    || fail "source never received promoted-funds transaction $SPEND_TXID"

CONFIRM_RES="$(rpc "$SRC_RPC" "$SRC_DIR" generate '[1]')"
CONFIRM_BLOCK="$(jq -r '.result.blocks[0] // empty' <<<"$CONFIRM_RES")"
[[ "$CONFIRM_BLOCK" =~ ^[0-9a-fA-F]{64}$ ]] \
    || fail "source failed to mine confirmation block: $CONFIRM_RES"
CONFIRM_VERBOSE="$(rpc "$SRC_RPC" "$SRC_DIR" getblock "[\"$CONFIRM_BLOCK\",1]")"
if jq -e --arg txid "$SPEND_TXID" \
        '.result.tx | map(if type == "string" then . else .txid end) | index($txid) != null' \
        <<<"$CONFIRM_VERBOSE" >/dev/null; then
    ck_pass "F6c: promoted pre-base wallet spend confirmed in block ${CONFIRM_BLOCK:0:16}..."
else
    ck_fail "F6c: confirmation block omitted promoted-funds transaction: $CONFIRM_VERBOSE"
fi

stop_node "$CON_DIR"
stop_node "$SRC_DIR"

if [[ "$FAILED" == "0" ]]; then
    echo "ALL FORWARD-CONNECT ASSERTIONS PASSED"
else
    echo "FORWARD-CONNECT TEST FAILED (see [FAIL] lines above)" >&2
fi
exit "$FAILED"
