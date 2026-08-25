#!/usr/bin/env bash
# Regression test for #458 — the extranonce must survive a RESTART.
#
# A process-lifetime counter alone is not sufficient. It resets to zero when the
# daemon restarts, and the block timestamp cannot be relied on to differ:
#
#     block_time = max(prevMTP + 1, wall clock)
#
# Once the chain's median time past runs ahead of wall clock, this collapses to
# the constant prevMTP + 1 — completely independent of wall clock. Waiting does
# not change it, and neither does a restart of any duration. With the same
# parent, the same frozen timestamp, a reset counter and a nonce search that
# always starts at 0, a restarted node rebuilds the invalidated block exactly.
#
# This test invalidates the tip, RESTARTS the daemon, mines from the same
# parent, and requires the replacement to differ.
#
# ── WHAT ACTUALLY GATES THIS TEST ────────────────────────────────────────────
#
# The MERKLE ROOT is the regression gate. The block hash and the timestamps are
# INFORMATIVE ONLY and must never be treated as the gate.
#
# Why the merkle root is decisive:
#   - both blocks are mined at the same height on the same parent
#   - both are mined by a freshly started process, so both use counter=0
#   - everything else (address, subsidy, no mempool txs) is identical
#   => the coinbase differs ONLY by the extranonce session id, and the merkle
#      root is derived from the coinbase. It does not depend on the timestamp.
#
# Why the block hash is NOT sufficient:
#   A restart takes several seconds while the frozen-time margin is ~1s, so the
#   wall clock usually overtakes prevMTP+1 during the restart. The two blocks
#   then get different timestamps and their hashes differ for a reason that has
#   nothing to do with the extranonce.
#
# NEUTER EVIDENCE (do not delete — this is what proves the gate works).
# With the session id neutered to a constant, both counters at 0:
#
#     target merkle = 931dcc4a59ef336388f6957cf37926469222dcfe9b40b25b8e9a8d52c37200f5
#     new merkle    = 931dcc4a59ef336388f6957cf37926469222dcfe9b40b25b8e9a8d52c37200f5
#
# byte-identical coinbases — while "replacement hash differs" still PASSED.
# Re-confirmed after the frozen-time precondition was removed:
#
#     [PASS] replacement hash differs from the invalidated block across a restart
#     [FAIL] merkle roots are identical (240688fd1d54d508f9ec...)
#
# Restoring the session id makes the merkle roots differ, independently of the
# timestamps. If a future change makes the merkle assertion non-decisive, this
# test stops guarding #458 even while appearing green.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_mine_invalidate_restart_$$"
LOG_A="${DATA_DIR}.a.log"
LOG_B="${DATA_DIR}.b.log"
LOG_C="${DATA_DIR}.c.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

# Enough blocks that each subsequent timestamp is forced to prevMTP + 1 and the
# chain's notion of time overtakes the wall clock.
SEED_BLOCKS=40

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for L in "${LOG_A}" "${LOG_B}" "${LOG_C}"; do
        [[ -f "$L" ]] && { printf -- '--- %s tail ---\n' "$L" >&2; tail -40 "$L" >&2 || true; }
    done
    exit 1
}

cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    [[ "${KEEP_ON_FAIL}" != "1" ]] && rm -rf "${DATA_DIR}" "${LOG_A}" "${LOG_B}" "${LOG_C}"
    return 0
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT=$(alloc_port_base)
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

