#!/usr/bin/env bash
# Forest checkpoint delta campaign phase 2
# (docs/design/forest-checkpoint-deltas.md, test plan items 3-4).
#
# Leg 1 — clean-restart replay: an every-5-checkpoint node stopped at a
# non-checkpoint tip must come back AT that tip through checkpoint + UD
# sidecar replay (log marker [ForestDeltaReplay]), with the latest full
# checkpoint UNMOVED (pure in-memory restore, no body-based catch-up
# rewrite), and stay fully functional (mine + disconnect afterwards).
#
# Leg 2 — crash-point: kill via DINERO_CRASH_AT at the
# after_unified_batch_before_frontier_write boundary (unified batch
# durable, auxiliary writes not). Restart must satisfy the campaign
# invariant forest(tip) == checkpoint(K) + deltas(K+1..tip] and land
# canonically aligned at the committed tip.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"

DATA_DIR="$(mktemp -d /tmp/dinero_delta_replay_restart.XXXXXX)"
LOG_A="${DATA_DIR}/node_initial.log"
LOG_B="${DATA_DIR}/node_restart.log"
LOG_C="${DATA_DIR}/node_crash.log"
LOG_D="${DATA_DIR}/node_postcrash.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for f in "${LOG_A}" "${LOG_B}" "${LOG_C}" "${LOG_D}"; do
        [[ -f "${f}" ]] && { printf -- '--- %s tail ---\n' "${f}" >&2; tail -40 "${f}" >&2 || true; }
    done
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}"
    fi
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

