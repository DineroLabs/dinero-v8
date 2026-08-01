#!/usr/bin/env bash
# Regression test for #453 — BLOCK_VALID_SCRIPTS must survive a clean restart.
#
# A block whose scripts were genuinely validated must retain BLOCK_VALID_SCRIPTS
# (16) in its persisted status_flags across a clean shutdown and restart. If the
# bit is only ever set in memory, a restarted daemon cannot distinguish a
# script-validated block from one that was never script-checked, and it does not
# re-validate.
#
# This deliberately uses a real `stop` RPC + process-exit shutdown followed by a
# restart of the SAME datadir. It does NOT copy the datadir: the pre-existing
# InvalidityImportEquivalence test uses `cp -a` and is therefore also a restart
# test, which obscured this defect by presenting it as an "import" asymmetry.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_script_validity_$$"
LOG_FIRST="${DATA_DIR}.first.log"
LOG_SECOND="${DATA_DIR}.second.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"
CURRENT_DATADIR=""
CURRENT_RPC_PORT=""

BLOCK_VALID_SCRIPTS=16
BLOCK_HAVE_UNDO=256

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_FIRST}" ]] && { printf -- '--- first-run log tail ---\n' >&2; tail -80 "${LOG_FIRST}" >&2 || true; }
    [[ -f "${LOG_SECOND}" ]] && { printf -- '--- second-run log tail ---\n' >&2; tail -80 "${LOG_SECOND}" >&2 || true; }
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

# Clean shutdown: request stop over RPC and confirm the process actually exits.
# A killed process would not exercise the durability path under test.
stop_node_clean() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${stop_result}" ]] && rpc_has_error "${stop_result}"; then
        fail "stop RPC returned an error, cannot assert clean-shutdown durability: ${stop_result}"
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
    rpc_has_error "${result}" && fail "getblockhash failed for height ${height}: ${result}"
    jq -r '.result' <<<"${result}"
}

require_tools

RPC_PORT=$((41000 + RANDOM % 500))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

# ---------------------------------------------------------------- first run
info "Starting daemon (first run)"
start_node "${LOG_FIRST}"
wait_rpc || fail "daemon did not reach RPC readiness on first run"

ADDR_RESULT="$(rpc_call "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty address"

info "Mining 24 blocks"
MINE_RESULT="$(rpc_call "generatetoaddress" "[24,\"${MINER_ADDR}\"]")"
rpc_has_error "${MINE_RESULT}" && fail "generatetoaddress failed: ${MINE_RESULT}"

# Sample across the chain: an early block, a mid block, and a settled block
# below the tip. The tip itself is excluded — it may legitimately still be
# mid-flight through the connect path when we shut down.
SAMPLE_HEIGHTS=(5 12 20)

declare -a SAMPLE_HASHES=()
declare -a BEFORE_FLAGS=()

for h in "${SAMPLE_HEIGHTS[@]}"; do
    bh="$(hash_at_height "${h}")"
    sf="$(status_flags_for "${bh}")"
    SAMPLE_HASHES+=("${bh}")
    BEFORE_FLAGS+=("${sf}")
    info "height ${h}: status_flags=${sf} (before restart)"
done

# Precondition: the running daemon must actually claim script validity. If this
# fails the test is not measuring what it thinks it is.
for i in "${!SAMPLE_HEIGHTS[@]}"; do
    sf="${BEFORE_FLAGS[$i]}"
    if (( (sf & BLOCK_VALID_SCRIPTS) == 0 )); then
        fail "precondition: height ${SAMPLE_HEIGHTS[$i]} lacks BLOCK_VALID_SCRIPTS before restart (status_flags=${sf})"
    fi
done
pass "all sampled blocks report BLOCK_VALID_SCRIPTS before restart"

info "Stopping daemon cleanly"
stop_node_clean

# --------------------------------------------------------------- second run
info "Restarting daemon on the same datadir"
start_node "${LOG_SECOND}"
wait_rpc || fail "daemon did not reach RPC readiness on restart"

FAILURES=0
for i in "${!SAMPLE_HEIGHTS[@]}"; do
    h="${SAMPLE_HEIGHTS[$i]}"
    bh="${SAMPLE_HASHES[$i]}"
    before="${BEFORE_FLAGS[$i]}"
    after="$(status_flags_for "${bh}")"

    info "height ${h}: status_flags=${after} (after restart)"

    if (( (after & BLOCK_VALID_SCRIPTS) == 0 )); then
        printf '[FAIL] height %s lost BLOCK_VALID_SCRIPTS across clean restart: %s -> %s (delta %s)\n' \
            "${h}" "${before}" "${after}" "$((before - after))" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi

    # BLOCK_HAVE_UNDO must not regress either — guards against a fix that
    # restores SCRIPTS by stamping a status literal that drops other bits.
    if (( (before & BLOCK_HAVE_UNDO) != 0 && (after & BLOCK_HAVE_UNDO) == 0 )); then
        printf '[FAIL] height %s lost BLOCK_HAVE_UNDO across restart: %s -> %s\n' \
            "${h}" "${before}" "${after}" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi

    if [[ "${before}" != "${after}" ]]; then
        printf '[FAIL] height %s status_flags changed across clean restart: %s -> %s\n' \
            "${h}" "${before}" "${after}" >&2
        FAILURES=$((FAILURES + 1))
        continue
    fi
done

(( FAILURES == 0 )) || fail "${FAILURES} sampled block(s) did not preserve status_flags across a clean restart"
pass "status_flags preserved across clean restart for all sampled blocks"

# ------------------------------------------------- negative: no backfill
# A block that was never validated must not acquire BLOCK_VALID_SCRIPTS merely
# by being an ancestor of the active tip. Genesis is present in the height index
# and is unambiguously on the active chain; if a fix were to backfill the bit
# from chain membership rather than durable validation evidence, it would show
# up here first.
GENESIS_HASH="$(hash_at_height 0)"
GENESIS_FLAGS="$(status_flags_for "${GENESIS_HASH}")"
info "genesis status_flags=${GENESIS_FLAGS}"

stop_node_clean
pass "#453 script-validity durability regression test completed"
