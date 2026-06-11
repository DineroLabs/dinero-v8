#!/usr/bin/env bash
#
# AssumeUTXO replay engine e2e (regtest).
# Spec docs/design/assumeutxo-fatal-state-machine.md Required Tests:
#   Scenario A+D = Test 4 (good snapshot retires trust marker through REAL
#                  genesis->base replay; fully_validated survives restart)
#                  PLUS cross-restart stall recovery: peerless load stalls in
#                  ~15s (proves --assumeutxo_bg_stall_timeout CLI delivery),
#                  the stall persists across a peerless restart (no silent
#                  promotion), and connecting the source peer heals the stall
#                  UNATTENDED: the daemon's P2P body backfill fetches heights
#                  1..base via getdata and the worker's complete replay pass
#                  retires the trust marker — no operator RPC, no restart.
#   Scenario B   = Test 1 (poisoned snapshot is fatal: load-time gates pass —
#                  regtest has no compiled-in anchor, simulating a compromised
#                  binary — but the genesis replay recomputes an honest digest
#                  that mismatches the poisoned commitment). Fatal survives
#                  restart; B2 = explicit operator reset recovers.
#   Scenario F   = post-promotion below-base fork (#280): after scenario A's
#                  node exits assumeutxo mode (promotion), a higher-work chain
#                  whose fork point lies BELOW the snapshot base must drive
#                  fatal + safe mode, never a silent reorg (the guard keys off
#                  the never-cleared promoted base height). The fork node is a
#                  full source-datadir copy taken at height 50, which then
#                  mines its own longer chain.
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
#   * Pre-base bodies arrive via the daemon's REAL P2P backfill: the
#     services tick loop (5s) arms the download scheduler for heights
#     1..base — anchored on the snapshot base HASH — whenever the lifecycle
#     is validating history, and disarms it once it is not (retired/fatal/
#     reset). Backfill shares the tip window's 16-slot in-flight cap.
#     Progress is surfaced in getsnapshotbootstrapstatus as
#     backfill_enabled/_total/_completed/_in_flight.
#   * Backfilled bodies are stored, not connected; the background worker's
#     header-chain fallback lookup finds them without ChainDB's canonical
#     height->hash index, so NO --reindex restart and NO submitblock crutch
#     is needed — the whole heal is unattended P2P.
#
set -euo pipefail

DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
BASE_PORT="${BASE_PORT:-36500}"
RUN_SCENARIOS="${RUN_SCENARIOS:-AD B C F}"

SRC_RPC=$((BASE_PORT + 0));  SRC_P2P=$((BASE_PORT + 100)); SRC_WS=$((BASE_PORT + 200))
AD_RPC=$((BASE_PORT + 1));   AD_P2P=$((BASE_PORT + 101));  AD_WS=$((BASE_PORT + 201))
B_RPC=$((BASE_PORT + 2));    B_P2P=$((BASE_PORT + 102));   B_WS=$((BASE_PORT + 202))
C_RPC=$((BASE_PORT + 3));    C_P2P=$((BASE_PORT + 103));   C_WS=$((BASE_PORT + 203))
F_RPC=$((BASE_PORT + 4));    F_P2P=$((BASE_PORT + 104));   F_WS=$((BASE_PORT + 204))

WORK="$(mktemp -d -t dinero_replay_e2e_XXXXXX)"
SRC_DIR="$WORK/source"; AD_DIR="$WORK/nodeAD"; B_DIR="$WORK/nodeB"; C_DIR="$WORK/nodeC"
F_DIR="$WORK/nodeF"; F_SEED="$WORK/source_at_50"; FORK_H=50
SNAP="$WORK/utxo-snapshot.dat"; SNAP2="$WORK/utxo-snapshot-2.dat"; POISON="$WORK/poisoned.dat"
HEADERS_AT_BASE="$WORK/headers_at_base"
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
SHIELDED_NOTE="not attempted"

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for d in "$SRC_DIR" "$AD_DIR" "$B_DIR" "$C_DIR" "$F_DIR"; do
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

connect_source() {  # <rpcport> <datadir> <node-desc> — addnode + wait for the link
    local port="$1" datadir="$2" desc="$3" i conns=0
    rpc "$port" "$datadir" addnode "[\"127.0.0.1:${SRC_P2P}\",\"add\"]" >/dev/null || true
    rpc "$port" "$datadir" addnode "[\"127.0.0.1:${SRC_P2P}\",\"onetry\"]" >/dev/null || true
    for i in $(seq 1 30); do
        conns="$(rpc "$port" "$datadir" getconnectioncount | jq -r '.result // 0')"
        [[ "$conns" -ge 1 ]] && return 0
        sleep 1
    done
    fail "$desc could not connect to the source peer"
}

