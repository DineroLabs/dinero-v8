#!/usr/bin/env bash
#
# Copied-datadir reindex regression — companion to phase 2 of the
# shielded reorg invertibility plan
# (docs/specs/shielded_reorg_invertibility_audit.md).
#
# ── Scope (read this before changing the test) ──────────────────
#
# What this test pins:
#
#   The path where a fresh dinerod process is pointed at a *copy*
#   of a populated datadir and asked to --reindex-chainstate. That
#   path is structurally different from the in-place reindex
#   tested by test_reindex_chainstate_utreexo_equivalence.sh:
#     - no warm in-memory forest carried over from a running daemon
#     - no continuation of process state
#     - sensitive to anything in persisted state that might depend
#       on the original absolute datadir path, the current PID, or
#       the prior in-memory caches
#   Operators take this path when they stage a recovery on a
#   separate host or want to validate a snapshot before promoting
#   it. A regression in serialize/deserialize, DisconnectBlock,
#   the v3 canonical_empty_roots_ flag (a72053a9a), the anchor
#   rollback (e5aa07009), or the position-index ephemeral filter
#   (e5aa07009) trips this test loud instead of accumulating
#   silently.
#
# What this test does NOT pin:
#
#   The actual LA 2026-04-28 9291-style failure. That failure was
#   produced by drift baked into the on-disk state by an OLD
#   binary (pre-fix). On a chain the new binary builds from
#   scratch, that drift cannot accumulate, so reindexing such a
#   chain succeeds whether the v3 fixes are in place or not. The
#   pre-fix drift is reproducible only against:
#     (a) a canned drifted-datadir fixture derived from a real
#         pre-fix mainnet snapshot (size/privacy concerns; out of
#         scope for this commit), or
#     (b) a debug-only "inject v2 serialize" mode in the daemon
#         that emulates the pre-a72053a9a behavior at a single
#         block boundary.
#   Tracked as audit gap #10. This file gets us from "property
#   test green on chains we built" to "the copy-then-reindex
#   path is invertible on chains we built." The remaining step
#   to "the actual 9291-style failure is pinned" still needs
#   gap #10 closed.
#
# ── Test shape ──────────────────────────────────────────────────
#
#   1. mine a regtest chain past the canonical-roots fork height
#      so any subtree-deletion path is exercised
#   2. capture daemon.shieldedstatehash on the original datadir  (S0)
#   3. stop the original daemon cleanly
#   4. copy the datadir to a sibling location
#   5. scrub volatile artefacts in the copy (cookie, log, sock)
#   6. start a fresh dinerod against the copy with
#      --reindex-chainstate
#   7. capture daemon.shieldedstatehash on the reindexed copy    (S1)
#   8. assert S0 == S1 and that the reindex log shows the
#      success banner without "reindex-forest-root-mismatch"

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
RUN_ID=$$
DATA_DIR="/tmp/dinero_reindex_copy_orig_${RUN_ID}"
COPY_DIR="/tmp/dinero_reindex_copy_dest_${RUN_ID}"
LOG_ORIG="${DATA_DIR}/daemon.log"
LOG_COPY="${COPY_DIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0
CHAIN_HEIGHT="${CHAIN_HEIGHT:-15}"
RPC_PORT_ORIG="${RPC_PORT_ORIG:-$((25000 + RANDOM % 500))}"
P2P_PORT_ORIG="${P2P_PORT_ORIG:-$((RPC_PORT_ORIG + 1))}"
RPC_PORT_COPY="${RPC_PORT_COPY:-$((RPC_PORT_ORIG + 100))}"
P2P_PORT_COPY="${P2P_PORT_COPY:-$((RPC_PORT_COPY + 1))}"
RPC_TIMEOUT=20
ACTIVE_RPC_PORT=""
ACTIVE_DATADIR=""

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for f in "${LOG_ORIG}" "${LOG_COPY}"; do
        if [[ -f "${f}" ]]; then
            printf -- '--- %s tail ---\n' "${f}" >&2
            tail -n 80 "${f}" >&2 || true
        fi
    done
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
        rm -rf "${DATA_DIR}" "${COPY_DIR}" 2>/dev/null || true
    else
        info "preserving ${DATA_DIR} and ${COPY_DIR} for inspection"
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
    cookie="$(cat "${ACTIVE_DATADIR}/.cookie" 2>/dev/null || true)"
    if [[ -z "${cookie}" ]]; then
        return 1
    fi
    curl -s --connect-timeout 2 --max-time "${RPC_TIMEOUT}" \
        -u "${cookie}" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${ACTIVE_RPC_PORT}" 2>/dev/null
}