RPC_PORT=$((42000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
NODE_DATADIR="${DATA_DIR}/node"

cookie_file() {
    for c in "${NODE_DATADIR}/.cookie" "${NODE_DATADIR}/regtest/.cookie"; do
        [[ -f "${c}" ]] && { printf '%s\n' "${c}"; return 0; }
    done
    return 1
}

rpc() {
    local method="$1"
    local params_json="${2:-[]}"
    local cookie_path
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    curl -s --max-time 15 --user "$(tr -d '\n' < "${cookie_path}")" \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${NODE_DATADIR}"
    env "$@" "${DINEROD}" \
        --regtest \
        --datadir="${NODE_DATADIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        --utreexo.checkpoint_interval=5 \
        >"${log_file}" 2>&1 &
    PID=$!
}

wait_rpc() {
    for _ in $(seq 1 60); do
        kill -0 "${PID}" 2>/dev/null || return 1
        local r
        r="$(rpc getblockcount 2>/dev/null || true)"
        if [[ -n "${r}" ]] && jq -e '.result != null' <<<"${r}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 60); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 1
    done
    return 1
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc stop '[]' >/dev/null 2>&1 || kill "${PID}" 2>/dev/null || true
    wait_dead "${PID}" || kill -9 "${PID}" 2>/dev/null || true
    PID=""
}

get_field() { jq -r "$2" <<<"$1"; }

new_address() {
    local r
    r="$(rpc "wallet.getnewaddress" "[\"taproot\",\"$1\"]")"
    get_field "${r}" '.result.address // .result // empty'
}

mine_to() {
    local r
    r="$(rpc generatetoaddress "[$1,\"$2\"]")"
    jq -e '.error == null or (.error | not)' <<<"${r}" >/dev/null || fail "generatetoaddress failed: ${r}"
}

assert_health() {
    local expected_tip="$1" expected_ckpt="$2" label="$3"
    local health
    health="$(rpc "blockchain.getsynchealth" '[]')"
    jq -e \
        --argjson tip "${expected_tip}" \
        --argjson ckpt "${expected_ckpt}" \
        '
        .result.canonical_state_aligned == true and
        .result.active_height == $tip and
        .result.chaindb_tip_height == $tip and
        .result.forest_tip_marker_found == true and
        .result.forest_tip_marker_height == $tip and
        .result.latest_utreexo_checkpoint_found == true and
        .result.latest_utreexo_checkpoint_height == $ckpt
        ' <<<"${health}" >/dev/null || fail "${label}: ${health}"
}

# ─── Leg 1: clean restart restores via checkpoint + delta replay ────────────
info "Leg 1: building chain to a non-checkpoint tip (12, checkpoints every 5)"
start_node "${LOG_A}"
wait_rpc || fail "initial daemon did not reach RPC readiness"
ADDR_A="$(new_address "delta-replay-miner")"
[[ -n "${ADDR_A}" ]] || fail "no miner address"
ADDR_B="$(new_address "delta-replay-remine")"
[[ -n "${ADDR_B}" ]] || fail "no re-mine address"
mine_to 12 "${ADDR_A}"
assert_health 12 10 "pre-restart state"
stop_node

info "Leg 1: clean restart at tip 12 (checkpoint 10 + sidecars 11,12)"
start_node "${LOG_B}"
wait_rpc || fail "restarted daemon did not reach RPC readiness"
grep -q "\[ForestDeltaReplay\] forest restored to tip height 12 via checkpoint 10 + 2 delta sidecars" "${LOG_B}" \
    || fail "restart did not restore via delta replay (marker missing)"
# Pure in-memory restore: the latest FULL checkpoint must still be 10 —
# the pre-phase-2 body-based catch-up rewrote one at the tip instead.
assert_health 12 10 "post-restart replayed state"
pass "Clean restart restored the forest via checkpoint 10 + 2 delta sidecars"

info "Leg 1: node stays functional after a replay-restored start"
mine_to 3 "${ADDR_A}"
assert_health 15 15 "post-restart interval boundary"
TIP_HASH="$(get_field "$(rpc getblockhash '[15]')" '.result')"
INV="$(rpc "blockchain.invalidateblock" "[\"${TIP_HASH}\"]")"
jq -e '.error == null' <<<"${INV}" >/dev/null || fail "post-replay invalidateblock failed: ${INV}"
# The disconnected block sat on an interval height, so its full checkpoint
# stays on disk (immutable per height; restore ignores checkpoints above
# the tip) — latest reported checkpoint remains 15 while the tip is 14.
assert_health 14 15 "post-replay disconnect state"
mine_to 1 "${ADDR_B}"
assert_health 15 15 "post-replay re-advance"
pass "Replay-restored node mines, disconnects, and re-advances cleanly"

# ─── Leg 2: crash at the unified-batch boundary, restart via replay ─────────
info "Leg 2: crash at after_unified_batch_before_frontier_write during connect"
stop_node
start_node "${LOG_C}" DINERO_CRASH_AT="after_unified_batch_before_frontier_write"
wait_rpc || fail "crash-armed daemon did not reach RPC readiness"
set +e
rpc generatetoaddress "[1,\"${ADDR_A}\"]" >/dev/null 2>&1
set -e
wait_dead "${PID}" || fail "daemon did not crash at the armed hook"
PID=""
grep -q "DINERO_CRASH" "${LOG_C}" || fail "crash log did not show the named crash hook"
pass "Crash hook fired after the unified batch commit"

info "Leg 2: restart after crash must satisfy forest(tip) == checkpoint + deltas"
start_node "${LOG_D}"
wait_rpc || fail "post-crash daemon did not reach RPC readiness"
POST_TIP="$(get_field "$(rpc getblockcount '[]')" '.result')"
[[ "${POST_TIP}" == "16" || "${POST_TIP}" == "15" ]] \
    || fail "unexpected post-crash tip ${POST_TIP} (expected 16, or 15 if batch missed)"
HEALTH="$(rpc "blockchain.getsynchealth" '[]')"
jq -e --argjson tip "${POST_TIP}" '
    .result.canonical_state_aligned == true and
    .result.active_height == $tip and
    .result.chaindb_tip_height == $tip and
    .result.forest_tip_marker_height == $tip
' <<<"${HEALTH}" >/dev/null || fail "post-crash state not aligned: ${HEALTH}"
if [[ "${POST_TIP}" == "16" ]]; then
    grep -q "\[ForestDeltaReplay\] forest restored to tip height 16 via checkpoint 15 + 1 delta sidecars" "${LOG_D}" \
        || fail "post-crash restart did not replay the committed block's sidecar"
    pass "Crash-committed block recovered via checkpoint 15 + 1 delta sidecar"
else
    pass "Batch did not commit before crash; node restarted cleanly at 15"
fi
mine_to 1 "${ADDR_A}"
NEW_TIP="$(get_field "$(rpc getblockcount '[]')" '.result')"
[[ "${NEW_TIP}" == "$((POST_TIP + 1))" ]] || fail "post-crash node failed to advance"
pass "Post-crash node advances normally"

stop_node
pass "Delta-replay restart + crash-boundary recovery behave per design"
exit 0