# ═════════════════════════════════════════════════════════════════════════
# Setup: source chain (101 mature blocks + shielded tx + 1 confirm), snapshot
# ═════════════════════════════════════════════════════════════════════════
info "=== Setup: source node mines chain, exports snapshot at tip ==="
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon.log"

# Mine in two legs with a full-datadir copy at height FORK_H (50): scenario F
# needs a node whose chain ends BELOW the snapshot base so it can mine a
# competing fork whose fork point is below base. The copy must be taken with
# the source STOPPED (consistent stores).
rpc "$SRC_RPC" "$SRC_DIR" generate "[$FORK_H]" | jq -e ".result.blocks | length == $FORK_H" >/dev/null \
    || fail "source failed to mine first $FORK_H blocks"
stop_node "$SRC_DIR"
cp -R "$SRC_DIR" "$F_SEED"
rm -f "$F_SEED"/daemon*.log "$F_SEED"/.cookie "$F_SEED"/regtest/.cookie 2>/dev/null || true
start_node "$SRC_DIR" "$SRC_RPC" "$SRC_P2P" "$SRC_WS" "$SRC_DIR/daemon1b.log"
rpc "$SRC_RPC" "$SRC_DIR" generate "[$((101 - FORK_H))]" \
    | jq -e ".result.blocks | length == $((101 - FORK_H))" >/dev/null \
    || fail "source failed to mine blocks $((FORK_H + 1))..101"

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

# D3 + A: connect the source peer and do NOTHING else. The tick loop has
# already armed backfill (the lifecycle is validating/stalled and the base
# header is on the seeded chain); with a peer attached the scheduler's
# getdata fetches heights 1..base, the worker's header-chain fallback finds
# the stored bodies, and the COMPLETE replay pass recovers the persisted
# stall and retires the trust marker — unattended, no submitblock, no
# --reindex, no restart.
connect_source "$AD_RPC" "$AD_DIR" "nodeAD"
pass "source peer connected — heal is now pure P2P backfill"

# A: wait for retirement while watching the backfill surface every second:
#   * backfill_enabled must be observed true while validating (tick-loop arm)
#   * backfill_completed must progress (real bodies arriving via getdata)
#   * backfill_in_flight must never exceed the shared 16-slot window cap
#     (exceeding it is the e2e-visible symptom of the case-17
#     oversubscription bug)
BF_SEEN_ENABLED=0; BF_MAX_COMPLETED=0; BF_TOTAL_SEEN=0; BF_CAP_VIOLATION=""
A_DONE=0
for i in $(seq 1 240); do
    ST="$(snap_status "$AD_RPC" "$AD_DIR")"
    if jq -e '.backfill_enabled == true' <<<"$ST" >/dev/null 2>&1; then
        BF_SEEN_ENABLED=1
        BF_C="$(jq -r '.backfill_completed // 0' <<<"$ST")"
        BF_T="$(jq -r '.backfill_total // 0' <<<"$ST")"
        BF_F="$(jq -r '.backfill_in_flight // 0' <<<"$ST")"
        [[ "$BF_C" -gt "$BF_MAX_COMPLETED" ]] && BF_MAX_COMPLETED="$BF_C"
        [[ "$BF_T" -gt "$BF_TOTAL_SEEN" ]] && BF_TOTAL_SEEN="$BF_T"
        [[ "$BF_F" -gt 16 ]] && BF_CAP_VIOLATION="backfill_in_flight=$BF_F at sample $i"
    fi
    # Trust-marker retirement = history_fully_validated (lifecycle state
    # "fully_validated"). Mode exit (assumeutxo_active false) follows via
    # promotion (#280) and is asserted separately below (A-EXIT).
    if jq -e '.history_fully_validated == true and .history_validation_state == "fully_validated" and .fatal == false' \
            <<<"$ST" >/dev/null 2>&1; then
        A_DONE=1
        break
    fi
    sleep 1
