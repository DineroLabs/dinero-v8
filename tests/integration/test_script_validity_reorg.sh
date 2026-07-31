#!/usr/bin/env bash
# Regression test for #453 — BLOCK_VALID_SCRIPTS is a durable, MONOTONIC
# validation result. It must not be revoked when a block leaves the active
# chain, and it must not be granted to blocks that were never script-validated.
#
# Script validity and chain eligibility are separate facts. A block whose
# scripts validated successfully keeps that fact even after a competing branch
# with more chainwork displaces it, and keeps it across a restart.
#
# The disconnect here is caused by CHAINWORK, not by administrative
# invalidation: the target block is temporarily invalidated only to let a
# longer competing branch be built, then reconsidered. Once reconsidered it is
# a perfectly valid block that simply lost the fork race — exactly the state
# whose script validity must survive.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_script_reorg_$$"
LOG_FIRST="${DATA_DIR}.first.log"
LOG_SECOND="${DATA_DIR}.second.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
CURRENT_DATADIR=""
CURRENT_RPC_PORT=""

BLOCK_VALID_SCRIPTS=16

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_FIRST}" ]] && { printf -- '--- first-run log tail ---\n' >&2; tail -60 "${LOG_FIRST}" >&2 || true; }
    [[ -f "${LOG_SECOND}" ]] && { printf -- '--- second-run log tail ---\n' >&2; tail -60 "${LOG_SECOND}" >&2 || true; }
    exit 1
}

cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_FIRST}" "${LOG_SECOND}"
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
    local method="$1" params_json="$2" cookie_path cookie
    cookie_path="$(cookie_file "${CURRENT_DATADIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${CURRENT_RPC_PORT}/"
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
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
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

start_node() {
    local log_file="$1"
    mkdir -p "${DATA_DIR}"
    CURRENT_DATADIR="${DATA_DIR}"
    CURRENT_RPC_PORT="${RPC_PORT}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node_clean() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${stop_result}" ]] && rpc_has_error "${stop_result}"; then
        fail "stop RPC errored, cannot assert clean-restart durability: ${stop_result}"
    fi
    wait_dead "${PID}" || fail "daemon did not exit cleanly after stop RPC"
    wait "${PID}" 2>/dev/null || true
    PID=""
}

status_flags_for() {
    local hash="$1" result
    result="$(rpc_call "getblockheader" "[\"${hash}\"]")"
    rpc_has_error "${result}" && fail "getblockheader failed for ${hash}: ${result}"
    jq -r '.result.status_flags' <<<"${result}"
}

hash_at_height() {
    local height="$1" result
    result="$(rpc_call "getblockhash" "[${height}]")"
    rpc_has_error "${result}" && fail "getblockhash failed at height ${height}: ${result}"
    jq -r '.result' <<<"${result}"
}

block_count() {
    local result
    result="$(rpc_call "getblockcount" '[]')"
    jq -r '.result' <<<"${result}"
}

require_tools

RPC_PORT=$((42000 + RANDOM % 500))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

info "Starting daemon"
start_node "${LOG_FIRST}"
wait_rpc || fail "daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty mining address"

# ------------------------------------------------- 1. connect a valid block
info "Mining branch A (16 blocks)"
MINE="$(rpc_call "generatetoaddress" "[16,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE}" && fail "generatetoaddress failed: ${MINE}"

FORK_HEIGHT=12
TARGET_HASH="$(hash_at_height "${FORK_HEIGHT}")"
TARGET_FLAGS_CONNECTED="$(status_flags_for "${TARGET_HASH}")"
info "target block height ${FORK_HEIGHT} hash ${TARGET_HASH}"
info "status_flags while connected: ${TARGET_FLAGS_CONNECTED}"

if (( (TARGET_FLAGS_CONNECTED & BLOCK_VALID_SCRIPTS) == 0 )); then
    fail "precondition: connected block lacks BLOCK_VALID_SCRIPTS (${TARGET_FLAGS_CONNECTED})"
fi
pass "connected valid block carries BLOCK_VALID_SCRIPTS"

# --------------------------------- 2. build a competing branch that displaces it
# invalidateblock is used only as the mechanism to roll back and build a
# competing branch. It is reversed immediately afterwards, so the final state
# is "valid block that lost the fork race", not "administratively invalid".
info "Invalidating height ${FORK_HEIGHT} to roll back and fork"
INV="$(rpc_call "invalidateblock" "[\"${TARGET_HASH}\"]")"
rpc_has_error "${INV}" && fail "invalidateblock failed: ${INV}"

HEIGHT_AFTER_INVALIDATE="$(block_count)"
info "height after invalidate: ${HEIGHT_AFTER_INVALIDATE}"

