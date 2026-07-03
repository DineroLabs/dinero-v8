#!/usr/bin/env bash
#
# Shielded epoch reset (hard-fork cutover) — boundary + reorg + restart +
# reindex acceptance test.
#
# The reset discards the whole shielded pool at the cutover height H: the
# commitment tree, the anchor history, and the nullifier set are wiped, making
# every pre-cutover note unspendable and starting a fresh empty epoch. This test
# drives a real regtest daemon across H and asserts the four properties that
# unit tests structurally cannot cover (a wrong live/reindex/restart path is a
# consensus split, invisible to in-process tests):
#
#   Leg 0  Semantic   — shield + partially unshield BEFORE H (creating notes,
#                       nullifiers, and a non-trivial anchor window), mine across
#                       H, then assert the shielded balance is gone and the old
#                       notes are UNSPENDABLE (unshield fails).
#   Leg B  Reorg      — invalidateblock below H forces a disconnect ACROSS the
#                       cutover, reconsiderblock replays it; the restored tip's
#                       shieldedStateHash must equal the original (invertibility).
#   Leg C  Restart    — stop + restart rehydrates shielded state from ChainDB;
#                       the hash must be unchanged (the nullifier-CF PURGE must
#                       have persisted — the resurrection path).
#   Leg D  Reindex    — --reindex rebuilds the chain from block files; its
#                       shieldedStateHash at the same tip must equal the live
#                       node's (the reindexer must apply the SAME reset + record
#                       anchors symmetrically, or a reindexed node forks).
#
# shieldedStateHash == daemon.shieldedstatehash == ComputeShieldedReorgStateHash
# (DSR2), which binds the utreexo forest commitment + shielded tree root/size +
# nullifier set content + full anchor history. It is the exact value every fleet
# node commits to, so equality across these legs is the real consensus oracle.
#
# The cutover height is forced low on REGTEST via the test-only daemon flag
# --consensus-shielded-epoch-reset-height=H (which also activates cv-binding at
# H per the chainparams invariant). That flag is hard-refused on any non-regtest
# chain.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"
RUN_ID=$$
DATADIR="/tmp/dinero_epoch_reset_${RUN_ID}"
LOG="${DATADIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0

# The wallet enforces a hardcoded 100-confirmation coinbase maturity (see
# wallet_manager.cpp COINBASE_MATURITY), independent of the regtest chainparam,
# so coinbase can only be shielded after ~100 blocks. Mine past maturity first,
# then shield/unshield, then place the cutover above all of that.
MATURITY_H="${MATURITY_H:-105}"          # coinbase from early blocks is spendable here
SHIELD_A_CONFIRM_H=$((MATURITY_H + 2))   # first shield mined + confirmed
SHIELD_B_CONFIRM_H=$((MATURITY_H + 4))   # second shield mined + confirmed
UNSHIELD_CONFIRM_H=$((MATURITY_H + 6))   # unshield (nullifier) mined + confirmed
RESET_HEIGHT="${RESET_HEIGHT:-115}"      # cutover (must be > UNSHIELD_CONFIRM_H)
FINAL_HEIGHT="${FINAL_HEIGHT:-$((RESET_HEIGHT + 5))}"
REORG_FROM="${REORG_FROM:-$((RESET_HEIGHT - 2))}"   # invalidate here → disconnect across H (after the pre-cutover shielded setup)
RPC_PORT="${RPC_PORT:-$((25000 + RANDOM % 500))}"
P2P_PORT="${P2P_PORT:-$((RPC_PORT + 1))}"
RPC_TIMEOUT=25

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -f "${LOG}" ]]; then
        printf -- '--- daemon log tail ---\n' >&2
        tail -n 80 "${LOG}" >&2 || true
    fi
    cleanup
    exit 1
}

cleanup() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
        kill -TERM "${PID}" 2>/dev/null || true
        for _ in $(seq 1 12); do kill -0 "${PID}" 2>/dev/null || break; sleep 1; done
        kill -KILL "${PID}" 2>/dev/null || true
    fi
    if [[ "${KEEP_ON_FAIL}" -eq 0 ]]; then
        rm -rf "${DATADIR}" 2>/dev/null || true
    else
        info "preserving ${DATADIR} for inspection"
    fi
}
trap cleanup EXIT

