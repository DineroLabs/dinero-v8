#!/usr/bin/env bash
#
# CSN ConnectTip stateless-recovery shielded-apply regression test (#356).
#
# The bug: ConnectTip's stateless RECOVERY/replay branch (guard
# `current_commitment == block.utreexo->accumulator_root_before`, INFO line
# "Stateless replay path: advancing shared forest from stored proof data")
# fires only when a CSN crashed AFTER a block was durably stored+indexed but
# BEFORE it was connected/tip-advanced. Pre-#356 that branch replayed the
# forest but did NOT apply the block's shielded section (note commitments /
# nullifiers) — so a crash-recovered CSN silently built a WRONG shielded pool
# and diverged from a full node (and could reject or mis-accept later spends).
#
# This leg reaches that branch DETERMINISTICALLY via the Task-3 fault-injection
# hook (env DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT=H makes AcceptBlockFromRPC
# _exit(70) after block H is stored+indexed but before connect):
#
#   1. A bridge (full node) mines a chain with a REAL shielded spend — a shield
#      (note commitment, lands in block SHIELD_BLK) and an unshield (a nullifier
#      + change note, lands in block H). H is the unshield block, chosen so that
#      the shield block connects via the NORMAL stateless path before the abort
#      and ONLY block H is ever connected by the recovery branch — isolating the
#      recovery shielded-apply.
#   2. A CSN syncs with the abort env set to H: it connects <H normally, stores
#      H, then _exit(70) before connecting H. Asserted: exit 70 + abort marker +
#      pre-abort connected tip < H (block H stored-ahead-of-tip).
#   3. The CSN is restarted WITHOUT the env var (bridge stays up). ActivateBest-
#      Chain catches up and connects H through the recovery branch. Asserted, IN
#      ORDER (so a loud shielded-gap stall is caught before a hash compare):
#        (a) CSN reaches the bridge tip FINAL_H (did not stall at H),
#        (b) the recovery branch fired for height H exactly (else the fix is not
#            exercised → FAIL, never pass vacuously),
#        (c) CSN daemon.shieldedstatehash == bridge's (recovery applied shielded
#            state correctly — the primary teeth),
#        (d) the CSN accepts a SUBSEQUENT shielded spend (another unshield mined
#            on the bridge) without an anchor-invalid stall, proving the
#            recovery-built anchor/nullifier state is usable going forward.
#
# NEUTER (proves non-vacuity): revert Task 2's recovery-branch apply
# (`shielded_applied ? &replay_undo : nullptr` -> `nullptr` + drop the
# ApplyStatelessReplayShielded call) → this leg MUST FAIL: either a shielded
# state-hash divergence at FINAL_H (marker advanced) or a loud shielded-gap
# stall short of FINAL_H (marker held). Restore → PASS.

set -uo pipefail

SYNC_TIMEOUT=${SYNC_TIMEOUT:-180}
ABORT_TIMEOUT=${ABORT_TIMEOUT:-120}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
if [[ -n "${DINEROD:-}" ]]; then
    # CTest supplies this (ENVIRONMENT "DINEROD=$<TARGET_FILE:dinerod>"), so the
    # test follows the build directory wherever it is. Honour it and require it
    # to be real — never silently fall through to a guessed path.
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 1; }
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    # Manual/local convenience only.
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    # Fail HERE, naming the paths tried. Launching a non-existent binary and
    # then waiting on its RPC turns a missing file into a 30s timeout reported
    # as "RPC never came up", which reads like a consensus failure.
    echo "dinerod not found (tried: \$DINEROD unset, ${PROJECT_ROOT}/build/dinerod, ${PROJECT_ROOT}/dinerod)" >&2
    echo "set DINEROD=/path/to/dinerod to override" >&2
    exit 1
fi

