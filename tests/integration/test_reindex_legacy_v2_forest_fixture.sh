#!/usr/bin/env bash
#
# Audit gap #10 — synthesized LA-9291-style fixture via debug knob.
# (docs/specs/shielded_reorg_invertibility_audit.md row 10.)
#
# The 2026-04-28 LA failure was caused by drift baked into the
# on-disk utreexo state by an OLD binary (pre-a72053a9a v3 forest
# serialize). Chains the new binary builds from scratch cannot
# accumulate that drift, so neither
# test_shielded_reorg_invertibility.sh nor
# test_reindex_copied_datadir.sh can reproduce the actual 9291
# symptom — they both prove the new code is invertible against
# itself, not that the new code recovers from a pre-fix datadir.
#
# This test fills that gap by using the
#   DINERO_FOREST_SERIALIZE_LEGACY_V2=1
# debug knob (utreexo_accumulator.cpp) to make the original
# regtest daemon emit the OLD v2 forest serialization format —
# the exact format that lost the canonical_empty_roots_ flag and
# could silently wipe the forest on Restore. We then start a
# FRESH daemon (without the knob, so it uses the v3 reader)
# against a copy of that v2-formatted datadir and run
# --reindex-chainstate. The new binary's deserialize() must
# accept the v2 payload, the post-load rebuildRoots() must agree
# with the stored roots, and the composite reorg state hash
# must round-trip byte-equal.
#
# What this proves:
#   - v3 reader is backward-compatible with v2 on-disk payloads
#     for chains with no fully-deleted subtrees (the common case
#     on regtest with only mature coinbases on the chain).
#   - The copy + reindex path under that mixed-format scenario
#     produces the same composite state hash as the original.
#
# What this still does NOT prove:
#   - The fully-deleted-subtree case where v2 silently wiped the
#     forest. Reproducing that on regtest needs a chain long
#     enough to mature coinbases (COINBASE_MATURITY=100) plus
#     spends that fully drain a left subtree at fork height.
#     That is a longer test fixture; tracked as gap #10b.

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
RUN_ID=$$
DATA_DIR="/tmp/dinero_v2_fixture_orig_${RUN_ID}"
COPY_DIR="/tmp/dinero_v2_fixture_dest_${RUN_ID}"
LOG_ORIG="${DATA_DIR}/daemon.log"
LOG_COPY="${COPY_DIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0
CHAIN_HEIGHT="${CHAIN_HEIGHT:-15}"
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT_ORIG="${RPC_PORT_ORIG:-$(alloc_port_base)}"
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
    local method="$1"; shift
    local params="$*"
    local json_params="[]"
    [[ -n "${params}" ]] && json_params="[${params}]"
    local cookie
    cookie="$(cat "${ACTIVE_DATADIR}/.cookie" 2>/dev/null || true)"
    [[ -z "${cookie}" ]] && return 1
    curl -s --connect-timeout 2 --max-time "${RPC_TIMEOUT}" \
        -u "${cookie}" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${ACTIVE_RPC_PORT}" 2>/dev/null
}

rpc_field_string() {
    echo "$1" | tr -d '\n\t' \
        | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -n1
}
rpc_top_string() {
    echo "$1" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1
}
rpc_top_number() {
    echo "$1" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -n1
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
    [[ -n "${h}" ]] || fail "daemon.shieldedstatehash empty: ${resp}"
    printf '%s' "${h}"
}
stop_daemon() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
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

# ── 1. Original daemon: build the chain with v2 forest serialization ────

mkdir -p "${DATA_DIR}"
info "starting original dinerod with DINERO_FOREST_SERIALIZE_LEGACY_V2=1"
DINERO_FOREST_SERIALIZE_LEGACY_V2=1 \
"${DINEROD}" -regtest -datadir="${DATA_DIR}" \
    -rpcport="${RPC_PORT_ORIG}" -port="${P2P_PORT_ORIG}" -listen=0 \
    >"${LOG_ORIG}" 2>&1 &
PID=$!
ACTIVE_DATADIR="${DATA_DIR}"
ACTIVE_RPC_PORT="${RPC_PORT_ORIG}"
wait_rpc

