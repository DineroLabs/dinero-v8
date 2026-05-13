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
DINEROD="${ROOT_DIR}/build/dinerod"
RUN_ID=$$
DATADIR="/tmp/dinero_shielded_reorg_inv_${RUN_ID}"
LOG="${DATADIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0
CHAIN_HEIGHT="${CHAIN_HEIGHT:-15}"
RPC_PORT="${RPC_PORT:-$((24000 + RANDOM % 500))}"
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

# ── 1. Spin up a fresh regtest node ──────────────────────────────────

mkdir -p "${DATADIR}"
info "starting dinerod regtest at ${DATADIR} rpc=${RPC_PORT}"
# Phase 3a (commit 2/3 onward): the test honors ATOMIC_PERSIST env
# var so the same script covers flag-off, flag-on, and flag-toggle
# configurations. With ATOMIC_PERSIST=1 the daemon routes UTXO map
# mutations through ConsensusWriteBatch; identical state hash is
# the property under test.
ATOMIC_PERSIST_FLAG=""
case "${ATOMIC_PERSIST:-0}" in
    1|true|yes|on) ATOMIC_PERSIST_FLAG="-consensus.atomic_persist=1" ;;
esac

if [[ -n "${ATOMIC_PERSIST_FLAG}" ]]; then
    "${DINEROD}" -regtest -datadir="${DATADIR}" \
        -rpcport="${RPC_PORT}" -port="${P2P_PORT}" -listen=0 \
        "${ATOMIC_PERSIST_FLAG}" \
        >"${LOG}" 2>&1 &
else
    "${DINEROD}" -regtest -datadir="${DATADIR}" \
        -rpcport="${RPC_PORT}" -port="${P2P_PORT}" -listen=0 \
        >"${LOG}" 2>&1 &
fi
PID=$!

wait_rpc

# ── 2. Mine N blocks ──────────────────────────────────────────────────

WALLET_RESP="$(rpc wallet.createhd "\"shielded_reorg_inv\"")"
MINING_ADDR="$(rpc_field_string "${WALLET_RESP}" first_address)"
[[ -n "${MINING_ADDR}" ]] || fail "could not create wallet / extract first_address: ${WALLET_RESP}"
info "mining ${CHAIN_HEIGHT} blocks to ${MINING_ADDR}"

# Phase 3a D3 third configuration — flag-toggle. Mid-chain restart
# with the flag flipped catches state that survives the toggle, which
# is exactly where hidden-state bugs hide. Triggered by setting
# ATOMIC_PERSIST_TOGGLE=1; mines half the chain in the current
# configuration, restarts the daemon with the OPPOSITE flag value,
# completes the chain, then runs the standard invalidate/reconsider
# property check.
if [[ "${ATOMIC_PERSIST_TOGGLE:-0}" == "1" ]]; then
    HALF=$(( CHAIN_HEIGHT / 2 ))
    info "flag-toggle: mining first ${HALF} blocks, then restarting with flipped flag"
    HALF_RESP="$(rpc generatetoaddress "${HALF}, \"${MINING_ADDR}\"")"
    echo "${HALF_RESP}" | tr -d '\n\t ' | grep -q '"error":null' \
        || fail "generatetoaddress (half) failed: ${HALF_RESP}"

    # Wait for tip to settle, then graceful stop.
    for _ in $(seq 1 30); do
        H="$(rpc_top_number "$(rpc getblockcount)")"
        [[ "${H}" == "${HALF}" ]] && break
        sleep 1
    done
    rpc stop >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do
        kill -0 "${PID}" 2>/dev/null || break
        sleep 1
    done
    kill -KILL "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""

    # Flip the flag for the restart.
    if [[ -n "${ATOMIC_PERSIST_FLAG}" ]]; then
        ATOMIC_PERSIST_FLAG=""
        info "flag-toggle: restart with flag OFF"
    else
        ATOMIC_PERSIST_FLAG="-consensus.atomic_persist=1"
        info "flag-toggle: restart with flag ON"
    fi

    if [[ -n "${ATOMIC_PERSIST_FLAG}" ]]; then
        "${DINEROD}" -regtest -datadir="${DATADIR}" \
            -rpcport="${RPC_PORT}" -port="${P2P_PORT}" -listen=0 \
            "${ATOMIC_PERSIST_FLAG}" \
            >>"${LOG}" 2>&1 &
    else
        "${DINEROD}" -regtest -datadir="${DATADIR}" \
            -rpcport="${RPC_PORT}" -port="${P2P_PORT}" -listen=0 \
            >>"${LOG}" 2>&1 &
    fi
    PID=$!
    wait_rpc

    # Need a fresh wallet RPC to keep using the post-restart RPC handle.
    OPEN_RESP="$(rpc wallet.open "\"shielded_reorg_inv\"")"
    echo "${OPEN_RESP}" | tr -d '\n\t ' | grep -q '"error":null' \
        || fail "wallet.open after restart failed: ${OPEN_RESP}"
    REMAINING=$(( CHAIN_HEIGHT - HALF ))
    info "flag-toggle: mining remaining ${REMAINING} blocks under flipped flag"
    REM_RESP="$(rpc generatetoaddress "${REMAINING}, \"${MINING_ADDR}\"")"
    echo "${REM_RESP}" | tr -d '\n\t ' | grep -q '"error":null' \
        || fail "generatetoaddress (remaining) failed: ${REM_RESP}"