rpc_field_string() {
    local response="$1" field="$2"
    echo "${response}" | tr -d '\n\t' \
        | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
        | head -n1
}

rpc_top_string() {
    local response="$1"
    echo "${response}" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n1
}

rpc_top_number() {
    local response="$1"
    echo "${response}" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' \
        | head -n1
}

wait_rpc() {
    for _ in $(seq 1 60); do
        local r
        r="$(rpc getblockcount || true)"
        if [[ -n "${r}" && -n "$(rpc_top_number "${r}")" ]]; then
            return 0
        fi
        sleep 1
    done
    fail "RPC never came up at port ${ACTIVE_RPC_PORT}"
}

state_hash() {
    local resp h
    resp="$(rpc daemon.shieldedstatehash)"
    h="$(rpc_field_string "${resp}" state_hash)"
    [[ -n "${h}" ]] || fail "daemon.shieldedstatehash returned empty: ${resp}"
    printf '%s' "${h}"
}

start_daemon() {
    local datadir="$1" rpc_port="$2" p2p_port="$3" log="$4"
    shift 4
    # Phase 3a: ATOMIC_PERSIST env var routes both the original
    # daemon and the reindex run through ConsensusWriteBatch when
    # set to 1.
    local atomic_flag=""
    case "${ATOMIC_PERSIST:-0}" in
        1|true|yes|on) atomic_flag="-consensus.atomic_persist=1" ;;
    esac
    if [[ -n "${atomic_flag}" ]]; then
        "${DINEROD}" -regtest -datadir="${datadir}" \
            -rpcport="${rpc_port}" -port="${p2p_port}" -listen=0 \
            "${atomic_flag}" \
            "$@" \
            >"${log}" 2>&1 &
    else
        "${DINEROD}" -regtest -datadir="${datadir}" \
            -rpcport="${rpc_port}" -port="${p2p_port}" -listen=0 \
            "$@" \
            >"${log}" 2>&1 &
    fi
    PID=$!
    ACTIVE_DATADIR="${datadir}"
    ACTIVE_RPC_PORT="${rpc_port}"
}

stop_daemon() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
        # Prefer RPC stop; SIGTERM as fallback.
        rpc stop >/dev/null 2>&1 || kill -TERM "${PID}" 2>/dev/null || true
        for _ in $(seq 1 30); do
            kill -0 "${PID}" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "${PID}" 2>/dev/null || true
        wait "${PID}" 2>/dev/null || true
    fi
    PID=""
}

# ── 1. Spin up the original daemon, mine, optional shielded sends ────

mkdir -p "${DATA_DIR}"
info "starting original dinerod regtest at ${DATA_DIR} rpc=${RPC_PORT_ORIG}"
start_daemon "${DATA_DIR}" "${RPC_PORT_ORIG}" "${P2P_PORT_ORIG}" "${LOG_ORIG}"
wait_rpc

WALLET_RESP="$(rpc wallet.createhd "\"reindex_copy_test\"")"
MINING_ADDR="$(rpc_field_string "${WALLET_RESP}" first_address)"
[[ -n "${MINING_ADDR}" ]] || fail "wallet.createhd failed: ${WALLET_RESP}"