done
[[ "$A_DONE" == "1" ]] || {
    printf '[FAIL] last status: %s\n' "$(snap_status "$AD_RPC" "$AD_DIR")" >&2
    fail "A: unattended P2P backfill did not retire the trust marker within 240s (backfill_enabled seen: $BF_SEEN_ENABLED, max completed: $BF_MAX_COMPLETED/$BF_TOTAL_SEEN)"
}
pass "A: real genesis->base replay via P2P backfill retires the trust marker (history_fully_validated)"
[[ "$BF_SEEN_ENABLED" == "1" ]] \
    || fail "A: backfill_enabled was never true while validating — tick-loop arm missing"
[[ "$BF_MAX_COMPLETED" -gt 0 ]] \
    || fail "A: backfill_completed never progressed above 0 — bodies did not arrive via backfill getdata"
[[ -z "$BF_CAP_VIOLATION" ]] \
    || fail "A: backfill exceeded the shared 16-slot in-flight cap: $BF_CAP_VIOLATION"
pass "A: backfill surface healthy (enabled seen, completed=$BF_MAX_COMPLETED/total=$BF_TOTAL_SEEN, in_flight <= 16 across all samples)"

# A: the 5s tick loop must DISARM backfill once the lifecycle stops
# validating (retirement) — backfill_enabled flips to false.
wait_status "$AD_RPC" "$AD_DIR" '.backfill_enabled == false' 30 \
    "A: backfill disarmed after retirement (periodic tick disarm)"

# A-EXIT (spec Required Test 4 restored by promotion, #280): after the worker
# promotes the replay-proven history into ChainDB (tip reaches base), the
# OnBackgroundValidationComplete exit gate clears AssumeUTXO mode — the legacy
# mode FULLY exits, not just the lifecycle flag. fully_validated is reported
# first (OnReplayComplete), then promotion runs (5-15s), then the gate fires;
# generous timeout for slow disks.
wait_status "$AD_RPC" "$AD_DIR" '.assumeutxo_active == false' 60 \
    "A-EXIT: assumeutxo_active false after promotion (spec Test 4)"

# Promotion side effects: the canonical height index now serves pre-base
# heights (getblockhash answers from ChainDB's height->hash index).
H1="$(rpc "$AD_RPC" "$AD_DIR" getblockhash '[1]')"
jq -e '.result | type == "string" and test("^[0-9a-fA-F]{64}$")' <<<"$H1" >/dev/null \
    || fail "A-EXIT: getblockhash 1 not served from promoted height index: $H1"
pass "A-EXIT: getblockhash 1 served from promoted height index: $(jq -r '.result' <<<"$H1")"

# A: retirement + mode exit survive a plain restart (spec Persistence /
# Required Test 4). Restart-clean: startup audits (strict archival + undo
# tail + alignment) must pass with tip at base and NO assumeutxo tolerance
# active, and the daemon must NOT enter safe mode.
stop_node "$AD_DIR"
start_node "$AD_DIR" "$AD_RPC" "$AD_P2P" "$AD_WS" "$AD_DIR/daemon3.log" \
    --assumeutxo_snapshot="$SNAP"
wait_status "$AD_RPC" "$AD_DIR" \
    '.history_fully_validated == true and .assumeutxo_active == false and .fatal == false' \
    60 "A: fully_validated + mode exit survive restart"
grep -q "Strict archival audit passed" "$AD_DIR/daemon3.log" \
    || fail "A: restarted daemon did not log the strict-archival pass line (promotion must leave flatfile coverage genesis->tip)"
pass "A: strict archival startup audit passed on the promoted datadir"
if grep -q "SAFE MODE ACTIVATED" "$AD_DIR/daemon3.log"; then
    fail "A: restarted promoted daemon entered safe mode: $(grep -m1 'SAFE MODE ACTIVATED' "$AD_DIR/daemon3.log")"
fi
pass "A: no safe-mode entry on the restarted promoted daemon"
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

# The poison flipped a UTXO record VALUE, not the base hash: the poisoned
# node's header chain is the honest one, so its tick-loop-armed backfill
# (anchored on the snapshot base HASH — same hash) downloads the HONEST
# chain's bodies from the source over real P2P. The replay then completes
# against honest history and the recomputed digest mismatches the poisoned
# commitment -> fatal. Unattended discovery: no submitblock, no --reindex.
connect_source "$B_RPC" "$B_DIR" "nodeB"
pass "B: source peer connected — poisoned node backfills honest bodies via P2P"

wait_status "$B_RPC" "$B_DIR" \
    '.fatal == true and .history_fully_validated == false and (.fatal_reason | contains("mismatch"))' \
    240 "B: poisoned snapshot drives fatal with mismatch reason after real replay over backfilled bodies"