cookie_file() {
    [[ -f "${DATA_DIR}/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/.cookie"; return 0; }
    [[ -f "${DATA_DIR}/regtest/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/regtest/.cookie"; return 0; }
    return 1
}

rpc_call() {
    local cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    curl -s --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

# Structural: a well-formed JSON-RPC reply reports failure in the TOP-LEVEL
# "error" member. #458 also fixed the dispatcher that used to bury handler
# errors inside "result", so this no longer needs substring heuristics.
rpc_failed() {
    local r="$1"
    [[ -z "${r}" ]] && return 0
    jq -e '.error != null' >/dev/null 2>&1 <<<"${r}" && return 0
    return 1
}

start_node() {
    "${DINEROD}" --regtest --datadir="${DATA_DIR}" --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 >"$1" 2>&1 &
    PID=$!
    for _ in $(seq 1 90); do
        rpc_call getblockcount '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    rpc_call stop '[]' >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do
        kill -0 "${PID}" 2>/dev/null || { wait "${PID}" 2>/dev/null || true; PID=""; return 0; }
        sleep 1
    done
    fail "daemon did not exit cleanly"
}

block_count() { jq -r '.result' <<<"$(rpc_call getblockcount '[]')"; }

hash_at_height() {
    local r v
    r="$(rpc_call getblockhash "[$1]")"
    rpc_failed "${r}" && return 1
    v="$(jq -r 'if (.result | type) == "string" then .result else empty end' <<<"${r}")"
    [[ "${v}" =~ ^[0-9a-fA-F]{64}$ ]] || return 1
    printf '%s\n' "${v}"
}

header_merkle() {
    local r
    r="$(rpc_call getblockheader "[\"$1\"]")"
    rpc_failed "${r}" && fail "getblockheader failed for $1: ${r}"
    jq -r '.result.merkleroot' <<<"${r}"
}

header_time() {
    local r
    r="$(rpc_call getblockheader "[\"$1\"]")"
    rpc_failed "${r}" && fail "getblockheader failed for $1: ${r}"
    jq -r '.result.time' <<<"${r}"
}

mkdir -p "${DATA_DIR}"
info "Starting daemon"
start_node "${LOG_A}" || fail "daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call wallet.getnewaddress '[]')"
rpc_failed "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty mining address"

info "Mining ${SEED_BLOCKS} blocks to drive median time past ahead of wall clock"
MINE="$(rpc_call generatetoaddress "[${SEED_BLOCKS},\"${MINER_ADDR}\"]")"
rpc_failed "${MINE}" && fail "seed mine failed: ${MINE}"

SEEDED_HEIGHT="$(block_count)"
[[ "${SEEDED_HEIGHT}" == "${SEED_BLOCKS}" ]] || fail "expected height ${SEED_BLOCKS}, got ${SEEDED_HEIGHT}"

# CRITICAL SEQUENCING — the target block must be mined at the SAME extranonce
# counter value the restarted node will use, otherwise this test cannot detect a
# process-only counter.
#
# The seed blocks above consumed counter values 0..N-1. If the target were the
# last seed block, it would carry counter=N-1 while the post-restart
# replacement carries counter=0 — they would differ for that reason alone, and
# the test would pass even with a constant session id. (Verified: an earlier
# revision did exactly this and a neutered build still passed.)
#
# So restart FIRST, then mine the target as the first block of a fresh process
# (counter=0). The replacement after the second restart is also counter=0. Same
# parent, same frozen timestamp, same counter — the ONLY remaining difference
# is the per-process random session id.
info "Restarting so the target block is mined at counter=0"
stop_node
start_node "${LOG_B}" || fail "daemon did not restart before mining the target"

MINE_TARGET="$(rpc_call generatetoaddress "[1,\"${MINER_ADDR}\"]")"
rpc_failed "${MINE_TARGET}" && fail "mining the target block failed: ${MINE_TARGET}"

TIP_HEIGHT="$(block_count)"
[[ "${TIP_HEIGHT}" == "$((SEEDED_HEIGHT + 1))" ]] \
    || fail "expected height $((SEEDED_HEIGHT + 1)) after mining target, got ${TIP_HEIGHT}"

TARGET_HASH="$(hash_at_height "${TIP_HEIGHT}")" || fail "getblockhash at tip failed"
TARGET_TIME="$(header_time "${TARGET_HASH}")"
TARGET_MERKLE="$(header_merkle "${TARGET_HASH}")"
NOW="$(date +%s)"
info "tip height=${TIP_HEIGHT} time=${TARGET_TIME} wall_clock=${NOW}"

# INFORMATIONAL, deliberately not a hard precondition.
#
# Whether the chain's time has overtaken the wall clock depends on how fast the
# host mines the seed blocks. On Linux CI 40 blocks complete inside one second,
# so tip time == wall clock and a strict "> NOW" check fails there while passing
# on slower hardware — a hardware-speed dependency, not a property of the code
# under test.
#
# It does not need to be a precondition, because the assertion that gates this
# test is the MERKLE ROOT, which does not depend on the timestamp at all.
# Demonstrated directly: in the neutered run that proved this test works, the
# timestamps DIFFERED (1785486203 -> 1785486209) and the merkle-root assertion
# still caught the constant session id. Frozen time only strengthens the
# secondary block-hash assertion; it is not required for the gate.
if (( TARGET_TIME > NOW )); then
    info "block_time is frozen at prevMTP+1 (tip time ${TARGET_TIME} > wall clock ${NOW}) — the block-hash assertion is also isolated in this run"
else
    info "block_time is not frozen (tip time ${TARGET_TIME} <= wall clock ${NOW}); the merkle-root assertion below is timestamp-independent and still gates this test"
fi

PARENT_HASH="$(hash_at_height $((TIP_HEIGHT - 1)))" || fail "getblockhash at parent failed"
info "target=${TARGET_HASH}"
info "parent=${PARENT_HASH}"

info "Invalidating the tip"
INV="$(rpc_call invalidateblock "[\"${TARGET_HASH}\"]")"
rpc_failed "${INV}" && fail "invalidateblock failed: ${INV}"
[[ "$(block_count)" == "$((TIP_HEIGHT - 1))" ]] \
    || fail "expected rollback to $((TIP_HEIGHT - 1)), got $(block_count)"

# ---- restart: the extranonce counter resets to zero here --------------------
info "Restarting the daemon (any process-lifetime counter resets)"
stop_node
start_node "${LOG_C}" || fail "daemon did not reach RPC readiness after restart"

[[ "$(block_count)" == "$((TIP_HEIGHT - 1))" ]] \
    || fail "height changed across restart: expected $((TIP_HEIGHT - 1)), got $(block_count)"

info "Mining one block from the same parent after restart"
MINE_B="$(rpc_call generatetoaddress "[1,\"${MINER_ADDR}\"]")"
if rpc_failed "${MINE_B}"; then
    fail "mining after invalidate+restart failed: ${MINE_B}
The replacement block collided with the invalidated one, which carries BLOCK_FAILED_VALID and can never activate. A process-lifetime extranonce is not enough: it resets on restart, and block_time is frozen at prevMTP+1."
fi

[[ "$(block_count)" == "${TIP_HEIGHT}" ]] \
    || fail "chain did not extend after restart: got $(block_count), expected ${TIP_HEIGHT}"

NEW_HASH="$(hash_at_height "${TIP_HEIGHT}")" || fail "getblockhash after restart mine failed"
NEW_TIME="$(header_time "${NEW_HASH}")"
NEW_MERKLE="$(header_merkle "${NEW_HASH}")"
NEW_PARENT="$(rpc_call getblockheader "[\"${NEW_HASH}\"]" | jq -r '.result.previousblockhash')"

info "new hash=${NEW_HASH}"
info "new time=${NEW_TIME} (target time was ${TARGET_TIME})"
info "new merkle=${NEW_MERKLE}"
info "target merkle=${TARGET_MERKLE}"

[[ "${NEW_PARENT}" == "${PARENT_HASH}" ]] \
    || fail "replacement block does not build on the same parent (${NEW_PARENT} != ${PARENT_HASH}); this test is not comparing like with like"
pass "replacement block builds on the same parent"

# The heart of it: same parent, same timestamp, different block.
[[ "${NEW_HASH}" != "${TARGET_HASH}" ]] \
    || fail "the restarted node reproduced the invalidated block (${TARGET_HASH}). The extranonce is not unique across restarts."
pass "replacement hash differs from the invalidated block across a restart"

# THE DECISIVE, TIMESTAMP-INDEPENDENT ASSERTION.
#
# The block hash alone is a weak gate here: if the wall clock happens to
# overtake the frozen prevMTP+1 during the restart, the two blocks get
# different timestamps and their hashes differ for that reason alone — a
# constant session id would still pass. The restart takes several seconds and
# the frozen-time margin is typically ~1s, so that is the COMMON case, not a
# corner case.
#
# The merkle root does not depend on the timestamp. It is derived from the
# coinbase, which is exactly where the extranonce lives. Both blocks here are
# mined at counter=0 from the same parent for the same address with the same
# subsidy, so with a process-only (or constant) session id their coinbases
# would be byte-identical and their merkle roots would MATCH.
#
# Requiring the merkle roots to differ therefore gates the per-process random
# session id directly, whatever the clock did.
[[ "${NEW_MERKLE}" != "${TARGET_MERKLE}" ]] \
    || fail "merkle roots are identical (${TARGET_MERKLE}) — the coinbases are byte-identical, so the extranonce did NOT vary across the restart. A process-lifetime counter resets to zero and cannot distinguish these two blocks; a per-process random session id is required."
pass "merkle roots differ — the coinbase extranonce varied across the restart"

if [[ "${NEW_TIME}" == "${TARGET_TIME}" ]]; then
    pass "timestamps are also identical (${NEW_TIME}) — block_time stayed frozen across the restart"
else
    info "timestamps differ (${TARGET_TIME} -> ${NEW_TIME}); the merkle-root assertion above is what gates this test, and it is timestamp-independent"
fi

stop_node
pass "#458 mine-after-invalidate-restart regression test completed"