info "Mining competing branch B (9 blocks) to outweigh branch A"
MINE_B="$(rpc_call "generatetoaddress" "[9,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_B}" && fail "generatetoaddress (branch B) failed: ${MINE_B}"
info "branch B mine response: $(echo "${MINE_B}" | tr -d '\n' | cut -c1-300)"

# Diagnose the fork state BEFORE reconsidering, so a premise failure points at
# the step that actually broke rather than at the end state.
HEIGHT_AFTER_B="$(block_count)"
HASH_AT_FORK_AFTER_B="$(hash_at_height "${FORK_HEIGHT}")"
info "after mining B: height=${HEIGHT_AFTER_B}, hash@${FORK_HEIGHT}=${HASH_AT_FORK_AFTER_B}"
if [[ "${HASH_AT_FORK_AFTER_B}" == "${TARGET_HASH}" ]]; then
    fail "branch B did not fork: height ${FORK_HEIGHT} still holds the target hash after invalidate+mine. invalidateblock did not roll the active chain back as expected."
fi
if (( HEIGHT_AFTER_B <= 16 )); then
    fail "branch B (height ${HEIGHT_AFTER_B}) does not outweigh branch A (16); mine more blocks on B"
fi

info "Reconsidering the target so it is valid-but-outweighed"
REC="$(rpc_call "reconsiderblock" "[\"${TARGET_HASH}\"]")"
rpc_has_error "${REC}" && fail "reconsiderblock failed: ${REC}"

FINAL_HEIGHT="$(block_count)"
info "active height after reorg: ${FINAL_HEIGHT}"

# Confirm the target really is OFF the active chain now.
ACTIVE_AT_FORK="$(hash_at_height "${FORK_HEIGHT}")"
if [[ "${ACTIVE_AT_FORK}" == "${TARGET_HASH}" ]]; then
    fail "competing branch did not displace the target; test premise not met (active at ${FORK_HEIGHT} is still ${TARGET_HASH})"
fi
pass "competing branch displaced the target block"

# ---------------------------- 3. disconnected block retains script validity
TARGET_FLAGS_DISCONNECTED="$(status_flags_for "${TARGET_HASH}")"
info "status_flags while disconnected: ${TARGET_FLAGS_DISCONNECTED}"

if (( (TARGET_FLAGS_DISCONNECTED & BLOCK_VALID_SCRIPTS) == 0 )); then
    fail "disconnected-but-valid block LOST BLOCK_VALID_SCRIPTS: ${TARGET_FLAGS_CONNECTED} -> ${TARGET_FLAGS_DISCONNECTED}. Script validity is a durable validation result and must not be revoked by losing a fork race."
fi
pass "disconnected block retains BLOCK_VALID_SCRIPTS"

# ----------------------------------- 4. and still retains it after restart
info "Restarting daemon"
stop_node_clean
start_node "${LOG_SECOND}"
wait_rpc || fail "daemon did not reach RPC readiness after restart"

TARGET_FLAGS_RESTART="$(status_flags_for "${TARGET_HASH}")"
info "status_flags after restart: ${TARGET_FLAGS_RESTART}"

if (( (TARGET_FLAGS_RESTART & BLOCK_VALID_SCRIPTS) == 0 )); then
    fail "disconnected block lost BLOCK_VALID_SCRIPTS across restart: ${TARGET_FLAGS_DISCONNECTED} -> ${TARGET_FLAGS_RESTART}"
fi
if [[ "${TARGET_FLAGS_DISCONNECTED}" != "${TARGET_FLAGS_RESTART}" ]]; then
    fail "disconnected block status_flags changed across restart: ${TARGET_FLAGS_DISCONNECTED} -> ${TARGET_FLAGS_RESTART}"
fi
pass "disconnected block retains BLOCK_VALID_SCRIPTS across restart"

# ------------- 5. membership does not grant the bit to unvalidated blocks
# Genesis is on the active chain but its scripts are never validated by the
# connect path. If any fix ever backfilled the bit from chain membership, this
# is where it would appear.
GENESIS_HASH="$(hash_at_height 0)"
GENESIS_FLAGS="$(status_flags_for "${GENESIS_HASH}")"
info "genesis status_flags: ${GENESIS_FLAGS}"
if (( (GENESIS_FLAGS & BLOCK_VALID_SCRIPTS) != 0 )); then
    fail "genesis acquired BLOCK_VALID_SCRIPTS (${GENESIS_FLAGS}) — active-chain membership must never grant script validity"
fi
pass "active-chain membership does not grant BLOCK_VALID_SCRIPTS"

stop_node_clean
pass "#453 script-validity reorg durability test completed"