info "fatal_reason: $(snap_status "$B_RPC" "$B_DIR" | jq -r '.fatal_reason')"

# Fatal survives restart (spec Required Test 1).
stop_node "$B_DIR"
start_node "$B_DIR" "$B_RPC" "$B_P2P" "$B_WS" "$B_DIR/daemon2.log" \
    --assumeutxo_snapshot="$POISON"
wait_status "$B_RPC" "$B_DIR" '.fatal == true' 60 "B: fatal survives restart"

# B-GATE: wallet send/spend paths must be refused while in safe mode (spec Fatal
# Mismatch Semantics §3).  The gate must fire BEFORE all downstream checks
# (address validation, balance, UTXO selection) so it returns the safe-mode
# error regardless of wallet state or funds.
#
# RED (ungated code): wallet.sendtoaddress on a fatal+safe-mode node passes the
#   active-wallet check (regtest auto-creates a default wallet) and fails at
#   address validation — {"result":{"error":"Invalid address: unsupported format"}}
#   — which is NOT a safe-mode refusal.  (Observed empirically 2026-06-10.)
# GREEN (gated code): returns {"result":{"error":"disabled while node is in
#   safe mode: assumeutxo fatal: ..."}} before any address/balance check.
#   (Observed empirically 2026-06-10.)
#
# We pass syntactically-valid params (address + amount) so no params-count
# short-circuit fires before the gate.
SEND_RES="$(rpc "$B_RPC" "$B_DIR" wallet.sendtoaddress \
    '["din1q0000000000000000000000000000000000000", 0.001]' || echo '{}')"
if jq -e '(.result.error // .error.message // "") | test("disabled while node is in safe mode")' \
        <<<"$SEND_RES" >/dev/null 2>&1; then
    pass "B-GATE: wallet.sendtoaddress refused with safe-mode error (gate fires before wallet/balance check)"
else
    fail "B-GATE: wallet.sendtoaddress did not return safe-mode refusal; got: $SEND_RES"
fi

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

# ═════════════════════════════════════════════════════════════════════════
# Scenario F: post-promotion below-base fork goes fatal, never reorg (#280)
# ═════════════════════════════════════════════════════════════════════════
if [[ " $RUN_SCENARIOS " == *" F "* ]]; then
info "=== Scenario F: below-base higher-work fork is fatal on the promoted node ==="
[[ " $RUN_SCENARIOS " == *" AD "* ]] \
    || fail "scenario F requires scenario AD (it reuses AD's promoted datadir)"

# Fork node: full source datadir copied at height FORK_H (< base). It mines
# its own chain from there — its block FORK_H+1 differs from the canonical
# one (different timestamp), so the fork point is FORK_H < base.
cp -R "$F_SEED" "$F_DIR"
start_node "$F_DIR" "$F_RPC" "$F_P2P" "$F_WS" "$F_DIR/daemon.log"
F_H="$(rpc "$F_RPC" "$F_DIR" getblockcount | jq -r '.result // 0')"
[[ "$F_H" -eq "$FORK_H" ]] \
    || fail "F: fork node expected height $FORK_H from the seed copy, got $F_H"

# Outwork every canonical tip in this test (base+5 after scenario C): regtest
# difficulty is flat, so more blocks = more work. 70 blocks -> fork tip 120.
FORK_TIP=$((FORK_H + 70))
rpc "$F_RPC" "$F_DIR" generate '[70]' | jq -e '.result.blocks | length == 70' >/dev/null \
    || fail "F: fork node failed to mine 70 fork blocks"
pass "F: fork node mined a higher-work chain diverging at height $FORK_H (tip $FORK_TIP)"

# Restart the PROMOTED scenario-A node. Mode already exited; the below-base
# guard keys off promoted_base_height_, restored at startup from the
# FullyValidated lifecycle record (it survives ClearAssumeUTXOState).
start_node "$AD_DIR" "$AD_RPC" "$AD_P2P" "$AD_WS" "$AD_DIR/daemon4.log" \
    --assumeutxo_snapshot="$SNAP"
wait_status "$AD_RPC" "$AD_DIR" \
    '.history_fully_validated == true and .assumeutxo_active == false and .fatal == false' \
    60 "F: promoted node restarted clean (mode exited) before fork arrival"