MATURITY_H=${MATURITY_H:-105}
SHIELD_BLK=$((MATURITY_H + 1))   # 106: shield tx (note commitment) — connects via NORMAL path
SHIELD_H=$((MATURITY_H + 2))     # 107: mine height after submitting shield
H=$((MATURITY_H + 3))            # 108: unshield tx (nullifier + change note) — ABORT + recovery-branch target
UNSHIELD_H=$((MATURITY_H + 4))   # 109: mine height after submitting unshield
FINAL_H=$((MATURITY_H + 6))      # 111: bridge tip
POST_SHIELD_H=$((FINAL_H + 2))   # 113: mine after the subsequent shield (note lands in 112)
POST_H=$((FINAL_H + 5))          # 116: bridge tip after the subsequent unshield (spend lands in 114)

DATADIR_BRIDGE=""; DATADIR_CSN=""
cleanup() {
    [[ -n "$DATADIR_BRIDGE" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    [[ -n "$DATADIR_CSN" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    sleep 1
    if [[ $EXIT_CODE -ne 0 ]]; then
        echo "=== CSN daemon.log (tail) ==="; [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -60 "${DATADIR_CSN}/daemon.log" || true
        [[ "$KEEP_TMP_ON_FAIL" == "1" ]] && { echo "kept: $DATADIR_BRIDGE $DATADIR_CSN"; return; }
    fi
    [[ -d "$DATADIR_BRIDGE" ]] && rm -rf "$DATADIR_BRIDGE"; [[ -d "$DATADIR_CSN" ]] && rm -rf "$DATADIR_CSN"
}
EXIT_CODE=0; trap 'EXIT_CODE=$?; cleanup' EXIT
fail() { echo "  [FAIL] $1"; exit 1; }
pass() { echo "  [PASS] $1"; }
info() { echo "$1"; }

rpc() {
    local port=$1 datadir=$2 method=$3; shift 3; local params="$*"
    local cookie; cookie=$(cat "${datadir}/.cookie" 2>/dev/null); [[ -z "$cookie" ]] && return 1
    local jp="[]"; [[ -n "$params" ]] && jp="[$params]"
    curl -s -u "$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$jp,\"id\":1}" "http://127.0.0.1:${port}" 2>/dev/null
}
ok() { echo "$1" | tr -d '\n\t ' | grep -q '"error":null'; }
field() { echo "$1" | tr -d '\n\t' | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -n1; }
topnum() { echo "$1" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -n1; }
topstr() { echo "$1" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1; }
shield_ok() { ok "$1" || return 1; local s; s=$(field "$1" status); [[ "$s" == "shielded" || "$s" == "unshielded" ]]; }
statehash() { field "$(rpc "$1" "$2" daemon.shieldedstatehash)" state_hash; }
height() { topnum "$(rpc "$1" "$2" getblockcount)"; }
besthash() { topstr "$(rpc "$1" "$2" getbestblockhash)"; }
wait_ready() { local p=$1 d=$2 t=$3 s; s=$(date +%s); while true; do [[ $(($(date +%s)-s)) -gt $t ]] && return 1; [[ -f "$d/.cookie" ]] && { local h; h=$(height "$p" "$d" 2>/dev/null); [[ -n "$h" ]] && return 0; }; sleep 1; done; }
wait_height() { local p=$1 d=$2 tgt=$3 t=$4 s; s=$(date +%s); while true; do [[ $(($(date +%s)-s)) -gt $t ]] && return 1; [[ "$(height "$p" "$d" 2>/dev/null)" == "$tgt" ]] && return 0; sleep 1; done; }
mine_to() { local p=$1 d=$2 tgt=$3 a=$4; local c; c=$(height "$p" "$d"); local n=$((tgt-c)); [[ $n -le 0 ]] && return 0; ok "$(rpc "$p" "$d" generatetoaddress "$n, \"$a\"")" || return 1; wait_height "$p" "$d" "$tgt" 60; }

RPC_B=$((28000+RANDOM%900)); P2P_B=$((RPC_B+1)); RPC_C=$((RPC_B+2)); P2P_C=$((RPC_B+3))
DATADIR_BRIDGE=$(mktemp -d -t dinero_recov_bridge_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_recov_csn_XXXXXX)

# ── Phase 1: bridge mines a chain with a real shielded spend (shield @106, unshield @H=108)
info "[bridge] starting"
"$DINEROD" --regtest --datadir="$DATADIR_BRIDGE" --rpcport="$RPC_B" --port="$P2P_B" --listen=1 \
    --utreexo=1 --utreexo-bridge=1 >> "${DATADIR_BRIDGE}/daemon.log" 2>&1 &
wait_ready "$RPC_B" "$DATADIR_BRIDGE" 30 || fail "bridge did not start"
ADDR=$(field "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.createhd '"recov"')" first_address)
[[ -n "$ADDR" ]] || fail "wallet create failed"

info "[bridge] mining to maturity, then shield (@${SHIELD_BLK}) + unshield (@${H})"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$MATURITY_H" "$ADDR" || fail "mine maturity"
shield_ok "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.shield "5.0")" || fail "shield"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$SHIELD_H" "$ADDR" || fail "mine after shield"
shield_ok "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.unshield "2.0")" || fail "unshield"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$UNSHIELD_H" "$ADDR" || fail "mine after unshield"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$FINAL_H" "$ADDR" || fail "mine to final"
B_TIP=$(besthash "$RPC_B" "$DATADIR_BRIDGE"); B_SH=$(statehash "$RPC_B" "$DATADIR_BRIDGE")
[[ -n "$B_SH" ]] || fail "bridge shieldedstatehash empty"
info "  bridge tip=${FINAL_H} sh=${B_SH}"

# ── Phase 2: CSN syncs with the abort hook armed at H — store H, crash before connect
info "[csn] starting with DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT=${H} (expect _exit 70 after storing H)"
DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT="$H" \
"$DINEROD" --regtest --datadir="$DATADIR_CSN" --rpcport="$RPC_C" --port="$P2P_C" --listen=1 \
    --utreexo=1 --utreexo-stateless=1 --connect="127.0.0.1:${P2P_B}" >> "${DATADIR_CSN}/daemon.log" 2>&1 &
CSN_PID=$!
_waited=0
while kill -0 "$CSN_PID" 2>/dev/null; do
    [[ $_waited -ge $ABORT_TIMEOUT ]] && { kill -9 "$CSN_PID" 2>/dev/null; fail "CSN did not abort within ${ABORT_TIMEOUT}s (hook never fired at H=${H}?)"; }
    sleep 1; _waited=$((_waited+1))
done
wait "$CSN_PID"; CSN_EC=$?
[[ "$CSN_EC" == "70" ]] || fail "CSN abort exit code=${CSN_EC}, expected 70"
grep -q "aborting after block store+index at height ${H}" "${DATADIR_CSN}/daemon.log" \
    || fail "abort marker for height ${H} not found in CSN log"
pass "CSN stored H=${H} then _exit(70) (fault-injection reproduced crash-between-store-and-connect)"

# Block H must be stored-ahead-of-tip: the previous block (H-1) connected but H
# did NOT (its ConnectTip never ran — the abort fired pre-connect). The recovery
# branch must NOT have fired yet (it fires only on the restart). These are
# fixed-string checks (the daemon log interleaves concurrent writers, so parsing
# a numeric max is unreliable; exact-height membership is robust).
PREV_H=$((H - 1))
grep -qF "ConnectTip SUCCEEDED for height ${PREV_H}" "${DATADIR_CSN}/daemon.log" \
    || fail "expected previous block ${PREV_H} to have connected before the abort"
grep -qF "ConnectTip SUCCEEDED for height ${H}" "${DATADIR_CSN}/daemon.log" \
    && fail "block H=${H} was CONNECTED before the abort — the store-ahead-of-tip precondition did not materialize"
grep -qF "Stateless replay path: advancing shared forest from stored proof data at height ${H}" "${DATADIR_CSN}/daemon.log" \
    && fail "recovery branch fired for H=${H} DURING initial sync (expected only on restart)"
pass "block H=${H} stored but unconnected (prev ${PREV_H} connected, H not); recovery branch not yet fired"

# ── Phase 3: restart WITHOUT the env; catch up through the recovery branch
info "[csn] restarting without abort env; ActivateBestChain must connect H via the recovery branch"
"$DINEROD" --regtest --datadir="$DATADIR_CSN" --rpcport="$RPC_C" --port="$P2P_C" --listen=1 \
    --utreexo=1 --utreexo-stateless=1 --connect="127.0.0.1:${P2P_B}" >> "${DATADIR_CSN}/daemon.log" 2>&1 &
wait_ready "$RPC_C" "$DATADIR_CSN" 40 || fail "CSN did not restart"

# (a) reaches FINAL_H — a neutered marker-held build loud-fails the 109 connect and stalls here.
wait_height "$RPC_C" "$DATADIR_CSN" "$FINAL_H" "$SYNC_TIMEOUT" \
    || fail "CSN did not catch up to FINAL_H=${FINAL_H} after restart (stalled at $(height "$RPC_C" "$DATADIR_CSN") — shielded-gap loud-fail on the recovery block?)"
[[ "$(besthash "$RPC_C" "$DATADIR_CSN")" == "$B_TIP" ]] || fail "CSN tip hash != bridge after catch-up"
pass "CSN caught up to FINAL_H=${FINAL_H} through the recovery branch (no shielded-gap stall)"

# (b) the recovery branch fired for H exactly — else the fix is not exercised.
grep -q "Stateless replay path: advancing shared forest from stored proof data at height ${H}" "${DATADIR_CSN}/daemon.log" \
    || fail "recovery branch NEVER fired for H=${H} — this leg is NOT exercising the #356 fix (would pass vacuously)"
pass "recovery branch fired for the shielded-bearing block H=${H} (ConnectTip stateless replay path)"

# (c) shielded state hash convergence — the primary teeth (recovery applied shielded state).
C_SH=$(statehash "$RPC_C" "$DATADIR_CSN")
info "  compare shieldedstatehash: bridge=${B_SH} csn=${C_SH}"
[[ -n "$C_SH" ]] || fail "CSN shieldedstatehash empty"
[[ "$C_SH" == "$B_SH" ]] \
    || fail "shieldedStateHash DIVERGENCE: recovery branch did NOT apply H=${H}'s shielded state (bridge=${B_SH} csn=${C_SH})"
pass "CSN shieldedStateHash matches the full node (recovery branch applied the shielded section)"

# (d) subsequent shielded spend must validate against the recovery-built anchor/
# nullifier state. Shield fresh transparent funds (guarantees a spendable note),
# confirm it, then unshield it — a real spend whose anchor is a tree root built
# on top of the recovery-applied H=${H} note. If the recovery tree diverged, the
# CSN would reject this spend anchor-invalid while connecting the block.
info "[bridge] driving a SUBSEQUENT shielded spend: shield (@$((FINAL_H + 1))) then unshield (@$((FINAL_H + 3))), mining to ${POST_H}"
shield_ok "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.shield "3.0")" || fail "subsequent shield submit"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$POST_SHIELD_H" "$ADDR" || fail "mine after subsequent shield"
shield_ok "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.unshield "1.0")" || fail "subsequent unshield submit"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$POST_H" "$ADDR" || fail "mine to POST_H"
B_TIP2=$(besthash "$RPC_B" "$DATADIR_BRIDGE"); B_SH2=$(statehash "$RPC_B" "$DATADIR_BRIDGE")
wait_height "$RPC_C" "$DATADIR_CSN" "$POST_H" "$SYNC_TIMEOUT" \
    || fail "CSN did not sync the subsequent shielded spend to ${POST_H} (anchor-invalid on recovery-built state? got $(height "$RPC_C" "$DATADIR_CSN"))"
[[ "$(besthash "$RPC_C" "$DATADIR_CSN")" == "$B_TIP2" ]] || fail "CSN tip hash != bridge after subsequent spend"
C_SH2=$(statehash "$RPC_C" "$DATADIR_CSN")
[[ -n "$C_SH2" && -n "$B_SH2" ]] || fail "empty shieldedStateHash after subsequent spend (bridge=${B_SH2} csn=${C_SH2})"
[[ "$C_SH2" == "$B_SH2" ]] || fail "CSN shieldedStateHash != bridge after subsequent spend (bridge=${B_SH2} csn=${C_SH2})"
pass "CSN accepted a subsequent shielded spend on the recovery-built state (no anchor-invalid; hash still matches)"

echo
pass "ConnectTip stateless-recovery branch applies shielded state correctly (#356 recovery regression closed)"
exit 0