# --- raw JSON-RPC over cookie auth ---------------------------------------
rpc() {
    local method="$1"; shift
    local params="$*"
    local json_params="[]"
    [[ -n "${params}" ]] && json_params="[${params}]"
    local cookie
    cookie="$(cat "${DATADIR}/.cookie" 2>/dev/null || true)"
    [[ -z "${cookie}" ]] && return 1
    curl -s --connect-timeout 2 --max-time "${RPC_TIMEOUT}" \
        -u "${cookie}" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${json_params},\"id\":1}" \
        "http://127.0.0.1:${RPC_PORT}" 2>/dev/null
}
rpc_field_string() {
    echo "$1" | tr -d '\n\t' \
        | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -n1
}
rpc_field_number() {
    echo "$1" | tr -d '\n\t' \
        | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\([0-9.]*\).*/\1/p" | head -n1
}
rpc_top_string() {
    echo "$1" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1
}
rpc_top_number() {
    echo "$1" | tr -d '\n\t' \
        | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -n1
}
ok_response() { echo "$1" | tr -d '\n\t ' | grep -q '"error":null'; }

state_hash() {
    local resp h
    resp="$(rpc daemon.shieldedstatehash)"
    h="$(rpc_field_string "${resp}" state_hash)"
    [[ -n "${h}" ]] || fail "daemon.shieldedstatehash returned no state_hash: ${resp}"
    printf '%s' "${h}"
}

wait_rpc() {
    for _ in $(seq 1 40); do
        local r; r="$(rpc getblockcount || true)"
        if [[ -n "${r}" ]]; then
            local h; h="$(rpc_top_number "${r}")"
            [[ -n "${h}" ]] && return 0
        fi
        sleep 1
    done
    fail "RPC never came up"
}

start_daemon() {
    local extra="$1"
    # shellcheck disable=SC2086
    "${DINEROD}" -regtest -datadir="${DATADIR}" \
        -rpcport="${RPC_PORT}" -port="${P2P_PORT}" -listen=0 \
        --consensus-shielded-epoch-reset-height="${RESET_HEIGHT}" \
        ${extra} >>"${LOG}" 2>&1 &
    PID=$!
    wait_rpc
}

stop_daemon() {
    rpc stop >/dev/null 2>&1 || true
    for _ in $(seq 1 20); do kill -0 "${PID}" 2>/dev/null || break; sleep 1; done
    kill -KILL "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

mine_to() {
    local target="$1"
    local cur; cur="$(rpc_top_number "$(rpc getblockcount)")"
    local n=$(( target - cur ))
    [[ "${n}" -le 0 ]] && return 0
    local resp; resp="$(rpc generatetoaddress "${n}, \"${MINING_ADDR}\"")"
    ok_response "${resp}" || fail "generatetoaddress to ${target} failed: ${resp}"
    for _ in $(seq 1 40); do
        [[ "$(rpc_top_number "$(rpc getblockcount)")" == "${target}" ]] && return 0
        sleep 1
    done
    fail "tip never reached ${target} (at $(rpc_top_number "$(rpc getblockcount)"))"
}

# ── 1. Spin up a fresh regtest node with the cutover forced at RESET_HEIGHT ──
mkdir -p "${DATADIR}"
info "starting dinerod regtest at ${DATADIR} rpc=${RPC_PORT} reset_height=${RESET_HEIGHT}"
start_daemon ""

grep -q "shielded epoch reset + cv-binding forced at height ${RESET_HEIGHT}" "${LOG}" \
    || fail "daemon did not honor --consensus-shielded-epoch-reset-height=${RESET_HEIGHT}"
pass "daemon activated the shielded epoch reset at height ${RESET_HEIGHT} (regtest override)"

WALLET_RESP="$(rpc wallet.createhd "\"epoch_reset\"")"
MINING_ADDR="$(rpc_field_string "${WALLET_RESP}" first_address)"
[[ -n "${MINING_ADDR}" ]] || fail "could not create wallet / extract first_address: ${WALLET_RESP}"

# ── 2. Build pre-cutover shielded state: mine to maturity, shield, unshield ──
# shield_ok — outer JSON-RPC success AND no inner result.error (the shielded
# RPCs return {"error":null,"result":{"error":"insufficient_funds",...}} on an
# internal failure, so the outer check alone is not enough).
shield_ok() {
    ok_response "$1" || return 1
    local inner; inner="$(rpc_field_string "$1" status)"
    [[ "${inner}" == "shielded" || "${inner}" == "unshielded" ]]
}

info "mining to coinbase maturity (${MATURITY_H})"
mine_to "${MATURITY_H}"

