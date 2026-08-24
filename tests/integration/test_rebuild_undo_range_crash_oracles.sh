#!/usr/bin/env bash
#
# Crash-oracle integration test for the offline `--rebuild-undo-range`
# orchestrator (commit #7). Pins the interruption guarantees:
#
#   #4 — Crash AFTER `live_block_storage->writeUndo` returns but BEFORE
#        the LIVE chain DB metadata commit.
#        Hook: rebuild_after_live_writeUndo_before_metadata_commit
#        Expected post-crash state on restart:
#          * BLOCK_HAVE_UNDO unchanged on the LIVE row (still false)
#          * undo_file / undo_pos / undo_size unchanged
#          * the appended undo bytes in rev*.dat are durable but
#            harmless dead space (no metadata pointer references them)
#        Diagnostic: a follow-up dry-run still classifies the height as
#        a `hole`. No dishonest metadata.
#
#   #5 — Crash AFTER the LIVE chain DB metadata commit completes.
#        Hook: rebuild_after_live_metadata_commit
#        Expected post-crash state on restart:
#          * BLOCK_HAVE_UNDO == true on the LIVE row
#          * undo_file / undo_pos / undo_size point at rebuilt bytes
#          * daemon reads undo cleanly (DisconnectTip would succeed)
#        Diagnostic: a follow-up dry-run classifies the height as
#        `already_ok`. Consistent durable undo.
#
# Both hooks are gated on `Params().network_id == "regtest"` in the
# reindexer source so production never invokes them. They live at
# the only two interruption points in Step 5b that have observable
# state asymmetries.

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
DATA_DIR="/tmp/dinero_rebuild_crash_$$"
LOG_LIVE="${DATA_DIR}.live.log"
LOG_CRASH1="${DATA_DIR}.crash1.log"
LOG_CRASH2="${DATA_DIR}.crash2.log"
LOG_RESTART1="${DATA_DIR}.restart1.log"
LOG_RESTART2="${DATA_DIR}.restart2.log"
LOG_DRY1="${DATA_DIR}.dry1.log"
LOG_DRY2="${DATA_DIR}.dry2.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for f in "${LOG_LIVE}" "${LOG_CRASH1}" "${LOG_CRASH2}" \
             "${LOG_RESTART1}" "${LOG_RESTART2}" "${LOG_DRY1}" "${LOG_DRY2}"; do
        [[ -f "${f}" ]] && { printf -- '--- %s tail ---\n' "${f}" >&2; tail -120 "${f}" >&2 || true; }
    done
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" \
               "${LOG_LIVE}" "${LOG_CRASH1}" "${LOG_CRASH2}" \
               "${LOG_RESTART1}" "${LOG_RESTART2}" \
               "${LOG_DRY1}" "${LOG_DRY2}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null   || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    [[ -f "$1/.cookie" ]] && { printf '%s\n' "$1/.cookie"; return 0; }
    [[ -f "$1/regtest/.cookie" ]] && { printf '%s\n' "$1/regtest/.cookie"; return 0; }
    return 1
}

rpc_call() {
    local cookie_path
    cookie_path="$(cookie_file "$1" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$2\",\"params\":$3}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_has_error() {
    local compact
    compact="$(echo "$1" | tr -d '\n\t ')"
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 60); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 \
        "$@" >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc_call "${DATA_DIR}" "stop" '[]' >/dev/null 2>&1 || kill "${PID}" 2>/dev/null || true
    wait_dead "${PID}" || kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

# Run dinerod in --rebuild-undo-range mode synchronously. Optionally
# sets DINERO_CRASH_AT for crash-oracle injection.
run_offline_rebuild_with_crash() {
    local log_file="$1"
    local crash_at="$2"
    shift 2
    DINERO_CRASH_AT="${crash_at}" \
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 \
        "$@" >"${log_file}" 2>&1 || true
}

run_offline_rebuild() {
    local log_file="$1"
    shift
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 \
        "$@" >"${log_file}" 2>&1 || true
}

require_tools