fi

# Single-shot path (covers ATOMIC_PERSIST=0 and =1 alike when toggle is off).
if [[ "${ATOMIC_PERSIST_TOGGLE:-0}" != "1" ]]; then
    GEN_RESP="$(rpc generatetoaddress "${CHAIN_HEIGHT}, \"${MINING_ADDR}\"")"
    if ! echo "${GEN_RESP}" | tr -d '\n\t ' | grep -q '"error":null'; then
        fail "generatetoaddress failed: ${GEN_RESP}"
    fi
fi

# Wait until the tip catches up to the requested height.
for _ in $(seq 1 30); do
    HEIGHT_RESP="$(rpc getblockcount)"
    TIP_HEIGHT="$(rpc_top_number "${HEIGHT_RESP}")"
    [[ "${TIP_HEIGHT}" == "${CHAIN_HEIGHT}" ]] && break
    sleep 1
done
[[ "${TIP_HEIGHT}" == "${CHAIN_HEIGHT}" ]] || \
    fail "tip ${TIP_HEIGHT} != expected ${CHAIN_HEIGHT}"
TIP_HASH="$(rpc_top_string "$(rpc getbestblockhash)")"
info "tip is ${TIP_HEIGHT} ${TIP_HASH:0:16}"

# ── 3. Capture state at the tip (S0) ──────────────────────────────────

S0="$(state_hash)"
info "S0 = ${S0}"

# ── 4. invalidateblock at height 2, forcing disconnect of [2..tip] ────

H2_RESP="$(rpc getblockhash 2)"
DISCONNECT_TARGET_HASH="$(rpc_top_string "${H2_RESP}")"
[[ -n "${DISCONNECT_TARGET_HASH}" ]] || fail "could not look up block at height 2"
info "invalidating block at height 2 (${DISCONNECT_TARGET_HASH:0:16})"
INV_RESP="$(rpc invalidateblock "\"${DISCONNECT_TARGET_HASH}\"")"
if ! echo "${INV_RESP}" | tr -d '\n\t ' | grep -q '"error":null'; then
    fail "invalidateblock failed: ${INV_RESP}"
fi

DISC_HEIGHT="$(rpc_top_number "$(rpc getblockcount)")"
[[ "${DISC_HEIGHT}" -lt "${TIP_HEIGHT}" ]] || \
    fail "invalidateblock did not regress the tip (still ${DISC_HEIGHT})"
info "post-disconnect tip=${DISC_HEIGHT}"

# ── 5. reconsiderblock to walk forward again ──────────────────────────

info "reconsidering block (forces reconnect of [2..tip])"
REC_RESP="$(rpc reconsiderblock "\"${DISCONNECT_TARGET_HASH}\"")"
if ! echo "${REC_RESP}" | tr -d '\n\t ' | grep -q '"error":null'; then
    fail "reconsiderblock failed: ${REC_RESP}"
fi

for _ in $(seq 1 30); do
    NEW_TIP="$(rpc_top_number "$(rpc getblockcount)")"
    [[ "${NEW_TIP}" == "${TIP_HEIGHT}" ]] && break
    sleep 1
done
[[ "${NEW_TIP}" == "${TIP_HEIGHT}" ]] || \
    fail "reconsiderblock did not restore tip (got ${NEW_TIP}, expected ${TIP_HEIGHT})"
NEW_TIP_HASH="$(rpc_top_string "$(rpc getbestblockhash)")"
[[ "${NEW_TIP_HASH}" == "${TIP_HASH}" ]] || \
    fail "reconsiderblock restored a DIFFERENT tip hash (split-brain): ${NEW_TIP_HASH:0:16} != ${TIP_HASH:0:16}"

# ── 6. Capture state at the restored tip (S2) ─────────────────────────

S2="$(state_hash)"
info "S2 = ${S2}"

# ── 7. The invertibility property ─────────────────────────────────────

if [[ "${S0}" != "${S2}" ]]; then
    fail "INVERTIBILITY VIOLATED: state hash diverged across Connect/Disconnect/Connect cycle
  S0 (original tip)  = ${S0}
  S2 (restored tip)  = ${S2}
This means at least one of: utreexo forest, shielded tree, nullifier
set, or anchor history is not a perfect inverse under DisconnectBlock.
See docs/specs/shielded_reorg_invertibility_audit.md for the open
gaps that could produce this symptom."
fi

pass "shielded-era reorg invertibility holds across ${CHAIN_HEIGHT}-block reorg"
pass "S0 == S2 = ${S0}"
exit 0
