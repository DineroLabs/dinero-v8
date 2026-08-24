#!/usr/bin/env bash
#
# Phase 3b precondition for step 6 (recovery-maze deletion).
#
# After step 4's revisited slice-4 ordering, ConnectTip writes the
# shielded frontier flat file IMMEDIATELY after the unified rocksdb
# batch commits. There is still a tiny crash window between
# rocksdb's writeBatch fsync returning and std::ofstream's close()
# returning durable bytes — at that boundary the marker (in the
# unified batch) says N+1 while the frontier file is still at N.
# This is the exact partial state RecoverShieldedStateFromTipMarker
# was specced to reconcile.
#
# Step 6 plans to delete that recovery code on the basis that §1's
# atomic-unit law makes the partial states unreachable. Before any
# deletion, this test must pass: a crash injected exactly at the
# new "after_unified_batch_before_frontier_write" hook leaves the
# marker at N+1 and the frontier at N, and the daemon must recover
# WITHOUT operator intervention. If recovery succeeds without
# RecoverShieldedStateFromTipMarker doing real work, the maze is
# safe to delete. If it fails, the maze is still load-bearing and
# step 6 stays parked.
#
# Test asserts at restart:
#   - safemode.status is NOT active
#   - chain reaches the post-block height (unified batch did
#     commit, so the chain advanced)
#   - blockchain.getsynchealth shows aligned heights across every
#     container the unified batch touches
#   - one more block can be mined cleanly (forward-progress)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${ROOT_DIR}/build/dinerod"
fi
DATA_DIR="/tmp/dinero_frontier_gap_recovery_$$"
LOG_BASE="${DATA_DIR}.base.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_RESTART="${DATA_DIR}.restart.log"
PID=""
KEEP_ON_FAIL=0
RPC_PORT=$((35700 + RANDOM % 200))
P2P_PORT=$((RPC_PORT + 1))

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_CRASH}" ]] && { printf -- '--- crash log tail ---\n' >&2; tail -120 "${LOG_CRASH}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -120 "${LOG_RESTART}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_BASE}" "${LOG_CRASH}" "${LOG_RESTART}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    local datadir="$1"
    [[ -f "${datadir}/.cookie" ]] && { printf '%s\n' "${datadir}/.cookie"; return 0; }
    [[ -f "${datadir}/regtest/.cookie" ]] && { printf '%s\n' "${datadir}/regtest/.cookie"; return 0; }
    return 1
}

rpc_call() {
    local method="$1" params="$2"
    local cookie_path
    cookie_path="$(cookie_file "${DATA_DIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

wait_rpc() {
    for _ in $(seq 1 60); do
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 30); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    local atomic_flag=()
    case "${ATOMIC_PERSIST:-0}" in
        1|true|yes|on) atomic_flag=(-consensus.atomic_persist=1) ;;
    esac
    env "$@" "${DINEROD}" \
        -regtest \
        -datadir="${DATA_DIR}" \
        -rpcport="${RPC_PORT}" \
        -port="${P2P_PORT}" \
        -listen=0 \
        ${atomic_flag[@]+"${atomic_flag[@]}"} \
        > "${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

require_tools

info "ATOMIC_PERSIST=${ATOMIC_PERSIST:-0}"
info "Starting baseline daemon"
start_node "${LOG_BASE}"
wait_rpc || fail "baseline daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "getnewaddress" '[]')"
jq -e '.error == null' <<<"${ADDR_RESULT}" >/dev/null \
    || fail "getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" && "${MINER_ADDR}" != "null" ]] \
    || fail "empty miner address"

info "Mining a 5-block baseline"
MINE_RESULT="$(rpc_call "mining.generatetoaddress" "[5,\"${MINER_ADDR}\"]")"
jq -e '.error == null' <<<"${MINE_RESULT}" >/dev/null \
    || fail "baseline mining failed: ${MINE_RESULT}"

BASELINE_HEIGHT="$(rpc_call "getblockcount" '[]' | jq -r '.result')"
[[ "${BASELINE_HEIGHT}" -ge 5 ]] || fail "baseline height too low: ${BASELINE_HEIGHT}"
pass "Baseline at height ${BASELINE_HEIGHT}"

stop_node

info "Starting crash daemon with DINERO_CRASH_AT=after_unified_batch_before_frontier_write"
start_node "${LOG_CRASH}" "DINERO_CRASH_AT=after_unified_batch_before_frontier_write"
wait_rpc || fail "crash daemon did not reach RPC readiness"

set +e
rpc_call "mining.generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null 2>&1
set -e
wait_dead "${PID}" || fail "daemon did not crash at after_unified_batch_before_frontier_write"
PID=""

