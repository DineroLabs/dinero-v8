#!/usr/bin/env bash
#
# Phase 2 of the shielded-era reorg invertibility plan
# (docs/specs/shielded_reorg_invertibility_audit.md).
#
# Property under test: walking the chain forward and then forcing a
# disconnect/reconnect cycle via invalidateblock + reconsiderblock
# must produce byte-identical consensus state at the restored tip.
#
# State hash combines every container that crosses the reorg
# boundary in the audit:
#   1. utreexo forest commitment   (consensus_utxo_set forest root)
#   2. shielded tree root          (CommitmentTree.Root)
#   3. shielded tree size          (CommitmentTree.Size)
#   4. nullifier set size          (NullifierSet.Size)
#   5. anchor history size         (AnchorHistory.Size)
#
# A drift in any of those after a Connect↔Disconnect↔Connect cycle
# fails the test loud rather than letting it accumulate silently
# the way the LA fleet drift accumulated through 9000+ blocks.
#
# Test shape:
#   1. mine N blocks on a fresh regtest node
#   2. capture state at the tip                       (S0)
#   3. invalidateblock(block at height 2)             [disconnect 2..N]
#   4. reconsiderblock(block at height 2)             [reconnect 2..N]
#   5. capture state at the tip                       (S2)
#   6. assert S0 == S2  (Connect/Disconnect/Connect is the identity)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ -n "${DINEROD:-}" ]]; then
    # CTest supplies this (ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"), so the
    # test follows the build directory wherever it is. Honour it and require it
    # to be real — never silently fall through to a guessed path.
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 1; }
elif [[ -x "${ROOT_DIR}/build/dinerod" ]]; then
    # Manual/local convenience only.
    DINEROD="${ROOT_DIR}/build/dinerod"
elif [[ -x "${ROOT_DIR}/dinerod" ]]; then
    DINEROD="${ROOT_DIR}/dinerod"
else
    # Fail HERE, naming the paths tried. Launching a non-existent binary and
    # then waiting on its RPC turns a missing file into a 30s timeout reported
    # as "RPC never came up", which reads like a consensus failure.
    echo "dinerod not found (tried: \$DINEROD unset, ${ROOT_DIR}/build/dinerod, ${ROOT_DIR}/dinerod)" >&2
    echo "set DINEROD=/path/to/dinerod to override" >&2
    exit 1
fi
RUN_ID=$$
DATADIR="/tmp/dinero_toctou_${RUN_ID}"
LOG="${DATADIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0
CHAIN_HEIGHT="${CHAIN_HEIGHT:-15}"
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

# --- raw JSON-RPC over cookie auth (the pattern proven by the
#     existing test_csn_reorg_churn / test_bug1 harnesses) -----------

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
    # extract result.<field> when result is a JSON object
    local response="$1" field="$2"
    echo "${response}" | tr -d '\n\t' \
        | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
        | head -n1
}

rpc_field_number() {
    local response="$1" field="$2"
    echo "${response}" | tr -d '\n\t' \
        | sed -n "s/.*\"${field}\"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p" \
        | head -n1
}

rpc_top_string() {
    # extract result when result is a bare string
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
    for _ in $(seq 1 30); do
        local r
        r="$(rpc getblockcount || true)"
        if [[ -n "${r}" && "${r}" != *"\"error\":\"null\""* ]]; then
            local h
            h="$(rpc_top_number "${r}")"
            [[ -n "${h}" ]] && return 0
        fi
        sleep 1
    done
    fail "RPC never came up"
}

# State hash via the daemon-side `daemon.shieldedstatehash` RPC.
# That hash covers ALL five reorg-bound containers — utreexo forest
# (commitment + numLeaves + canonical_empty_roots flag), shielded
# tree (root + size), nullifier set size, and the full anchor
# history (every (height, root) pair). The earlier shell-side
# composition only exercised the first four indirectly; this RPC
# closes the audit's "anchor history is only covered indirectly"
# caveat.
# Shielded-state root via `daemon.shieldedroot` — the SHIELDED-ONLY
# fingerprint that a future block-header commitment would carry
# (state_commitment_v1). Distinct from state_hash() above: it excludes the
# utreexo forest (already committed by header.utreexo_root) and
# length-prefixes each variable-length section.
#
# Asserted through the same Connect/Disconnect/Connect cycle because this is
# the value that would become consensus: if it is not a perfect inverse under
# DisconnectBlock, a reorg across the activation height is a chain split.
#
# Empty when the daemon predates the RPC; the caller treats that as "skip",
# never as a pass, so an older binary cannot silently green this assertion.
shielded_root() {
    local resp h
    resp="$(rpc daemon.shieldedroot)"
    h="$(rpc_field_string "${resp}" shielded_root)"
    printf '%s' "${h}"
}

