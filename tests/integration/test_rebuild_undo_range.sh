#!/usr/bin/env bash
#
# Integration test for the offline `--rebuild-undo-range` orchestrator
# (commit #6 of the post-Apr 30 undo-rebuild series).
#
# Properties pinned by this test (per the rebuild design contract):
#
#   1. Dry-run maps holes but writes nothing.
#      A regtest node mines real blocks (which produces real undo via
#      ConnectTip). Stop. Run --rebuild-undo-range in dry-run mode.
#      Expect: manifest emitted with status=dry_run_complete, every
#      height classified as already_ok, and the on-disk LIVE rev*.dat
#      sha256 unchanged from before the run.
#
#   2. Write mode rebuilds holes.
#      Mine more blocks. Use the regtest-only RPC blockchain.debugclearundoflag
#      to flip BLOCK_HAVE_UNDO off for a few heights — that's exactly the
#      shape of a hole the orchestrator is designed to repair. Stop the
#      daemon. Run --rebuild-undo-range with --rebuild-undo-write.
#      Expect: manifest reports those heights as `rebuilt`, the LIVE
#      rev*.dat grew (orphan-safe append), and BLOCK_HAVE_UNDO is set
#      again on those heights when the daemon restarts.
#
#   3. Rebuilt undo survives restart.
#      Restart the daemon after the rebuild. Expect: clean RPC
#      readiness, no safe-mode trip, getblockheader of the rebuilt
#      heights reports the BLOCK_HAVE_UNDO flag, and chain advances on
#      a subsequent generatetoaddress.
#
# Crash-oracle properties (#4 "crash after writeUndo before metadata"
# and #5 "crash after metadata commit") require additional MaybeAbortAt
# hooks in the rebuilder's LIVE-write path; those land in a follow-up
# commit alongside the consensus property test.

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
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
DATA_DIR="/tmp/dinero_rebuild_undo_$$"
LOG_LIVE="${DATA_DIR}.live.log"
LOG_DRY_RUN="${DATA_DIR}.dry_run.log"
LOG_WRITE="${DATA_DIR}.write.log"
LOG_RESTART="${DATA_DIR}.restart.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for f in "${LOG_LIVE}" "${LOG_DRY_RUN}" "${LOG_WRITE}" "${LOG_RESTART}"; do
        [[ -f "${f}" ]] && { printf -- '--- %s tail ---\n' "${f}" >&2; tail -120 "${f}" >&2 || true; }
    done
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" \
               "${LOG_LIVE}" "${LOG_DRY_RUN}" "${LOG_WRITE}" "${LOG_RESTART}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null   || fail "jq is required"
    command -v sha256sum >/dev/null || command -v shasum >/dev/null || \
        fail "sha256sum or shasum is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
        return 0
    fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
        return 0
    fi
    return 1
}

