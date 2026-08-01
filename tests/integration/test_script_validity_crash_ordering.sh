#!/usr/bin/env bash
# Regression test for #453 — durability ORDERING at the pre-commit boundary.
#
# The #453 fix persists BLOCK_VALID_SCRIPTS by passing it as extra_status_bits
# to updateUndoLocator(), which STAGES it into ConnectTip's unified WriteBatch
# (chainstate_service.cpp, ConnectTip). The guarantee that matters is:
#
#   validation succeeds -> full status is staged and COMMITTED -> tip published
#
# If the process dies after staging but BEFORE the batch commits, nothing may
# be observable on restart: no advanced tip, no BLOCK_VALID_SCRIPTS, no partial
# undo metadata.
#
# HOOK POSITION IS LOAD-BEARING. This uses "after_undo_before_tip", which fires
# immediately BEFORE chain_db_->writeBatch(). The in-code comment states it
# directly: "the only point where 'undo durable, tip not yet' still holds is
# right before writeBatch is invoked."
#
# The nearby hook "after_unified_batch_before_frontier_write" fires AFTER the
# commit and is deliberately NOT used here — a post-commit crash cannot prove
# anything about a failure while staging updateUndoLocator().
#
# The hook is compiled in but gated to regtest, so it cannot fire on mainnet.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_script_crash_order_$$"
LOG_SEED="${DATA_DIR}.seed.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_AFTER="${DATA_DIR}.after.log"
LOG_FINAL="${DATA_DIR}.final.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
CURRENT_DATADIR=""
CURRENT_RPC_PORT=""

BLOCK_VALID_SCRIPTS=16
BLOCK_HAVE_UNDO=256
SEED_BLOCKS=10

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    for L in "${LOG_CRASH}" "${LOG_AFTER}"; do
        [[ -f "$L" ]] && { printf -- '--- %s tail ---\n' "$L" >&2; tail -40 "$L" >&2 || true; }
    done
    exit 1
}

cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_SEED}" "${LOG_CRASH}" "${LOG_AFTER}" "${LOG_FINAL}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    local d="$1"
    [[ -f "${d}/.cookie" ]] && { printf '%s\n' "${d}/.cookie"; return 0; }
    [[ -f "${d}/regtest/.cookie" ]] && { printf '%s\n' "${d}/regtest/.cookie"; return 0; }
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

# NOTE: deliberately stricter than the shared helper in the other integration
# scripts. generatetoaddress can report failure in a NESTED field
# (top-level "error":null, real error inside "result"), which the shared
# rpc_has_error() misses entirely — see #458. A silent mine failure here would
# make this test pass for the wrong reason.
rpc_failed() {
    local resp="$1"
    local compact
    compact="$(echo "${resp}" | tr -d '\n\t ')"
    [[ -z "${compact}" ]] && return 0
    [[ "${compact}" == *"\"error\":{"* ]] && return 0
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

# $1 = log file; remaining args = extra env assignments
start_node() {
    local log_file="$1"; shift
    mkdir -p "${DATA_DIR}"
    CURRENT_DATADIR="${DATA_DIR}"
    CURRENT_RPC_PORT="${RPC_PORT}"
    env "$@" "${DINEROD}" \
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
    local r
    r="$(rpc_call "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${r}" ]] && rpc_failed "${r}"; then
        fail "stop RPC errored: ${r}"
    fi
    wait_dead "${PID}" || fail "daemon did not exit cleanly after stop RPC"
    wait "${PID}" 2>/dev/null || true
    PID=""
}

status_flags_for() {
    local hash="$1" r
    r="$(rpc_call "getblockheader" "[\"${hash}\"]")"
    rpc_failed "${r}" && fail "getblockheader failed for ${hash}: ${r}"
    jq -r '.result.status_flags' <<<"${r}"
}

# Returns a 64-hex block hash on stdout, or fails (non-zero, no output) when the
# height does not exist. Validating the SHAPE rather than trusting the error
# field matters here: this daemon can answer with a nested error object inside
# "result" while the top-level "error" is null (#458), and jq -r would then emit
# a literal "{" that downstream calls would happily treat as a block hash.
hash_at_height() {
    local h="$1" r v
    r="$(rpc_call "getblockhash" "[${h}]")"
    rpc_failed "${r}" && return 1
    v="$(jq -r 'if (.result | type) == "string" then .result else empty end' <<<"${r}")"
    [[ "${v}" =~ ^[0-9a-fA-F]{64}$ ]] || return 1
    printf '%s\n' "${v}"
}

block_count() {
    jq -r '.result' <<<"$(rpc_call "getblockcount" '[]')"
}

require_tools

RPC_PORT=$((43000 + RANDOM % 500))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

# ------------------------------------------------------------ seed a chain
info "Starting daemon and seeding ${SEED_BLOCKS} blocks"
start_node "${LOG_SEED}"
wait_rpc || fail "daemon did not reach RPC readiness"

ADDR="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_failed "${ADDR}" && fail "wallet.getnewaddress failed: ${ADDR}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR}")"
[[ -n "${MINER_ADDR}" ]] || fail "empty mining address"