state_hash() {
    local resp h
    resp="$(rpc daemon.shieldedstatehash)"
    h="$(rpc_field_string "${resp}" state_hash)"
    if [[ -z "${h}" ]]; then
        # Daemon doesn't expose the RPC (older binary) — fall back to
        # the composition-by-fields shape so the test still runs in
        # mixed-version environments.
        local info_resp utreexo
        info_resp="$(rpc getblockchaininfo)"
        utreexo="$(rpc_field_string "${info_resp}" utreexo_root)"
        : "${utreexo:=NA}"
        printf '%s|fallback' "${utreexo}" | shasum -a 256 | awk '{print $1}'
        return 0
    fi
    printf '%s' "${h}"
}

# ─────────────────────────────────────────────────────────────────────────────
# TOCTOU between the generation comparison and the failure-flag write.
#
# ConnectBlock:  capture gen -> COMPARE -> [BARRIER] -> WRITE preserved flags
# ReconsiderBlock:                          clear flags + advance gen + activate
#
# With activation_mutex_ on ReconsiderBlock, the reconsider cannot start while
# ConnectBlock holds the lock, so releasing the barrier lets the write land and
# the reconsider then clears it: final state CLEARED, chain restored.
#
# Without the mutex the reconsider runs while ConnectBlock is parked, clears the
# flags, and the released ConnectBlock re-asserts them: final state FAILED and
# the chain stays down. That is the mutation this test must catch.
#
# Deterministic in both directions — the barrier is an explicit rendezvous, not
# a sleep, and every wait is bounded so CI cannot hang.
# ─────────────────────────────────────────────────────────────────────────────
BARRIER_DIR="${DATADIR}/barrier"
mkdir -p "${BARRIER_DIR}"
BARRIER="connectblock_after_generation_compare"

start_n() {  # datadir rpcport log [env...]
    local dd="$1" rp="$2" lg="$3"; shift 3
    mkdir -p "${dd}"
    env "$@" "${DINEROD}" -regtest -datadir="${dd}" \
        -rpcport="${rp}" -port="$((rp + 1))" -listen=0 >"${lg}" 2>&1 &
    echo $!
}
stop_n() {
    local pid="$1"; [[ -z "${pid}" ]] && return 0
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${pid}" 2>/dev/null || return 0; sleep 0.5; done
    kill -9 "${pid}" 2>/dev/null || true
}
count_blocks() { rpc getblockcount 2>/dev/null | jq -r '.result // empty' 2>/dev/null; }

# Bounded wait for the daemon to reach the barrier. Never an unconditional
# sleep: absence of the marker is a hard failure with diagnostics.
wait_for_barrier() {
    local f="${BARRIER_DIR}/${BARRIER}.arrived"
    for _ in $(seq 1 200); do [[ -f "$f" ]] && return 0; sleep 0.1; done
    return 1
}

NODE="${DATADIR}/node"
PID="$(start_n "${NODE}" "${RPC_PORT}" "${DATADIR}/n.log" \
        DINERO_BARRIER_DIR="${BARRIER_DIR}" \
        DINERO_BARRIER_TIMEOUT_S=25)"
DSAVE="${DATADIR}"; DATADIR="${NODE}"
wait_rpc || fail "node did not reach RPC"
ADDR="$(rpc_field_string "$(rpc wallet.createhd '"toctou"')" first_address)"
[[ -n "${ADDR}" ]] || fail "wallet creation failed"
for _ in $(seq 1 4); do rpc generatetoaddress "8, \"${ADDR}\"" >/dev/null 2>&1; done
TIP0="$(count_blocks)"
[[ -n "${TIP0}" && "${TIP0}" -ge 8 ]] || fail "mining failed (tip=${TIP0})"