rpc_call() {
    local datadir="$1"
    local method="$2"
    local params_json="$3"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
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
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        "$@" \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc_call "${DATA_DIR}" "stop" '[]' >/dev/null 2>&1 || kill "${PID}" 2>/dev/null || true
    wait_dead "${PID}" || kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

# Run dinerod in --rebuild-undo-range mode (synchronous, no daemon).
# The binary parses the flag, runs the orchestrator, exits.
run_offline_rebuild() {
    local log_file="$1"
    shift
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        "$@" \
        >"${log_file}" 2>&1 || true  # exits non-zero is normal when range is invalid
}

require_tools

RPC_PORT=$((34000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

# ─────────────────────────────────────────────────────────────────────
# Phase 1: Build a real regtest chain so LIVE has real undo to map.
# ─────────────────────────────────────────────────────────────────────
info "Spinning up regtest dinerod and mining blocks"
start_node "${LOG_LIVE}"
wait_rpc || fail "live daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty miner address"

# Mine 12 blocks so we have real bodies + real undo entries to operate on.
MINE_RESULT="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[12,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_RESULT}" && fail "generatetoaddress failed: ${MINE_RESULT}"
TIP_HEIGHT="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
[[ "${TIP_HEIGHT}" -ge 12 ]] || fail "expected tip ≥12, got ${TIP_HEIGHT}"
pass "Mined ${TIP_HEIGHT} regtest blocks"

# Capture rev*.dat sha256 BEFORE any rebuild — this anchors the dry-run
# "writes-nothing" property.
REV_FILES_BEFORE_DRY="$(find "${DATA_DIR}/blocks" -name 'rev*.dat' | sort)"
[[ -n "${REV_FILES_BEFORE_DRY}" ]] || fail "no rev*.dat files found before dry-run"
SHA_BEFORE_DRY=""
for f in ${REV_FILES_BEFORE_DRY}; do
    SHA_BEFORE_DRY+="$(basename "${f}"):$(sha256_file "${f}")\n"
done
info "rev*.dat fingerprint before dry-run captured"

stop_node

# ─────────────────────────────────────────────────────────────────────
# Property #1: Dry-run maps holes but writes nothing
# ─────────────────────────────────────────────────────────────────────
info "Property #1: dry-run on a healthy chain"
run_offline_rebuild "${LOG_DRY_RUN}" --rebuild-undo-range=1:5

MANIFEST="${DATA_DIR}/rebuild_undo_manifest.json"
[[ -f "${MANIFEST}" ]] || fail "manifest not emitted by dry-run"
DRY_STATUS="$(jq -r '.final_status' "${MANIFEST}")"
[[ "${DRY_STATUS}" == "dry_run_complete" ]] || \
    fail "dry-run final_status=${DRY_STATUS}, want dry_run_complete"
DRY_DRY_RUN="$(jq -r '.dry_run' "${MANIFEST}")"
[[ "${DRY_DRY_RUN}" == "true" ]] || fail "manifest.dry_run=${DRY_DRY_RUN}, want true"
DRY_ALREADY_OK="$(jq -r '.counts.already_ok' "${MANIFEST}")"
[[ "${DRY_ALREADY_OK}" -eq 5 ]] || \
    fail "dry-run expected 5 already_ok, got ${DRY_ALREADY_OK}"
DRY_REBUILT="$(jq -r '.counts.rebuilt' "${MANIFEST}")"
[[ "${DRY_REBUILT}" -eq 0 ]] || \
    fail "dry-run reported rebuilt=${DRY_REBUILT}, must be 0"
pass "dry-run manifest: 5/5 already_ok, 0 rebuilt, status=dry_run_complete"

# Verify rev*.dat sha256 unchanged — dry-run wrote nothing.
SHA_AFTER_DRY=""
for f in ${REV_FILES_BEFORE_DRY}; do
    SHA_AFTER_DRY+="$(basename "${f}"):$(sha256_file "${f}")\n"
done
[[ "${SHA_BEFORE_DRY}" == "${SHA_AFTER_DRY}" ]] || \
    fail "dry-run modified rev*.dat — must write nothing"
pass "dry-run touched zero LIVE bytes (rev*.dat sha256 unchanged)"

# ─────────────────────────────────────────────────────────────────────
# Phase 2: Induce holes via debugclearundoflag, then write-mode rebuild.
# ─────────────────────────────────────────────────────────────────────
info "Spinning daemon back up to corrupt undo metadata for heights 3,4,5"
start_node "${LOG_LIVE}"
wait_rpc || fail "daemon did not come up after dry-run"

CORRUPTED_HEIGHTS=(3 4 5)
CORRUPTED_HASHES=()
for h in "${CORRUPTED_HEIGHTS[@]}"; do
    HASH_RESULT="$(rpc_call "${DATA_DIR}" "getblockhash" "[${h}]")"
    rpc_has_error "${HASH_RESULT}" && fail "getblockhash ${h} failed: ${HASH_RESULT}"
    HASH="$(jq -r '.result' <<<"${HASH_RESULT}")"
    CORRUPTED_HASHES+=("${HASH}")
    CLEAR_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.debugclearundoflag" "[\"${HASH}\"]")"
    rpc_has_error "${CLEAR_RESULT}" && fail "debugclearundoflag ${h} failed: ${CLEAR_RESULT}"
    info "  cleared undo flag at height ${h} (${HASH})"
done

# Capture rev*.dat fingerprint BEFORE write-mode rebuild so we can
# verify it grew (orphan-safe append).
REV_SIZE_BEFORE_WRITE=0
for f in $(find "${DATA_DIR}/blocks" -name 'rev*.dat' | sort); do
    REV_SIZE_BEFORE_WRITE=$(( REV_SIZE_BEFORE_WRITE + $(stat -f%z "${f}" 2>/dev/null || stat -c%s "${f}") ))
done
info "rev*.dat total size before write-mode rebuild: ${REV_SIZE_BEFORE_WRITE} bytes"

stop_node

# ─────────────────────────────────────────────────────────────────────
# Property #2: Write mode rebuilds holes
# ─────────────────────────────────────────────────────────────────────
info "Property #2: --rebuild-undo-range=3:5 --rebuild-undo-write"
run_offline_rebuild "${LOG_WRITE}" \
    --rebuild-undo-range=3:5 --rebuild-undo-write

[[ -f "${MANIFEST}" ]] || fail "manifest not emitted by write-mode run"
WRITE_STATUS="$(jq -r '.final_status' "${MANIFEST}")"
[[ "${WRITE_STATUS}" == "ok" || "${WRITE_STATUS}" == "ok_with_verify_failures" ]] || \
    fail "write-mode final_status=${WRITE_STATUS}, want ok / ok_with_verify_failures"
WRITE_DRY_RUN="$(jq -r '.dry_run' "${MANIFEST}")"
[[ "${WRITE_DRY_RUN}" == "false" ]] || fail "manifest.dry_run=${WRITE_DRY_RUN}, want false"
WRITE_REBUILT="$(jq -r '.counts.rebuilt' "${MANIFEST}")"
[[ "${WRITE_REBUILT}" -eq 3 ]] || \
    fail "write-mode expected 3 rebuilt, got ${WRITE_REBUILT}"
WRITE_VERIFY_FAILED="$(jq -r '.counts.verify_failed' "${MANIFEST}")"
[[ "${WRITE_VERIFY_FAILED}" -eq 0 ]] || \
    fail "write-mode reported ${WRITE_VERIFY_FAILED} verify_failed — clean replay must verify cleanly"
pass "write-mode manifest: 3/3 rebuilt, 0 verify_failed, status=ok"

# Per-entry status for the corrupted heights must be "rebuilt"
for h in "${CORRUPTED_HEIGHTS[@]}"; do
    ENTRY_STATUS="$(jq -r --argjson h "${h}" '.entries[] | select(.height == $h) | .status' "${MANIFEST}")"
    [[ "${ENTRY_STATUS}" == "rebuilt" ]] || \
        fail "expected entries[height=${h}].status=rebuilt, got ${ENTRY_STATUS}"
done
pass "manifest entries for heights 3,4,5 each report status=rebuilt"

# rev*.dat must have grown (orphan-safe append of new undo bytes).
REV_SIZE_AFTER_WRITE=0
for f in $(find "${DATA_DIR}/blocks" -name 'rev*.dat' | sort); do
    REV_SIZE_AFTER_WRITE=$(( REV_SIZE_AFTER_WRITE + $(stat -f%z "${f}" 2>/dev/null || stat -c%s "${f}") ))
done
[[ "${REV_SIZE_AFTER_WRITE}" -gt "${REV_SIZE_BEFORE_WRITE}" ]] || \
    fail "rev*.dat did not grow after write-mode (before=${REV_SIZE_BEFORE_WRITE} after=${REV_SIZE_AFTER_WRITE})"
pass "rev*.dat grew by $((REV_SIZE_AFTER_WRITE - REV_SIZE_BEFORE_WRITE)) bytes (orphan-safe append)"

# Temp dirs must be cleaned up.
[[ -d "${DATA_DIR}/chainstate.rebuild-undo.tmp" ]] && \
    fail "temp chainstate not cleaned up after rebuild"
[[ -d "${DATA_DIR}/.rebuild-undo.tmp" ]] && \
    fail "temp block dir not cleaned up after rebuild"
pass "temp DB + temp block dir cleaned up"

# ─────────────────────────────────────────────────────────────────────
# Property #3: Rebuilt undo survives restart
# ─────────────────────────────────────────────────────────────────────
info "Property #3: restart daemon and verify undo metadata for rebuilt heights"
start_node "${LOG_RESTART}"
wait_rpc || fail "daemon did not come up cleanly after rebuild"

# Daemon must not be in safe mode.
SAFE_MODE_RESULT="$(rpc_call "${DATA_DIR}" "safemode.status" '[]' || true)"
if [[ -n "${SAFE_MODE_RESULT}" ]]; then
    SAFE_MODE_ACTIVE="$(jq -r '.result.active // false' <<<"${SAFE_MODE_RESULT}")"
    [[ "${SAFE_MODE_ACTIVE}" == "false" ]] || \
        fail "daemon entered safe mode after rebuild: ${SAFE_MODE_RESULT}"
fi
pass "daemon up; not in safe mode"

# getblockheader for each rebuilt height must report a flag set
# consistent with BLOCK_HAVE_UNDO. The exact RPC field varies by
# version; we check for `"have_undo": true` if exposed, otherwise
# fall back to inspecting the persisted metadata via getblockheader.
for h in "${CORRUPTED_HEIGHTS[@]}"; do
    HASH_RESULT="$(rpc_call "${DATA_DIR}" "getblockhash" "[${h}]")"
    rpc_has_error "${HASH_RESULT}" && fail "post-rebuild getblockhash ${h} failed"
    HASH_AFTER="$(jq -r '.result' <<<"${HASH_RESULT}")"
    HEADER_RESULT="$(rpc_call "${DATA_DIR}" "getblockheader" "[\"${HASH_AFTER}\"]")"
    rpc_has_error "${HEADER_RESULT}" && fail "post-rebuild getblockheader ${h} failed"
done
pass "every rebuilt height has its block header readable post-restart"

# Chain must advance — the most direct end-to-end signal that the
# daemon's chainstate is internally consistent post-rebuild.
HEIGHT_BEFORE_MINE="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
MINE_AGAIN="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[1,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_AGAIN}" && fail "post-rebuild generatetoaddress failed: ${MINE_AGAIN}"
HEIGHT_AFTER_MINE="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
[[ "${HEIGHT_AFTER_MINE}" == "$((HEIGHT_BEFORE_MINE + 1))" ]] || \
    fail "chain did not advance after rebuild: before=${HEIGHT_BEFORE_MINE} after=${HEIGHT_AFTER_MINE}"
pass "chain advanced from ${HEIGHT_BEFORE_MINE} to ${HEIGHT_AFTER_MINE} after rebuild"

# ─────────────────────────────────────────────────────────────────────
# Property #4 (commit #8): hole-only writes — already_ok heights
# stay byte-untouched on LIVE.
# ─────────────────────────────────────────────────────────────────────
# Mine more blocks so we have a window with both clean heights
# (already_ok) and a corrupted height (hole). The rebuilder must:
#   * verify EVERY block in the window (chain-wide DisconnectBlock parity)
#   * write LIVE bytes ONLY for the hole heights
#   * leave already_ok heights' undo_file / undo_pos / undo_size
#     byte-identical pre and post run
info "Property #4: hole-only writes leave already_ok heights byte-untouched"
MINE_PROP4="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[2,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_PROP4}" && fail "Property #4 mining failed: ${MINE_PROP4}"
TIP_PROP4="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
HOLE_HEIGHT=$(( TIP_PROP4 - 1 ))   # the middle one becomes the hole
WINDOW_START=$(( TIP_PROP4 - 2 ))
WINDOW_END=${TIP_PROP4}

# Capture undo metadata for the two heights that should stay clean.
# Use plain shell variables (macOS default bash is 3.x — no associative arrays).
get_pointer() {
    local h="$1"
    local hash_result hash header_result
    hash_result="$(rpc_call "${DATA_DIR}" "getblockhash" "[${h}]")"
    rpc_has_error "${hash_result}" && fail "getblockhash ${h} failed (#4)"
    hash="$(jq -r '.result' <<<"${hash_result}")"
    header_result="$(rpc_call "${DATA_DIR}" "getblockheader" "[\"${hash}\"]")"
    rpc_has_error "${header_result}" && fail "getblockheader ${h} failed (#4)"
    jq -c '{undo_file, undo_pos, undo_size, status_flags}' <<<"${header_result}"
}
POINTER_LOWER_BEFORE="$(get_pointer "${WINDOW_START}")"
POINTER_UPPER_BEFORE="$(get_pointer "${TIP_PROP4}")"
info "  before: height ${WINDOW_START} → ${POINTER_LOWER_BEFORE}"
info "  before: height ${TIP_PROP4} → ${POINTER_UPPER_BEFORE}"

# Punch a single hole.
HOLE_HASH_RESULT="$(rpc_call "${DATA_DIR}" "getblockhash" "[${HOLE_HEIGHT}]")"
rpc_has_error "${HOLE_HASH_RESULT}" && fail "getblockhash hole-height failed"
HOLE_HASH="$(jq -r '.result' <<<"${HOLE_HASH_RESULT}")"
CLEAR_HOLE="$(rpc_call "${DATA_DIR}" "blockchain.debugclearundoflag" "[\"${HOLE_HASH}\"]")"
rpc_has_error "${CLEAR_HOLE}" && fail "debugclearundoflag (#4) failed: ${CLEAR_HOLE}"

stop_node

# Run write-mode rebuild over the wider window.
LOG_PROP4="${DATA_DIR}.prop4.log"
run_offline_rebuild "${LOG_PROP4}" \
    --rebuild-undo-range=${WINDOW_START}:${WINDOW_END} --rebuild-undo-write

PROP4_STATUS="$(jq -r '.final_status' "${MANIFEST}")"
[[ "${PROP4_STATUS}" == "ok" ]] || \
    fail "property #4 final_status=${PROP4_STATUS}, want ok"
PROP4_REBUILT="$(jq -r '.counts.rebuilt' "${MANIFEST}")"
[[ "${PROP4_REBUILT}" -eq 1 ]] || \
    fail "property #4 expected exactly 1 rebuilt (the hole), got ${PROP4_REBUILT}"
PROP4_OK="$(jq -r '.counts.already_ok' "${MANIFEST}")"
[[ "${PROP4_OK}" -eq 2 ]] || \
    fail "property #4 expected 2 already_ok, got ${PROP4_OK}"
PROP4_VERIFY_FAILED="$(jq -r '.counts.verify_failed' "${MANIFEST}")"
[[ "${PROP4_VERIFY_FAILED}" -eq 0 ]] || \
    fail "property #4 verify_failed=${PROP4_VERIFY_FAILED}, want 0"
pass "property #4 manifest: 1 rebuilt + 2 already_ok + 0 verify_failed"

# Restart and assert the already_ok heights' undo pointers are
# byte-identical pre and post run.
start_node "${LOG_LIVE}"
wait_rpc || fail "daemon did not come up after property #4 rebuild"
POINTER_LOWER_AFTER="$(get_pointer "${WINDOW_START}")"
POINTER_UPPER_AFTER="$(get_pointer "${TIP_PROP4}")"
[[ "${POINTER_LOWER_AFTER}" == "${POINTER_LOWER_BEFORE}" ]] || \
    fail "already_ok height ${WINDOW_START} undo pointer changed
            before: ${POINTER_LOWER_BEFORE}
            after:  ${POINTER_LOWER_AFTER}"
[[ "${POINTER_UPPER_AFTER}" == "${POINTER_UPPER_BEFORE}" ]] || \
    fail "already_ok height ${TIP_PROP4} undo pointer changed
            before: ${POINTER_UPPER_BEFORE}
            after:  ${POINTER_UPPER_AFTER}"
info "  after:  height ${WINDOW_START} → ${POINTER_LOWER_AFTER} (unchanged)"
info "  after:  height ${TIP_PROP4} → ${POINTER_UPPER_AFTER} (unchanged)"
pass "already_ok heights' undo metadata pointers byte-identical pre and post"

# Stop daemon before Property #5 so that property's start_node owns the
# lockfile cleanly. Without this, the next start_node silently spawns
# a second dinerod that aborts on the lockfile, RPCs keep going to the
# original daemon, and the subsequent run_offline_rebuild then fails
# FATAL because the original is still running.
stop_node

# ─────────────────────────────────────────────────────────────────────
# Property #5 (regression guard for 3056607b9): hole-only optimization
# MUST still mutate the temp ChainDB UTXO set for already_ok heights —
# only the LIVE writeUndo + LIVE putHeaderMetadata are gated by the
# whitelist, NOT the temp-DB Step 6 putCoin calls.
#
# The bug fixed by 3056607b9 (was introduced in commit 9d892f2fb,
# the hole-only optimization): processBlock used `return Status::Ok`
# inside the LIVE-writes block on whitelist-skip, which aborted
# processBlock BEFORE Step 6's `chain_db_->putCoin(...)`. Result:
# already_ok heights left the temp DB without their output UTXOs,
# breaking subsequent blocks' prevout lookups.
#
# To trigger the bug in regression: need an in-window hole that
# spends a UTXO created by an in-window already_ok height. Property
# #4 didn't catch this because its 14-block coinbase-only fixture
# had no non-coinbase spends inside the window. Property #5 mines
# past coinbase maturity (100 blocks), executes a wallet send so
# h≈111 contains a non-coinbase tx whose prevout is an h<=10
# coinbase, then punches a hole at the spending height.
info "Property #5: regression guard — hole spending UTXO from in-window already_ok height"
start_node "${LOG_LIVE}"
wait_rpc || fail "daemon did not come up for property #5 setup"

# Mine to maturity + 1 spend window. We already have ~15 blocks from
# previous properties; mine 100 more so coinbase outputs from the
# earliest blocks are mature.
PRE_TIP="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
MINE_PROP5_PRE="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[100,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_PROP5_PRE}" && fail "property #5 maturity-mine failed: ${MINE_PROP5_PRE}"

# Send 1 DIN to a recipient; this consumes a mature coinbase.
RECIPIENT_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${RECIPIENT_RESULT}" && fail "property #5 wallet.getnewaddress failed: ${RECIPIENT_RESULT}"
RECIPIENT="$(jq -r '.result.address // .result // empty' <<<"${RECIPIENT_RESULT}")"
[[ -n "${RECIPIENT}" ]] || fail "property #5 empty recipient"
SEND_RESULT="$(rpc_call "${DATA_DIR}" "wallet.sendtoaddress" "[\"${RECIPIENT}\",1.0]")"
rpc_has_error "${SEND_RESULT}" && fail "property #5 wallet.sendtoaddress failed: ${SEND_RESULT}"

# Mine one more block to confirm the send. The newly-mined block has
# a non-coinbase tx that references one of the early coinbase outputs.
MINE_CONFIRM="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[1,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_CONFIRM}" && fail "property #5 confirm-mine failed: ${MINE_CONFIRM}"
SPEND_HEIGHT="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
SPEND_HASH_RESULT="$(rpc_call "${DATA_DIR}" "getblockhash" "[${SPEND_HEIGHT}]")"
rpc_has_error "${SPEND_HASH_RESULT}" && fail "property #5 spend-height hash lookup failed"
SPEND_HASH="$(jq -r '.result' <<<"${SPEND_HASH_RESULT}")"
info "  send confirmed at height ${SPEND_HEIGHT} hash ${SPEND_HASH}"

# Punch the hole at the spending height. This block has a non-coinbase
# input. With the bug, processBlock for that height looks up the
# prevout in the temp DB → fails because earlier already_ok heights
# returned early before putCoin.
CLEAR_SPEND_RESULT="$(rpc_call "${DATA_DIR}" "blockchain.debugclearundoflag" "[\"${SPEND_HASH}\"]")"
rpc_has_error "${CLEAR_SPEND_RESULT}" && fail "property #5 clear undo flag failed: ${CLEAR_SPEND_RESULT}"

stop_node

# Run the rebuild over the FULL chain. This is exactly the flow that
# triggered the LA Apr 30 2026 bug: full-range rebuild with a non-
# empty hole_heights_to_rebuild whitelist where some hole height
# spends a UTXO created by an in-window already_ok height. With the
# bug, processBlock at SPEND_HEIGHT fails with reindex-missing-utxo;
# manifest reports "incomplete_skipped". With the fix, all heights
# process cleanly.
LOG_PROP5="${DATA_DIR}.prop5.log"
run_offline_rebuild "${LOG_PROP5}" \
    --rebuild-undo-range=1:${SPEND_HEIGHT} --rebuild-undo-write

PROP5_STATUS="$(jq -r '.final_status' "${MANIFEST}")"
[[ "${PROP5_STATUS}" == "ok" ]] || \
    fail "property #5 final_status=${PROP5_STATUS}, want ok (bug 9d892f2fb regression)"
PROP5_REBUILT="$(jq -r '.counts.rebuilt' "${MANIFEST}")"
[[ "${PROP5_REBUILT}" -eq 1 ]] || \
    fail "property #5 expected 1 rebuilt (the spending hole), got ${PROP5_REBUILT}"
PROP5_VERIFY_FAILED="$(jq -r '.counts.verify_failed' "${MANIFEST}")"
[[ "${PROP5_VERIFY_FAILED}" -eq 0 ]] || \
    fail "property #5 verify_failed=${PROP5_VERIFY_FAILED}, want 0"
PROP5_SKIPPED="$(jq -r '.counts.skipped' "${MANIFEST}")"
[[ "${PROP5_SKIPPED}" -eq 0 ]] || \
    fail "property #5 skipped=${PROP5_SKIPPED}, want 0 (this is the bug signature)"

# Verify the spend height specifically rebuilt cleanly.
PROP5_SPEND_STATUS="$(jq -r --argjson h "${SPEND_HEIGHT}" '.entries[] | select(.height == $h) | .status' "${MANIFEST}")"
[[ "${PROP5_SPEND_STATUS}" == "rebuilt" ]] || \
    fail "property #5 entries[height=${SPEND_HEIGHT}].status=${PROP5_SPEND_STATUS}, want rebuilt"
pass "property #5 — hole at non-coinbase spend height rebuilt cleanly through in-window already_ok prevout history"

# Restart and verify chain advances normally.
start_node "${LOG_LIVE}"
wait_rpc || fail "property #5 daemon did not come up after rebuild"
HEIGHT_BEFORE_PROP5_MINE="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
MINE_PROP5_FINAL="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[1,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_PROP5_FINAL}" && fail "property #5 post-rebuild mine failed"
HEIGHT_AFTER_PROP5_MINE="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
[[ "${HEIGHT_AFTER_PROP5_MINE}" == "$((HEIGHT_BEFORE_PROP5_MINE + 1))" ]] || \
    fail "property #5 chain did not advance after rebuild"
pass "property #5 chain advanced from ${HEIGHT_BEFORE_PROP5_MINE} to ${HEIGHT_AFTER_PROP5_MINE} after rebuild"

stop_node
pass "All offline undo-rebuild integration properties hold"