# Feed the fork via ordered submitblock (BlockAcceptor path). NOT pure P2P:
# the download scheduler DOES header-sync and fetch a higher-work fork from a
# peer, but it stores fork bodies to flatfiles without writing chain_db
# header metadata, so ActivateBestChain's header-branch import
# (HasStoredBlockBody, RequireFlatfiles) never sees them — a pre-existing
# deep-fork body-ingestion gap, observed empirically 2026-06-10 (70 bodies
# "StoreBlock success" yet missing_bodies stuck at 69). The guard under test
# lives in ActivateBestChain and is delivery-transport-independent;
# submitblock stores bodies WITH metadata and exercises it deterministically.
# Empirically fatal fires at fork height base+1 (work first exceeds the
# canonical tip's); the loop breaks there.
F_FATAL_SEEN=0
for h in $(seq $((FORK_H + 1)) "$FORK_TIP"); do
    FH="$(rpc "$F_RPC" "$F_DIR" getblockhash "[$h]" | jq -r '.result')"
    FHEX="$(rpc "$F_RPC" "$F_DIR" getblock "[\"$FH\", 0]" | jq -r '.result')"
    SUB="$(rpc "$AD_RPC" "$AD_DIR" submitblock "[\"$FHEX\"]")"
    SUB_ERR="$(jq -r '.result.error // empty' <<<"$SUB")"
    if jq -e '.fatal == true' <<<"$(snap_status "$AD_RPC" "$AD_DIR")" >/dev/null 2>&1; then
        F_FATAL_SEEN=1
        info "F: fatal observed after submitting fork block at height $h"
        break
    fi
    [[ -z "$SUB_ERR" ]] \
        || fail "F: submitblock of fork block at height $h refused before fatal: $SUB"
done
[[ "$F_FATAL_SEEN" == "1" ]] \
    || fail "F: promoted node never went fatal while ingesting the below-base fork up to height $FORK_TIP"

# Spec (Fatal Mismatch Semantics): a higher-work divergence BELOW the base is
# fatal + safe mode, never a silent reorg — even after promotion cleared the
# live assumeutxo fields.
wait_status "$AD_RPC" "$AD_DIR" \
    '.fatal == true and (.fatal_reason | contains("below assumeutxo base"))' \
    30 "F: below-base higher-work fork drives fatal (not reorg) on the promoted node"
info "F fatal_reason: $(snap_status "$AD_RPC" "$AD_DIR" | jq -r '.fatal_reason')"
grep -q "SAFE MODE ACTIVATED" "$AD_DIR/daemon4.log" \
    || fail "F: promoted node did not enter safe mode on the below-base fork"
pass "F: safe mode entered on below-base fork"
# Never-reorg: the active chain must not have switched onto the fork.
AD_H="$(rpc "$AD_RPC" "$AD_DIR" getblockcount | jq -r '.result // 0')"
[[ "$AD_H" -lt "$FORK_TIP" ]] \
    || fail "F: promoted node REORGED onto the below-base fork (height $AD_H)"
pass "F: no reorg onto the fork (active height $AD_H < fork tip $FORK_TIP)"
# Stricter no-disconnect proof: the canonical block just above the fork point
# must be unchanged — a guard placed after the disconnect walk (partial
# disconnect before fatal) would alter it even if the tip stayed below the
# fork tip. The source's hash at FORK_H+1 is canonical by construction.
CANON_51="$(rpc "$SRC_RPC" "$SRC_DIR" getblockhash "[$((FORK_H + 1))]" | jq -r '.result // empty')"
AD_51="$(rpc "$AD_RPC" "$AD_DIR" getblockhash "[$((FORK_H + 1))]" | jq -r '.result // empty')"
[[ -n "$CANON_51" && "$AD_51" == "$CANON_51" ]] \
    || fail "F: canonical block at $((FORK_H + 1)) changed (disconnect below base occurred): ad=$AD_51 canon=$CANON_51"
pass "F: no disconnect below base (canonical block at $((FORK_H + 1)) unchanged)"
# Coverage note: F exercises the post-restart guard variant (effective_base
# from promoted_base_height_ restored off the persisted FullyValidated
# record). The live same-process variant (set at promotion success) and the
# pre-promotion mode-active branch remain unit/e2e-uncovered — registered.
stop_node "$AD_DIR"
stop_node "$F_DIR"
fi

info "shielded-tx coverage: $SHIELDED_NOTE"
echo "ALL SCENARIOS PASSED"