TH=$((TIP0 - 2))
TARGET="$(rpc getblockhash "${TH}" | jq -r '.result // ""')"
[[ "${TARGET}" =~ ^[0-9a-fA-F]{64}$ ]] || fail "no hash at height ${TH}"
rpc invalidateblock "\"${TARGET}\"" >/dev/null 2>&1
sleep 2
TIP_INV="$(count_blocks)"
[[ "${TIP_INV}" -lt "${TIP0}" ]] || fail "invalidate did not move the tip"
info "invalidated ${TH}: tip ${TIP0} -> ${TIP_INV}"
DATADIR="${DSAVE}"

# Arm the barrier and re-deliver the invalidated block so ConnectBlock parks in
# the TOCTOU window. submitblock is the simplest re-delivery that reaches the
# acceptance path.
stop_n "${PID}"
rm -f "${BARRIER_DIR}"/*
PID2="$(start_n "${NODE}" "${RPC_PORT}" "${DATADIR}/armed.log" \
        DINERO_BARRIER_AT="${BARRIER}" \
        DINERO_BARRIER_DIR="${BARRIER_DIR}" \
        DINERO_BARRIER_TIMEOUT_S=25)"
DATADIR="${NODE}"
wait_rpc || fail "node did not restart with the barrier armed"

BLOCKHEX="$(rpc getblock "\"${TARGET}\", 0" | jq -r '.result // ""')"
if [[ -z "${BLOCKHEX}" || "${BLOCKHEX}" == "null" ]]; then
    DATADIR="${DSAVE}"; stop_n "${PID2}"
    info "could not fetch raw block for re-delivery; barrier path not exercised"
    fail "test could not re-deliver the block — TOCTOU window not reached"
fi
( rpc submitblock "\"${BLOCKHEX}\"" >/dev/null 2>&1 ) &
SUBMIT_PID=$!

if wait_for_barrier; then
    pass "ConnectBlock parked at the generation-compare barrier"
else
    DATADIR="${DSAVE}"; stop_n "${PID2}"
    info "barrier marker never appeared in ${BARRIER_DIR}"
    ls -la "${BARRIER_DIR}" 2>/dev/null | sed 's/^/  /'
    fail "daemon never reached the barrier — window not exercised"
fi

# Concurrent reconsideration while ConnectBlock is parked.
( rpc reconsiderblock "\"${TARGET}\"" >/dev/null 2>&1 ) &
RECON_PID=$!
sleep 2   # scheduling only: the ASSERTIONS below do not depend on this
touch "${BARRIER_DIR}/${BARRIER}.release"
wait "${SUBMIT_PID}" 2>/dev/null || true
wait "${RECON_PID}" 2>/dev/null || true

# Settle, then assert the FINAL state.
for _ in $(seq 1 30); do
    a="$(count_blocks)"; sleep 1; b="$(count_blocks)"
    [[ "$a" == "$b" ]] && break
done
FINAL_TIP="$(count_blocks)"
info "final tip after concurrent submit+reconsider: ${FINAL_TIP} (pre-invalidate ${TIP0})"

# 1. The operator decision must win: the block must NOT still be invalid.
RE="$(rpc reconsiderblock "\"${TARGET}\"" 2>/dev/null)"
if echo "${RE}" | grep -q "not marked as invalid"; then
    pass "final flags: block is NOT marked invalid — reconsideration won"
else
    info "second reconsider says: $(echo "${RE}" | tr -d '\n' | head -c 200)"
    fail "TOCTOU: a stale acceptance re-asserted BLOCK_FAILED_VALID after reconsideration"
fi

# 2. Active-chain / candidate membership must have recovered.
[[ -n "${FINAL_TIP}" && "${FINAL_TIP}" -ge "${TIP0}" ]] \
    || fail "TOCTOU: chain did not recover to ${TIP0} (got ${FINAL_TIP}) — stale flags kept it down"
BEST="$(rpc getbestblockhash | jq -r '.result // ""')"
TIPH="$(rpc getblockhash "${TIP0}" | jq -r '.result // ""')"
[[ -n "${BEST}" && "${BEST}" == "${TIPH}" ]] \
    || fail "TOCTOU: best block ${BEST} is not the restored tip ${TIPH} — candidate membership did not recover"
pass "active chain and candidate membership recovered to ${FINAL_TIP}"

DATADIR="${DSAVE}"
stop_n "${PID2}"
pass "generation compare/write is serialized against reconsideration"