grep -q "DINERO_CRASH" "${LOG_CRASH}" \
    || fail "crash log did not show named crash hook"
pass "Crash hook fired between unified-batch commit and frontier write"

# At this exact crash boundary, on disk we expect:
#   - chaindb tip pointer at BASELINE+1 (unified batch committed)
#   - ShieldedTipMarker at BASELINE+1 (in the same batch)
#   - shielded frontier flat file at BASELINE (write didn't run)
# The ShieldedTipMarker startup consistency check WILL flag the
# mismatch; recovery (RecoverShieldedStateFromTipMarker) must
# resolve it without operator intervention.
info "Restarting daemon — testing whether recovery handles the gap"
start_node "${LOG_RESTART}"
wait_rpc \
    || fail "restarted daemon did not reach RPC readiness — recovery for the marker-vs-frontier gap is load-bearing"
pass "Restart reached RPC readiness without operator intervention"

# safemode.status: must NOT be active. If recovery silently entered
# safe mode, that counts as a failure for step-6 purposes.
SM_RESULT="$(rpc_call "safemode.status" '[]')"
if jq -e '.error == null' <<<"${SM_RESULT}" >/dev/null 2>&1; then
    SAFE_MODE_ACTIVE="$(jq -r '.result.active // false' <<<"${SM_RESULT}")"
    [[ "${SAFE_MODE_ACTIVE}" == "false" ]] \
        || fail "safe mode active after restart: ${SM_RESULT}"
    pass "Safe mode NOT active after restart"
else
    info "safemode.status RPC absent — relying on RPC readiness as proxy"
fi

# Composite probe: every container the unified batch touched plus
# the frontier-derived shielded state must agree on the same height.
SYNC="$(rpc_call "blockchain.getsynchealth" '[]')"
jq -e '.error == null' <<<"${SYNC}" >/dev/null \
    || fail "blockchain.getsynchealth failed: ${SYNC}"

ACTIVE="$(jq -r '.result.active_height // -1' <<<"${SYNC}")"
CHAINDB_TIP="$(jq -r '.result.chaindb_tip_height // -1' <<<"${SYNC}")"
FOREST_MARKER="$(jq -r '.result.forest_tip_marker_height // -1' <<<"${SYNC}")"
CHECKPOINT="$(jq -r '.result.latest_utreexo_checkpoint_height // -1' <<<"${SYNC}")"
SHIELDED_MARKER="$(jq -r '.result.shielded_tip_marker_height // -1' <<<"${SYNC}")"

EXPECTED=$((BASELINE_HEIGHT + 1))
[[ "${ACTIVE}" == "${EXPECTED}" ]] \
    || fail "active height ${ACTIVE} != expected ${EXPECTED} (probe=${SYNC})"
[[ "${CHAINDB_TIP}" == "${EXPECTED}" ]] \
    || fail "chaindb tip ${CHAINDB_TIP} != expected ${EXPECTED} (probe=${SYNC})"
[[ "${FOREST_MARKER}" == "${EXPECTED}" ]] \
    || fail "forest marker ${FOREST_MARKER} != expected ${EXPECTED} (probe=${SYNC})"
[[ "${CHECKPOINT}" == "${EXPECTED}" ]] \
    || fail "utreexo checkpoint ${CHECKPOINT} != expected ${EXPECTED} (probe=${SYNC})"
[[ "${SHIELDED_MARKER}" == "${EXPECTED}" ]] \
    || fail "shielded marker ${SHIELDED_MARKER} != expected ${EXPECTED} (probe=${SYNC})"
pass "All containers aligned at recovered tip ${EXPECTED}"

# Forward-progress: mining one more block exercises the post-recovery
# ConnectTip path. A regression that breaks fresh ConnectTip after
# recovery would surface here.
info "Forward-progress sanity: mine one more block"
MINE_AFTER_RESULT="$(rpc_call "mining.generatetoaddress" "[1,\"${MINER_ADDR}\"]")"
jq -e '.error == null' <<<"${MINE_AFTER_RESULT}" >/dev/null \
    || fail "post-recovery mining failed: ${MINE_AFTER_RESULT}"

FINAL_HEIGHT="$(rpc_call "getblockcount" '[]' | jq -r '.result')"
[[ "${FINAL_HEIGHT}" == $((EXPECTED + 1)) ]] \
    || fail "post-recovery mining did not advance: before=${EXPECTED} after=${FINAL_HEIGHT}"
pass "Post-recovery mining advanced to ${FINAL_HEIGHT}"

stop_node

pass "Phase 3b step-6 precondition: recovery for the marker-vs-frontier gap works without operator help"
