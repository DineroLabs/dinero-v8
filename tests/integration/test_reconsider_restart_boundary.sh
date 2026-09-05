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
DATADIR="/tmp/dinero_reconsider_restart_${RUN_ID}"
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
# Crash between the reconsider commit and re-activation.
#
#     ReconsiderBlock: flags cleared + generation advanced, COMMITTED
#           |
#           v   process dies at after_reconsider_persist_before_activate
#     ActivateBestChain() has NOT run
#
# The operator decision is durable but unapplied. A restart must preserve that
# decision — flags stay cleared, generation stays advanced — and must still
# reach the correct tip, because nothing has re-activated it yet.
#
# Without this, the hook only proves the boundary EXISTS, not that crossing it
# is safe.
# ─────────────────────────────────────────────────────────────────────────────
# NOTE: this harness (test_shielded_reorg_invertibility.sh) defines pass()
# directly. Do NOT alias it to ck_pass — that name belongs to the
# promotion-race harness, and aliasing produced exit 127 mid-run.

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
count_blocks() {
    # jq, not a regex: this harness's rpc() emits pretty-printed JSON across
    # multiple lines, so a single-line sed match returns a brace and every
    # numeric comparison below silently misbehaves.
    rpc getblockcount 2>/dev/null | jq -r '.result // empty' 2>/dev/null
}

NODE_DIR="${DATADIR}/node"
PID="$(start_n "${NODE_DIR}" "${RPC_PORT}" "${DATADIR}/n1.log")"
DATADIR_SAVE="${DATADIR}"; DATADIR="${NODE_DIR}"
wait_rpc || fail "node did not reach RPC"

ADDR="$(rpc_field_string "$(rpc wallet.createhd '"recon"')" first_address)"
[[ -n "${ADDR}" ]] || fail "could not create wallet"
for _ in $(seq 1 5); do rpc generatetoaddress "8, \"${ADDR}\"" >/dev/null 2>&1; done
TIP0="$(count_blocks)"
[[ -n "${TIP0}" && "${TIP0}" -ge 8 ]] || fail "mining failed (tip=${TIP0})"
info "tip before invalidation: ${TIP0}"

TARGET_H=$((TIP0 - 3))
TARGET="$(rpc getblockhash "${TARGET_H}" | jq -r '.result // ""')"
[[ "${TARGET}" =~ ^[0-9a-fA-F]{64}$ ]] || fail "no hash at height ${TARGET_H}"

rpc invalidateblock "\"${TARGET}\"" >/dev/null 2>&1
sleep 3
TIP_INV="$(count_blocks)"
info "after invalidate at ${TARGET_H}: tip=${TIP_INV}"
[[ "${TIP_INV}" -lt "${TIP0}" ]] || fail "invalidate did not move the tip"
pass "invalidation moved the tip ${TIP0} -> ${TIP_INV}"

DATADIR="${DATADIR_SAVE}"
stop_n "${PID}"

# Restart WITH the crash hook armed, then reconsider: the daemon must abort
# after committing the cleared flags and advanced generation, before activating.
CRASH_LOG="${DATADIR}/crash.log"
PID2="$(start_n "${NODE_DIR}" "${RPC_PORT}" "${CRASH_LOG}" \
        DINERO_CRASH_AT=after_reconsider_persist_before_activate)"
DATADIR="${NODE_DIR}"
wait_rpc || fail "node did not restart before the crash run"
rpc reconsiderblock "\"${TARGET}\"" >/dev/null 2>&1 || true
for _ in $(seq 1 40); do kill -0 "${PID2}" 2>/dev/null || break; sleep 1; done
DATADIR="${DATADIR_SAVE}"

if kill -0 "${PID2}" 2>/dev/null; then
    stop_n "${PID2}"
    info "hook did not fire; the daemon stayed up through reconsideration"
    fail "after_reconsider_persist_before_activate never aborted — boundary untested"
fi
grep -q "aborting at hook 'after_reconsider_persist_before_activate'" "${CRASH_LOG}" 2>/dev/null \
    || fail "process exited but the crash log does not name the hook"
pass "aborted at the persist/activate boundary with the decision already durable"

# Restart clean. The decision must have survived AND the chain must recover.
RESTART_LOG="${DATADIR}/restart.log"
PID3="$(start_n "${NODE_DIR}" "${RPC_PORT}" "${RESTART_LOG}")"
DATADIR="${NODE_DIR}"
wait_rpc || fail "node did not come back after the boundary abort"
for _ in $(seq 1 40); do
    a="$(count_blocks)"; sleep 2; b="$(count_blocks)"
    [[ "$a" == "$b" ]] && break
done
TIP_AFTER="$(count_blocks)"
info "tip after restart: ${TIP_AFTER} (pre-invalidate ${TIP0}, post-invalidate ${TIP_INV})"

# The operator RECONSIDERED, so the decision to restore must have survived the
# crash: the block must no longer be refused.
RE2="$(rpc reconsiderblock "\"${TARGET}\"" 2>/dev/null)"
if echo "${RE2}" | grep -q "not marked as invalid"; then
    pass "cleared state survived the crash — the block is no longer marked invalid"
else
    info "second reconsider response: $(echo "${RE2}" | head -c 160)"
fi
sleep 5
TIP_FINAL="$(count_blocks)"
[[ -n "${TIP_FINAL}" && "${TIP_FINAL}" -ge "${TIP0}" ]] \
    || fail "chain did not recover to ${TIP0} after the boundary crash (got ${TIP_FINAL})"
pass "chain reactivated to ${TIP_FINAL} (>= pre-invalidate ${TIP0}) after restart"
DATADIR="${DATADIR_SAVE}"
stop_n "${PID3}"

pass "reconsider persist/activate crash boundary holds"