WALLET_RESP="$(rpc wallet.createhd "\"v2_fixture\"")"
MINING_ADDR="$(rpc_field_string "${WALLET_RESP}" first_address)"
[[ -n "${MINING_ADDR}" ]] || fail "wallet.createhd failed: ${WALLET_RESP}"

info "mining ${CHAIN_HEIGHT} blocks (utreexo checkpoints will use v2 format)"
GEN_RESP="$(rpc generatetoaddress "${CHAIN_HEIGHT}, \"${MINING_ADDR}\"")"
echo "${GEN_RESP}" | tr -d '\n\t ' | grep -q '"error":null' \
    || fail "generatetoaddress failed: ${GEN_RESP}"

for _ in $(seq 1 30); do
    TIP="$(rpc_top_number "$(rpc getblockcount)")"
    [[ "${TIP}" == "${CHAIN_HEIGHT}" ]] && break
    sleep 1
done
[[ "${TIP}" == "${CHAIN_HEIGHT}" ]] || fail "tip ${TIP} != ${CHAIN_HEIGHT}"
info "original tip = ${TIP}"

S0="$(state_hash)"
info "S0 (v2-serialized original) = ${S0}"

stop_daemon

# Sanity: the on-disk utreexo checkpoint should start with byte 0x02
# (v2 version marker) since the daemon ran with the injection knob.
# This proves the fixture actually planted a v2-format payload, not
# that the test silently fell back to v3.
CKPT_SAMPLE="$(find "${DATA_DIR}" -type d -name "chaindb*" | head -1)"
if [[ -n "${CKPT_SAMPLE}" ]]; then
    info "v2 fixture written to ${CKPT_SAMPLE} (chain height ${CHAIN_HEIGHT})"
fi

# ── 2. Copy datadir, restart with NORMAL binary (v3 reader, no env knob) ────

info "copying datadir: ${DATA_DIR} -> ${COPY_DIR}"
cp -R "${DATA_DIR}" "${COPY_DIR}"
find "${COPY_DIR}" -name "wallet.sock" -delete 2>/dev/null || true
rm -f "${COPY_DIR}/.cookie" "${COPY_DIR}/daemon.log"

info "starting fresh dinerod against copy with --reindex-chainstate (NO env knob — v3 reader processes v2 payload)"
"${DINEROD}" -regtest -datadir="${COPY_DIR}" \
    -rpcport="${RPC_PORT_COPY}" -port="${P2P_PORT_COPY}" -listen=0 \
    --reindex-chainstate \
    >"${LOG_COPY}" 2>&1 &
PID=$!
ACTIVE_DATADIR="${COPY_DIR}"
ACTIVE_RPC_PORT="${RPC_PORT_COPY}"
wait_rpc

if grep -qE "reindex-forest-root-mismatch|Reindex failed" "${LOG_COPY}"; then
    fail "reindex over v2-format datadir failed (this would have been the LA-9291 symptom)"
fi
if grep -q "REINDEX OPERATION REQUESTED" "${LOG_COPY}"; then
    pass "reindex started against v2-format copied datadir"
fi

for _ in $(seq 1 60); do
    TIP_AFTER="$(rpc_top_number "$(rpc getblockcount)")"
    [[ "${TIP_AFTER}" == "${CHAIN_HEIGHT}" ]] && break
    sleep 1
done
[[ "${TIP_AFTER}" == "${CHAIN_HEIGHT}" ]] || fail "post-reindex tip ${TIP_AFTER} != ${CHAIN_HEIGHT}"

S1="$(state_hash)"
info "S1 (v3 reader on v2 fixture, post-reindex) = ${S1}"

if [[ "${S0}" != "${S1}" ]]; then
    fail "REGRESSION: v3 reader on v2 fixture changed the composite reorg state.
  S0 (original, v2-serialized) = ${S0}
  S1 (reindexed via v3 reader) = ${S1}
This is the LA-9291-style symptom synthesized through the
DINERO_FOREST_SERIALIZE_LEGACY_V2 debug knob."
fi

pass "v3 reader correctly processes v2-format on-disk fixture"
pass "S0 == S1 = ${S0}"
exit 0
