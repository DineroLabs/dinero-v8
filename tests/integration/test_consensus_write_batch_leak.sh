#!/usr/bin/env bash
#
# D4 contract test — phase 3a of the shielded reorg invertibility plan
# (docs/specs/atomic_consensus_persistence_phase3.md §6).
#
# A ConsensusWriteBatch destructed without an explicit Commit() or
# Abort() is, by definition, the "third state" that §1's law forbids.
# D4 says:
#   - debug / regtest / test build: hard abort
#   - release build:                enter consensus safe mode with
#                                    reason "consensus_write_batch_dropped",
#                                    refuse template generation + block
#                                    connect, require operator
#                                    safemode.exit { confirm: true }.
#
# This test cannot trip the contract from outside the daemon (the
# call site that constructs/destructs the batch is internal to
# ChainstateService::ConnectTip). Phase 3a's narrow scaffold doesn't
# yet expose a debug knob to leak a batch on purpose. So this script
# pins the lighter half of the contract:
#
#   1. boots a regtest node with the atomic-persist flag on
#   2. mines a chain (each block's ConnectTip constructs and
#      successfully Commits a batch — i.e., the destructor runs the
#      State::Committed path, which must NOT trip the leak panic)
#   3. asserts the daemon stayed up and the chain advanced
#   4. asserts the daemon log contains no "consensus_write_batch_dropped"
#      banner
#
# The harder half — actually leaking the batch and asserting the
# expected response per build mode — needs a debug-only
# DINERO_LEAK_BATCH knob inside ConsensusWriteBatch, gated by the
# same #ifdef as MaybeAbortAt. That lands when the §3.1 step 4
# journal row lands in phase 3b. Until then, this test is the
# negative-control half of the D4 contract.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
RUN_ID=$$
DATADIR="/tmp/dinero_cwb_leak_${RUN_ID}"
LOG="${DATADIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0
CHAIN_HEIGHT="${CHAIN_HEIGHT:-12}"
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT="${RPC_PORT:-$(alloc_port_base)}"
P2P_PORT="${P2P_PORT:-$((RPC_PORT + 1))}"
RPC_TIMEOUT=20

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -f "${LOG}" ]]; then
        printf -- '--- daemon log tail ---\n' >&2
        tail -n 60 "${LOG}" >&2 || true
    fi
    cleanup
    exit 1
}

cleanup() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
        kill -TERM "${PID}" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "${PID}" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "${PID}" 2>/dev/null || true
    fi
    if [[ "${KEEP_ON_FAIL}" -eq 0 ]]; then
        rm -rf "${DATADIR}" 2>/dev/null || true
    else
        info "preserving ${DATADIR} for inspection"
    fi
}
trap cleanup EXIT

rpc() {
    local method="$1"
    shift
    local params="$*"
    local json_params="[]"
    [[ -n "${params}" ]] && json_params="[${params}]"
    local cookie
    cookie="$(cat "${DATADIR}/.cookie" 2>/dev/null || true)"
    if [[ -z "${cookie}" ]]; then
        return 1
    fi
    curl -s --connect-timeout 2 --max-time "${RPC_TIMEOUT}" \
        -u "${cookie}" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${RPC_PORT}" 2>/dev/null
}

rpc_field_string() {
    local response="$1" field="$2"
    echo "${response}" | tr -d '\n\t' \
        | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
        | head -n1
}

rpc_top_number() {
    local response="$1"
    echo "${response}" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' \
        | head -n1
}

wait_rpc() {
    for _ in $(seq 1 30); do
        local r
        r="$(rpc getblockcount || true)"
        if [[ -n "${r}" && -n "$(rpc_top_number "${r}")" ]]; then
            return 0
        fi
        sleep 1
    done
    fail "RPC never came up"
}

mkdir -p "${DATADIR}"
info "starting dinerod regtest with -consensus.atomic_persist=1"
"${DINEROD}" -regtest -datadir="${DATADIR}" \
    -rpcport="${RPC_PORT}" -port="${P2P_PORT}" -listen=0 \
    -consensus.atomic_persist=1 \
    >"${LOG}" 2>&1 &
PID=$!

wait_rpc

WALLET_RESP="$(rpc wallet.createhd "\"cwb_leak_test\"")"
MINING_ADDR="$(rpc_field_string "${WALLET_RESP}" first_address)"
[[ -n "${MINING_ADDR}" ]] || fail "wallet.createhd failed: ${WALLET_RESP}"

info "mining ${CHAIN_HEIGHT} blocks (each must construct and Commit a batch cleanly)"
GEN_RESP="$(rpc generatetoaddress "${CHAIN_HEIGHT}, \"${MINING_ADDR}\"")"
echo "${GEN_RESP}" | tr -d '\n\t ' | grep -q '"error":null' \
    || fail "generatetoaddress failed: ${GEN_RESP}"

for _ in $(seq 1 30); do
    TIP="$(rpc_top_number "$(rpc getblockcount)")"
    [[ "${TIP}" == "${CHAIN_HEIGHT}" ]] && break
    sleep 1
done
[[ "${TIP}" == "${CHAIN_HEIGHT}" ]] \
    || fail "tip ${TIP} != expected ${CHAIN_HEIGHT}"

# Daemon survived → no destructor leak fired.
if grep -q "consensus_write_batch_dropped" "${LOG}"; then
    fail "daemon log contains 'consensus_write_batch_dropped' — D4 destructor leak fired during Commit-side path"
fi

if grep -q "ConsensusWriteBatch.*FATAL" "${LOG}"; then
    fail "daemon log contains ConsensusWriteBatch FATAL — destructor leak path triggered"
fi

# Daemon must still be responsive (trivial liveness check).
HC="$(rpc_top_number "$(rpc getblockcount)")"
[[ "${HC}" == "${CHAIN_HEIGHT}" ]] \
    || fail "daemon unresponsive after mining; got ${HC}"

pass "ConsensusWriteBatch Commit path runs cleanly across ${CHAIN_HEIGHT} blocks"
pass "no consensus_write_batch_dropped banner in daemon log"
exit 0