# Two shields then one unshield: leaves ONE live (unspent) note plus a nullifier
# and a non-trivial commitment tree + anchor window — so the reset has real state
# to discard and there is a live pre-cutover note whose unspendability we can test
# after the cutover. (unshield consumes a whole note; change returns transparent.)
info "shielding 5 DIN (note A)"
SHIELD_A="$(rpc wallet.shield "5.0")"
shield_ok "${SHIELD_A}" || fail "wallet.shield A failed: ${SHIELD_A}"
mine_to "${SHIELD_A_CONFIRM_H}"

info "shielding 3 DIN (note B)"
SHIELD_B="$(rpc wallet.shield "3.0")"
shield_ok "${SHIELD_B}" || fail "wallet.shield B failed: ${SHIELD_B}"
mine_to "${SHIELD_B_CONFIRM_H}"

info "unshielding 2 DIN (spends one note → a real nullifier before the cutover)"
UNSHIELD_RESP="$(rpc wallet.unshield "2.0")"
shield_ok "${UNSHIELD_RESP}" || fail "wallet.unshield failed: ${UNSHIELD_RESP}"
mine_to "${UNSHIELD_CONFIRM_H}"

BAL_RESP="$(rpc wallet.shieldedbalance)"
BAL_BEFORE="$(rpc_field_number "${BAL_RESP}" balance_una)"
TREE_BEFORE="$(rpc_field_number "${BAL_RESP}" tree_size)"
info "pre-cutover shielded balance_una=${BAL_BEFORE:-?} tree_size=${TREE_BEFORE:-?}"
# A live note must remain (spendable pre-cutover) and the tree must hold both
# commitments — so the reset genuinely discards a populated pool.
awk "BEGIN{exit !(${BAL_BEFORE:-0} > 0)}" \
    || fail "expected a live pre-cutover shielded note, balance_una='${BAL_BEFORE}'"
awk "BEGIN{exit !(${TREE_BEFORE:-0} >= 2)}" \
    || fail "expected commitment tree size >= 2 pre-cutover, got '${TREE_BEFORE}'"
pass "pre-cutover shielded pool is populated (live note + nullifier + ${TREE_BEFORE}-leaf tree + anchor window)"

# Sanity: we must still be below the cutover before crossing it.
CUR="$(rpc_top_number "$(rpc getblockcount)")"
[[ "${CUR}" -lt "${RESET_HEIGHT}" ]] || fail "already at/past cutover before crossing (cur=${CUR})"

# ── 3. Cross the cutover and beyond ─────────────────────────────────────────
info "mining across the cutover H=${RESET_HEIGHT} to ${FINAL_HEIGHT}"
mine_to "${FINAL_HEIGHT}"

# ── Leg 0: pre-cutover notes are UNSPENDABLE after the reset ─────────────────
# (The wallet may still LIST its old notes — it doesn't observe the consensus
#  reset directly — so the robust signal is that a spend of a pre-cutover note
#  FAILS: its anchor was discarded, so membership can't be proven.)
BAL_AFTER="$(rpc_field_number "$(rpc wallet.shieldedbalance)" balance_una)"
info "post-cutover wallet-reported shielded balance_una = ${BAL_AFTER:-?} (informational)"
# Spending a pre-cutover note must NOT succeed: its anchor was discarded by the
# reset, so consensus rejects the spend as anchor-invalid. (The wallet may still
# build the tx — the outer JSON-RPC call returns error:null — but shield_ok also
# checks the inner status, which is set only when the spend is accepted.)
UNSHIELD_AFTER="$(rpc wallet.unshield "1.0")"
if shield_ok "${UNSHIELD_AFTER}"; then
    fail "SEMANTIC VIOLATION: unshield of a pre-cutover note was ACCEPTED after the reset (a discarded note was spendable across the cutover): ${UNSHIELD_AFTER}"
fi
REJECT="$(rpc_field_string "${UNSHIELD_AFTER}" reject_reason)"
: "${REJECT:=$(rpc_field_string "${UNSHIELD_AFTER}" error)}"
info "  spend of a pre-cutover note correctly rejected (${REJECT:-rejected})"
pass "Leg 0: pre-cutover shielded notes are unspendable after the reset"

S0="$(state_hash)"
TIP_HASH="$(rpc_top_string "$(rpc getbestblockhash)")"
info "S0 (post-cutover tip @ ${FINAL_HEIGHT}) = ${S0}"