MINE="$(rpc_call "generatetoaddress" "[${SEED_BLOCKS},\"${MINER_ADDR}\"]")"
rpc_failed "${MINE}" && fail "seed generatetoaddress failed: ${MINE}"

SEED_HEIGHT="$(block_count)"
[[ "${SEED_HEIGHT}" == "${SEED_BLOCKS}" ]] || fail "expected height ${SEED_BLOCKS}, got ${SEED_HEIGHT}"

TIP_HASH="$(hash_at_height "${SEED_HEIGHT}")" || fail "getblockhash failed at seed tip"
TIP_FLAGS_BEFORE="$(status_flags_for "${TIP_HASH}")"
info "seed tip height=${SEED_HEIGHT} status_flags=${TIP_FLAGS_BEFORE}"

(( (TIP_FLAGS_BEFORE & BLOCK_VALID_SCRIPTS) != 0 )) \
    || fail "precondition: seed tip lacks BLOCK_VALID_SCRIPTS (${TIP_FLAGS_BEFORE})"

stop_node_clean

# ------------------------------------------- crash while the batch is staged
info "Restarting with crash hook after_undo_before_tip (fires BEFORE writeBatch)"
start_node "${LOG_CRASH}" DINERO_CRASH_AT=after_undo_before_tip DINERO_CRASH_AFTER_N=1
wait_rpc || fail "daemon did not reach RPC readiness with crash hook armed"

CRASH_PID="${PID}"
info "Mining 1 block — expected to abort mid-connect, before the batch commits"
rpc_call "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null 2>&1 || true

wait_dead "${CRASH_PID}" || fail "daemon did not abort at the crash hook"
PID=""

grep -q "aborting at hook 'after_undo_before_tip'" "${LOG_CRASH}" \
    || fail "crash hook did not fire at after_undo_before_tip — the test did not exercise the pre-commit boundary"
pass "crash fired at the pre-commit boundary (before writeBatch)"

# --------------------------------- restart: no tip advance, no partial state
info "Restarting after the crash"
start_node "${LOG_AFTER}"
wait_rpc || fail "daemon did not reach RPC readiness after crash restart"

HEIGHT_AFTER="$(block_count)"
info "height after crash restart: ${HEIGHT_AFTER} (seed was ${SEED_HEIGHT})"

if [[ "${HEIGHT_AFTER}" != "${SEED_HEIGHT}" ]]; then
    fail "active tip advanced across a pre-commit crash: ${SEED_HEIGHT} -> ${HEIGHT_AFTER}. The tip must not be published for a block whose validated status was never committed."
fi
pass "active tip was not advanced by the uncommitted block"

