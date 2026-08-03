#!/usr/bin/env bash
# Regression test for #458 — a rolled-back chain must be extendable.
#
# generatetoaddress builds its header from
#   block_time = max(prevMTP + 1, wall clock)      (ONE-second granularity)
# and searches nonces from 0. Without a coinbase extranonce, rebuilding on the
# same parent within one second reproduced a byte-identical block.
#
# That made invalidateblock a dead end:
#
#   mine 16 -> invalidateblock(hash@12) -> height 11
#   generatetoaddress(N) -> re-mines the SAME hash just invalidated, which
#   carries BLOCK_FAILED_VALID, so BlockAcceptor skips AddCandidate and it can
#   never activate. The chain was stuck at 11 forever.
#
# The invalidation guard is correct and must stay. What this test pins is that
# the MINER produces a distinct block, so a competing branch can be built.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_mine_after_invalidate_$$"
LOG="${DATA_DIR}.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

SEED_BLOCKS=16
FORK_HEIGHT=12
BRANCH_B_BLOCKS=9

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG}" ]] && { printf -- '--- daemon log tail ---\n' >&2; tail -60 "${LOG}" >&2 || true; }
    exit 1
}

# Cleanup must never change the verdict.
#
# This trap used to do:
#     kill "${PID}"; pkill -f ...; rm -rf "${DATA_DIR}" "${LOG}"; return 0
#
# kill/pkill only SEND a signal -- they return immediately, without waiting for
# the process to die. So `rm -rf` raced a dinerod that was still writing its
# datadir and failed with "Directory not empty". Under `set -e` that failure
# aborted the function BEFORE `return 0`, and because this is an EXIT trap, the
# trap's status became the script's status. A run whose every assertion passed
# reported failure. It only passed when the daemon happened to die fast enough.
#
# Two independent fixes, because either alone still leaves a way to lie:
#   1. wait for the process to actually exit (escalating to SIGKILL), so the
#      datadir is quiescent before removal;
#   2. capture the real exit status first and restore it last, and never let a
#      cleanup command's status escape. Cleanup can no longer turn a pass into
#      a failure -- nor a failure into a pass.
wait_for_exit() {
    local pid="$1" deadline=$((SECONDS + 10))
    while kill -0 "${pid}" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            kill -9 "${pid}" 2>/dev/null || true
            break
        fi
        sleep 0.1
    done
    wait "${pid}" 2>/dev/null || true
}