# ── Leg B: reorg ACROSS the cutover must be a perfect inverse ────────────────
REORG_HASH="$(rpc_top_string "$(rpc getblockhash "${REORG_FROM}")")"
[[ -n "${REORG_HASH}" ]] || fail "could not look up block at height ${REORG_FROM}"
info "Leg B: invalidateblock @ ${REORG_FROM} (< H) → disconnect ACROSS the cutover"
ok_response "$(rpc invalidateblock "\"${REORG_HASH}\"")" || fail "invalidateblock failed"
DISC="$(rpc_top_number "$(rpc getblockcount)")"
[[ "${DISC}" -lt "${RESET_HEIGHT}" ]] || fail "disconnect did not cross the cutover (tip=${DISC})"
info "  post-disconnect tip=${DISC} (below H); reconsidering"
ok_response "$(rpc reconsiderblock "\"${REORG_HASH}\"")" || fail "reconsiderblock failed"
for _ in $(seq 1 40); do
    [[ "$(rpc_top_number "$(rpc getblockcount)")" == "${FINAL_HEIGHT}" ]] && break
    sleep 1
done
[[ "$(rpc_top_number "$(rpc getblockcount)")" == "${FINAL_HEIGHT}" ]] \
    || fail "reconsiderblock did not restore tip to ${FINAL_HEIGHT}"
[[ "$(rpc_top_string "$(rpc getbestblockhash)")" == "${TIP_HASH}" ]] \
    || fail "reconsiderblock restored a DIFFERENT tip hash (split-brain)"
S_REORG="$(state_hash)"
[[ "${S_REORG}" == "${S0}" ]] \
    || fail "Leg B: reorg-across-cutover is NOT invertible: S0=${S0} S_reorg=${S_REORG}"
pass "Leg B: reorg across the cutover is a perfect inverse (S0 == S_reorg)"

# ── Leg C: restart must rehydrate the SAME state (purge persisted) ───────────
info "Leg C: stop + restart (rehydrate shielded state from ChainDB)"
stop_daemon
start_daemon ""
for _ in $(seq 1 40); do
    [[ "$(rpc_top_number "$(rpc getblockcount)")" == "${FINAL_HEIGHT}" ]] && break
    sleep 1
done
[[ "$(rpc_top_number "$(rpc getblockcount)")" == "${FINAL_HEIGHT}" ]] \
    || fail "tip not at ${FINAL_HEIGHT} after restart"
S_RESTART="$(state_hash)"
[[ "${S_RESTART}" == "${S0}" ]] \
    || fail "Leg C: RESURRECTION — restart rehydrated a DIFFERENT state: S0=${S0} S_restart=${S_RESTART} (the nullifier-CF purge did not persist)"
pass "Leg C: restart across the cutover rehydrates the identical state (purge persisted)"

# ── Leg D: reindex must reproduce the live node's state at the same tip ──────
# Remove the standalone anchor-history flat file first: it lives in blockchain/
# NEXT TO the chaindb dir that --reindex swaps, so it would otherwise survive
# stale and let the post-reindex load short-circuit to the OLD (live) anchors —
# masking whether the reindex actually reconstructs correct anchor state. With it
# gone, the reindexed ChainDB is the only source, so Leg D genuinely gates the
# reindexed anchor_history (not just the tree/nullifiers).
info "Leg D: stop + --reindex (rebuild from block files), compare to live"
stop_daemon
rm -f "${DATADIR}/blockchain/shielded_anchor_history.bin" 2>/dev/null || true
info "  removed stale anchor flat file so reindex must reconstruct anchors"
start_daemon "--reindex"
for _ in $(seq 1 90); do
    [[ "$(rpc_top_number "$(rpc getblockcount)")" == "${FINAL_HEIGHT}" ]] && break
    sleep 1
done
[[ "$(rpc_top_number "$(rpc getblockcount)")" == "${FINAL_HEIGHT}" ]] \
    || fail "reindex did not rebuild to tip ${FINAL_HEIGHT}"
S_REINDEX="$(state_hash)"
[[ "${S_REINDEX}" == "${S0}" ]] \
    || fail "Leg D: LIVE-vs-REINDEX SPLIT — reindexed shieldedStateHash differs: S0=${S0} S_reindex=${S_REINDEX} (reindexer reset/anchor recording diverges from the live path)"
pass "Leg D: reindex reproduces the live node's shieldedStateHash (no live-vs-reindex split)"

echo
pass "shielded epoch reset boundary holds across reorg + restart + reindex"
pass "invariant shieldedStateHash @ ${FINAL_HEIGHT} = ${S0}"
exit 0