# The block being connected must leave nothing behind.
if ORPHAN_HASH="$(hash_at_height $((SEED_HEIGHT + 1)) 2>/dev/null)" && [[ -n "${ORPHAN_HASH}" && "${ORPHAN_HASH}" != "null" ]]; then
    ORPHAN_FLAGS="$(status_flags_for "${ORPHAN_HASH}")"
    if (( (ORPHAN_FLAGS & BLOCK_VALID_SCRIPTS) != 0 )); then
        fail "a block whose commit never landed has durable BLOCK_VALID_SCRIPTS (${ORPHAN_FLAGS}) at height $((SEED_HEIGHT + 1))"
    fi
    info "height $((SEED_HEIGHT+1)) present with status_flags=${ORPHAN_FLAGS} (no SCRIPTS — acceptable)"
else
    pass "no block published at height $((SEED_HEIGHT + 1))"
fi

# The previously-good tip must be untouched — in particular BLOCK_HAVE_UNDO.
TIP_FLAGS_AFTER="$(status_flags_for "${TIP_HASH}")"
info "seed tip status_flags after crash restart: ${TIP_FLAGS_AFTER}"
if [[ "${TIP_FLAGS_BEFORE}" != "${TIP_FLAGS_AFTER}" ]]; then
    fail "pre-existing tip status changed across the crash: ${TIP_FLAGS_BEFORE} -> ${TIP_FLAGS_AFTER}"
fi
if (( (TIP_FLAGS_AFTER & BLOCK_HAVE_UNDO) == 0 )); then
    fail "crash restart lost BLOCK_HAVE_UNDO on the seed tip (${TIP_FLAGS_AFTER})"
fi
pass "no partial state: prior tip status intact, BLOCK_HAVE_UNDO preserved"

# --------------------------------- retry without the failure -> durable 415
info "Mining again with no crash hook armed"
RETRY="$(rpc_call "generatetoaddress" "[1,\"${MINER_ADDR}\"]")"
rpc_failed "${RETRY}" && fail "retry generatetoaddress failed: ${RETRY}"

RETRY_HEIGHT="$(block_count)"
[[ "${RETRY_HEIGHT}" == "$((SEED_HEIGHT + 1))" ]] \
    || fail "retry did not advance the tip: ${SEED_HEIGHT} -> ${RETRY_HEIGHT}"

RETRY_HASH="$(hash_at_height "${RETRY_HEIGHT}")" || fail "getblockhash failed after retry"
RETRY_FLAGS="$(status_flags_for "${RETRY_HASH}")"
info "retry block height=${RETRY_HEIGHT} status_flags=${RETRY_FLAGS}"
(( (RETRY_FLAGS & BLOCK_VALID_SCRIPTS) != 0 )) \
    || fail "retry block lacks BLOCK_VALID_SCRIPTS (${RETRY_FLAGS})"

stop_node_clean

# Durability of the retry across a clean restart.
info "Restarting to confirm the retry block's status is durable"
start_node "${LOG_FINAL}"
wait_rpc || fail "daemon did not reach RPC readiness for the final check"

FINAL_FLAGS="$(status_flags_for "${RETRY_HASH}")"
info "retry block status_flags after restart: ${FINAL_FLAGS}"
if (( (FINAL_FLAGS & BLOCK_VALID_SCRIPTS) == 0 )); then
    fail "retry block lost BLOCK_VALID_SCRIPTS across restart: ${RETRY_FLAGS} -> ${FINAL_FLAGS}"
fi
if [[ "${RETRY_FLAGS}" != "${FINAL_FLAGS}" ]]; then
    fail "retry block status changed across restart: ${RETRY_FLAGS} -> ${FINAL_FLAGS}"
fi
pass "after clearing the failure, the validated status is durable (${FINAL_FLAGS})"

stop_node_clean
pass "#453 durability-ordering crash test completed"