RPC_PORT=$((36000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

# ─────────────────────────────────────────────────────────────────────
# Phase 1: Build a real regtest chain.
# ─────────────────────────────────────────────────────────────────────
info "Mining a regtest chain to set up two distinct undo holes"
start_node "${LOG_LIVE}"
wait_rpc || fail "initial daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty miner address"

MINE_RESULT="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[12,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_RESULT}" && fail "generatetoaddress failed: ${MINE_RESULT}"

# Resolve the two specific block hashes we'll punch holes at.
HASH_5_RESULT="$(rpc_call "${DATA_DIR}" "getblockhash" '[5]')"
rpc_has_error "${HASH_5_RESULT}" && fail "getblockhash 5 failed"
HASH_5="$(jq -r '.result' <<<"${HASH_5_RESULT}")"
HASH_6_RESULT="$(rpc_call "${DATA_DIR}" "getblockhash" '[6]')"
rpc_has_error "${HASH_6_RESULT}" && fail "getblockhash 6 failed"
HASH_6="$(jq -r '.result' <<<"${HASH_6_RESULT}")"
pass "Mined regtest chain; hash[5]=${HASH_5} hash[6]=${HASH_6}"

# ─────────────────────────────────────────────────────────────────────
# Property #4: Crash AFTER writeUndo, BEFORE metadata commit.
# ─────────────────────────────────────────────────────────────────────
info "Property #4: clearing undo flag at height 5, then injecting"
info "             rebuild_after_live_writeUndo_before_metadata_commit"
CLEAR_5_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.debugclearundoflag" "[\"${HASH_5}\"]")"
rpc_has_error "${CLEAR_5_RESULT}" && fail "debugclearundoflag 5 failed: ${CLEAR_5_RESULT}"

stop_node

# Capture rev*.dat byte total before the crashy rebuild attempt so
# we can later assert "the writeUndo bytes are durable as orphans".
SIZE_BEFORE_CRASH1=0
for f in $(find "${DATA_DIR}/blocks" -name 'rev*.dat' | sort); do
    SIZE_BEFORE_CRASH1=$(( SIZE_BEFORE_CRASH1 + $(stat -f%z "${f}" 2>/dev/null || stat -c%s "${f}") ))
done

run_offline_rebuild_with_crash "${LOG_CRASH1}" \
    "rebuild_after_live_writeUndo_before_metadata_commit" \
    --rebuild-undo-range=5:5 --rebuild-undo-write

grep -q "DINERO_CRASH" "${LOG_CRASH1}" || \
    fail "expected DINERO_CRASH banner in ${LOG_CRASH1} (hook 1 not triggered)"
grep -q "rebuild_after_live_writeUndo_before_metadata_commit" "${LOG_CRASH1}" || \
    fail "expected hook name in crash log"
pass "hook fired: rebuild_after_live_writeUndo_before_metadata_commit"

# rev*.dat must have grown — the writeUndo fsync completed before the
# abort, so the bytes are durable on disk.
SIZE_AFTER_CRASH1=0
for f in $(find "${DATA_DIR}/blocks" -name 'rev*.dat' | sort); do
    SIZE_AFTER_CRASH1=$(( SIZE_AFTER_CRASH1 + $(stat -f%z "${f}" 2>/dev/null || stat -c%s "${f}") ))
done
[[ "${SIZE_AFTER_CRASH1}" -gt "${SIZE_BEFORE_CRASH1}" ]] || \
    fail "rev*.dat did not grow before crash hook 1 (writeUndo did not fsync?)"
pass "rev*.dat grew $((SIZE_AFTER_CRASH1 - SIZE_BEFORE_CRASH1)) bytes (writeUndo durable as orphan dead space)"

# Restart daemon — must come up cleanly (no dishonest metadata,
# nothing to recover from).
info "Restarting daemon after hook 1 crash"
start_node "${LOG_RESTART1}"
wait_rpc || fail "daemon did not come up after hook 1 crash"
pass "daemon clean after hook 1 crash"

# Diagnostic: dry-run preflight on height 5 must still see it as
# a hole (no metadata commit happened, so BLOCK_HAVE_UNDO is still
# false — exactly the "no dishonest metadata" guarantee).
stop_node
run_offline_rebuild "${LOG_DRY1}" --rebuild-undo-range=5:5
MANIFEST="${DATA_DIR}/rebuild_undo_manifest.json"
[[ -f "${MANIFEST}" ]] || fail "manifest missing after dry-run after hook 1"
DRY1_HOLES="$(jq -r '.counts.holes' "${MANIFEST}")"
DRY1_OK="$(jq -r '.counts.already_ok' "${MANIFEST}")"
DRY1_STATUS="$(jq -r '.entries[0].status' "${MANIFEST}")"
[[ "${DRY1_STATUS}" == "hole" ]] || \
    fail "expected entries[0].status=hole after hook 1 (got ${DRY1_STATUS})"
[[ "${DRY1_HOLES}" -eq 1 ]] || fail "expected 1 hole, got ${DRY1_HOLES}"
[[ "${DRY1_OK}" -eq 0 ]] || fail "expected 0 already_ok, got ${DRY1_OK}"
pass "post-hook-1 preflight: BLOCK_HAVE_UNDO untouched (no dishonest metadata)"

# ─────────────────────────────────────────────────────────────────────
# Property #5: Crash AFTER metadata commit.
# ─────────────────────────────────────────────────────────────────────
info "Property #5: clearing undo flag at height 6, then injecting"
info "             rebuild_after_live_metadata_commit"
start_node "${LOG_LIVE}"
wait_rpc || fail "daemon did not come up before hook 2 setup"

CLEAR_6_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.debugclearundoflag" "[\"${HASH_6}\"]")"
rpc_has_error "${CLEAR_6_RESULT}" && fail "debugclearundoflag 6 failed: ${CLEAR_6_RESULT}"
stop_node

run_offline_rebuild_with_crash "${LOG_CRASH2}" \
    "rebuild_after_live_metadata_commit" \
    --rebuild-undo-range=6:6 --rebuild-undo-write

grep -q "DINERO_CRASH" "${LOG_CRASH2}" || \
    fail "expected DINERO_CRASH banner in ${LOG_CRASH2} (hook 2 not triggered)"
grep -q "rebuild_after_live_metadata_commit" "${LOG_CRASH2}" || \
    fail "expected hook name in crash log"
# The hook fires AFTER the writeBatch(sync=true) returns — so the
# metadata commit is durable by the time abort runs. We don't grep
# for the post-hook info log (it's after the abort point); the
# load-bearing diagnostic is the dry-run preflight below, which
# observes whatever state actually made it to disk.
pass "hook fired: rebuild_after_live_metadata_commit (after metadata fsync)"

# Restart daemon.
info "Restarting daemon after hook 2 crash"
start_node "${LOG_RESTART2}"
wait_rpc || fail "daemon did not come up after hook 2 crash"
pass "daemon clean after hook 2 crash"

# Diagnostic: dry-run preflight on height 6 must NOW see it as
# already_ok (BLOCK_HAVE_UNDO=true is durable; undo bytes are
# readable from rev*.dat). This is the "consistent durable undo"
# guarantee: the rebuilder run was atomic from the perspective of
# any in-window block whose metadata commit succeeded.
stop_node
run_offline_rebuild "${LOG_DRY2}" --rebuild-undo-range=6:6
[[ -f "${MANIFEST}" ]] || fail "manifest missing after dry-run after hook 2"
DRY2_HOLES="$(jq -r '.counts.holes' "${MANIFEST}")"
DRY2_OK="$(jq -r '.counts.already_ok' "${MANIFEST}")"
DRY2_STATUS="$(jq -r '.entries[0].status' "${MANIFEST}")"
[[ "${DRY2_STATUS}" == "already_ok" ]] || \
    fail "expected entries[0].status=already_ok after hook 2 (got ${DRY2_STATUS})"
[[ "${DRY2_HOLES}" -eq 0 ]] || fail "expected 0 holes, got ${DRY2_HOLES}"
[[ "${DRY2_OK}" -eq 1 ]] || fail "expected 1 already_ok, got ${DRY2_OK}"
pass "post-hook-2 preflight: BLOCK_HAVE_UNDO durable, undo bytes readable"

# Final sanity: chain must advance.
info "Final sanity: mine 1 more block"
start_node "${LOG_LIVE}"
wait_rpc || fail "daemon did not come up for final mine"
HEIGHT_BEFORE_FINAL="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
MINE_FINAL="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[1,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_FINAL}" && fail "post-crash final mine failed"
HEIGHT_AFTER_FINAL="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
[[ "${HEIGHT_AFTER_FINAL}" == "$((HEIGHT_BEFORE_FINAL + 1))" ]] || \
    fail "chain did not advance after crash oracles: before=${HEIGHT_BEFORE_FINAL} after=${HEIGHT_AFTER_FINAL}"
pass "chain advanced from ${HEIGHT_BEFORE_FINAL} to ${HEIGHT_AFTER_FINAL} after both crash oracles"

stop_node
pass "Both crash oracles validated end-to-end"