info "mining ${CHAIN_HEIGHT} blocks"
GEN_RESP="$(rpc generatetoaddress "${CHAIN_HEIGHT}, \"${MINING_ADDR}\"")"
echo "${GEN_RESP}" | tr -d '\n\t ' | grep -q '"error":null' \
    || fail "generatetoaddress failed: ${GEN_RESP}"

# Wait for the tip to settle.
for _ in $(seq 1 30); do
    TIP_HEIGHT="$(rpc_top_number "$(rpc getblockcount)")"
    [[ "${TIP_HEIGHT}" == "${CHAIN_HEIGHT}" ]] && break
    sleep 1
done
[[ "${TIP_HEIGHT}" == "${CHAIN_HEIGHT}" ]] \
    || fail "tip ${TIP_HEIGHT} != expected ${CHAIN_HEIGHT}"
info "original tip = ${TIP_HEIGHT}"

# ── 2. Capture the composite state hash on the original ──────────────

S0="$(state_hash)"
info "S0 (original) = ${S0}"

# ── 3. Stop the original daemon cleanly ──────────────────────────────

info "stopping original daemon cleanly"
stop_daemon

# ── 4. Copy the datadir to a sibling location ────────────────────────
# Mirrors what an operator does when staging a recovery on a separate
# host or validating a snapshot before promoting it.

info "copying datadir: ${DATA_DIR} -> ${COPY_DIR}"
cp -R "${DATA_DIR}" "${COPY_DIR}"

# Rotate the wallet socket file path so we don't trip on stale
# absolute paths embedded in the copy. If anything else in persisted
# state depends on the original datadir path, the reindex will surface
# it as a startup error.
find "${COPY_DIR}" -name "wallet.sock" -delete 2>/dev/null || true
# Fresh cookie + RPC log files.
rm -f "${COPY_DIR}/.cookie" "${COPY_DIR}/daemon.log"

# ── 5. Run --reindex-chainstate against the copy ─────────────────────

info "starting fresh dinerod against ${COPY_DIR} with --reindex-chainstate"
start_daemon "${COPY_DIR}" "${RPC_PORT_COPY}" "${P2P_PORT_COPY}" "${LOG_COPY}" --reindex-chainstate
wait_rpc

# Sanity: log should show the reindex banner. If reindex failed at
# any block (the LA-style symptom), we'd see
# "reindex-forest-root-mismatch" in the log instead of completion.
if grep -q "REINDEX OPERATION REQUESTED" "${LOG_COPY}"; then
    pass "reindex started against the copied datadir"
else
    fail "reindex banner missing from log; daemon may not have triggered the reindex path"
fi

if grep -qE "reindex-forest-root-mismatch|Reindex failed" "${LOG_COPY}"; then
    fail "reindex over copied datadir failed (see log tail above)"
fi

# Wait for the tip to be reached before measuring state.
for _ in $(seq 1 60); do
    TIP_AFTER="$(rpc_top_number "$(rpc getblockcount)")"
    [[ "${TIP_AFTER}" == "${CHAIN_HEIGHT}" ]] && break
    sleep 1
done
[[ "${TIP_AFTER}" == "${CHAIN_HEIGHT}" ]] \
    || fail "post-reindex tip ${TIP_AFTER} != expected ${CHAIN_HEIGHT}"

# ── 6. Capture state hash on the copy ────────────────────────────────

S1="$(state_hash)"
info "S1 (copied + reindexed) = ${S1}"

# ── 7. The invertibility property ────────────────────────────────────

if [[ "${S0}" != "${S1}" ]]; then
    fail "REGRESSION: --reindex-chainstate over a copied datadir changed the
composite reorg state.
  S0 (original)            = ${S0}
  S1 (copied + reindexed)  = ${S1}
This is the same shape that produced the LA fleet drift before
2026-04-28. See docs/specs/shielded_reorg_invertibility_audit.md."
fi

pass "--reindex-chainstate over copied datadir preserved the composite reorg state"
pass "S0 == S1 = ${S0}"
exit 0