cleanup() {
    local rc=$?   # MUST be the first statement: preserves the real verdict

    if [[ -n "${PID}" ]]; then
        kill "${PID}" 2>/dev/null || true
        wait_for_exit "${PID}"
    fi

    # Any stray daemon still bound to this datadir, then wait for it too.
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    local deadline=$((SECONDS + 10))
    while pgrep -f "dinerod.*${DATA_DIR}" >/dev/null 2>&1; do
        if (( SECONDS >= deadline )); then
            pkill -9 -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
            break
        fi
        sleep 0.1
    done

    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        # `|| true`: a cleanup hiccup must not mask the verdict in rc.
        rm -rf "${DATA_DIR}" "${LOG}" 2>/dev/null || true
    fi

    return "${rc}"
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

RPC_PORT=$((46000 + RANDOM % 500))
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

# Detects the nested-error shape too: generatetoaddress reports activation
# failure inside "result", with the top-level "error" still null. A helper that
# only checks the top level reports a totally failed mine as success — that is
# the second half of #458 and is exactly what hid this bug.
rpc_failed() {
    local compact
    compact="$(echo "$1" | tr -d '\n\t ')"
    [[ -z "${compact}" ]] && return 0
    [[ "${compact}" == *"\"error\":{"* ]] && return 0
    return 1
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

mkdir -p "${DATA_DIR}"
"${DINEROD}" --regtest --datadir="${DATA_DIR}" --rpcport="${RPC_PORT}" \
    --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
    --listen=0 --utreexo=1 --p2p.offline=1 >"${LOG}" 2>&1 &
PID=$!

for _ in $(seq 1 90); do
    rpc_call getblockcount '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1 && break
    sleep 1
done
[[ "$(block_count)" =~ ^[0-9]+$ ]] || fail "daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call wallet.getnewaddress '[]')"
rpc_failed "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty mining address"

info "Mining ${SEED_BLOCKS} blocks"
MINE="$(rpc_call generatetoaddress "[${SEED_BLOCKS},\"${MINER_ADDR}\"]")"
rpc_failed "${MINE}" && fail "seed mine failed: ${MINE}"
[[ "$(block_count)" == "${SEED_BLOCKS}" ]] || fail "expected height ${SEED_BLOCKS}, got $(block_count)"

TARGET_HASH="$(hash_at_height "${FORK_HEIGHT}")" || fail "getblockhash at ${FORK_HEIGHT} failed"
info "target@${FORK_HEIGHT} = ${TARGET_HASH}"

info "Invalidating height ${FORK_HEIGHT}"
INV="$(rpc_call invalidateblock "[\"${TARGET_HASH}\"]")"
rpc_failed "${INV}" && fail "invalidateblock failed: ${INV}"

HEIGHT_AFTER_INV="$(block_count)"
[[ "${HEIGHT_AFTER_INV}" == "$((FORK_HEIGHT - 1))" ]] \
    || fail "expected rollback to $((FORK_HEIGHT - 1)), got ${HEIGHT_AFTER_INV}"
pass "invalidateblock rolled the active chain back to ${HEIGHT_AFTER_INV}"

# ---- the regression itself -------------------------------------------------
info "Mining ${BRANCH_B_BLOCKS} blocks on the rolled-back chain"
MINE_B="$(rpc_call generatetoaddress "[${BRANCH_B_BLOCKS},\"${MINER_ADDR}\"]")"

if rpc_failed "${MINE_B}"; then
    fail "mining after invalidateblock failed: ${MINE_B}
This is #458: the miner rebuilt a byte-identical block within the same second, reproducing the hash that was just invalidated. That block carries BLOCK_FAILED_VALID, so it is skipped from AddCandidate and can never activate."
fi

HEIGHT_AFTER_B="$(block_count)"
EXPECTED=$((HEIGHT_AFTER_INV + BRANCH_B_BLOCKS))
info "height after mining branch B: ${HEIGHT_AFTER_B} (expected ${EXPECTED})"
[[ "${HEIGHT_AFTER_B}" == "${EXPECTED}" ]] \
    || fail "rolled-back chain did not extend: ${HEIGHT_AFTER_INV} -> ${HEIGHT_AFTER_B}, expected ${EXPECTED}"
pass "rolled-back chain extended to height ${HEIGHT_AFTER_B}"

# The new block at the fork height must be a DIFFERENT block, not the
# invalidated one re-mined.
NEW_AT_FORK="$(hash_at_height "${FORK_HEIGHT}")" || fail "getblockhash at ${FORK_HEIGHT} failed after mining B"
info "hash@${FORK_HEIGHT} after fork = ${NEW_AT_FORK}"
[[ "${NEW_AT_FORK}" != "${TARGET_HASH}" ]] \
    || fail "the miner reproduced the invalidated block (${TARGET_HASH}) instead of building a new one — the coinbase extranonce is not varying"
pass "the competing branch is a genuinely different block"

# Every block on branch B must be distinct — a fixed extranonce would collide
# again as soon as two blocks share a second.
BLOCK_HASHES=""
for h in $(seq "${FORK_HEIGHT}" "${HEIGHT_AFTER_B}"); do
    BLOCK_HASHES="${BLOCK_HASHES}$(hash_at_height "${h}")"$'\n'
done
UNIQ_COUNT="$(printf '%s' "${BLOCK_HASHES}" | sort -u | grep -c .)"
TOTAL_COUNT="$(printf '%s' "${BLOCK_HASHES}" | grep -c .)"
[[ "${UNIQ_COUNT}" == "${TOTAL_COUNT}" ]] \
    || fail "branch B contains duplicate block hashes (${UNIQ_COUNT} unique of ${TOTAL_COUNT})"
pass "all ${TOTAL_COUNT} blocks on the competing branch are distinct"

rpc_call stop '[]' >/dev/null 2>&1 || true
pass "#458 mine-after-invalidate regression test completed"
