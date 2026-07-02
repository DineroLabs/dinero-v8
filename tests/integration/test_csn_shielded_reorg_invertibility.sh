#!/usr/bin/env bash
#
# CSN shielded reorg invertibility regtest — the e2e teeth for the CSN shielded
# reorg branch (shared connect/disconnect funnel + DisconnectTip-CSN rollback +
# ABC-CSN replay apply + replay-connect undo/shielded persistence).
#
# A stateless / CSN node must survive a REORG whose disconnected AND replayed
# branches carry REAL shielded spends, ending byte-identical to a full node.
# This drives a bridge (full, --utreexo-bridge) that builds shielded traffic and
# forces CSN-side reorgs (invalidateblock + heavier remine, CSN already synced),
# and asserts on the stateless node under test.
#
# Shielded traffic is built with the deterministic `shielded_tx_builder` (not the
# wallet) so a note's nullifier is REPRODUCIBLE across branches and re-spends —
# the builder only spends leaf 0, so exactly one note is shielded (as the first
# shielded output, in the common prefix) and every spend of it yields the same
# nullifier N.
#
# Legs (each prints [PASS]/[FAIL]/[XFAIL]; script exits nonzero if any hard leg
# fails):
#   A  reorg invertibility  — branch X (shielded spend of N) is DISCONNECTED and
#      branch Y (shielded spend of N) is REPLAYED on the CSN; assert the reorg
#      disconnected >=1 block, CSN shieldedStateHash == bridge, and the CSN kept
#      accepting the replayed shielded-spend block (no anchor-invalid).
#   B  double-spend probe    — after A's reorg, re-spend N (whose nullifier is in
#      CSN state via the REPLAYED block); the CSN must REJECT it (nullifier
#      present). Neuter Task 5 => the CSN ACCEPTS it (the scary failure).
#   C  reorg across the epoch-reset height H — KNOWN DEFECT (see below). Asserts
#      the true invariant (post-reorg CSN hash == bridge). Marked XFAIL because
#      the CSN reorg-across-reset path leaves a DIVERGENT anchor history today.
#   D  second reorg / undo durability — after A's reorg, a SECOND reorg
#      disconnects the replay-connected branch-Y blocks; assert it SUCCEEDS (no
#      "Missing undo data" in the CSN log) and hashes reconverge. Neuter Task 6
#      => "Missing undo data".
#
# shieldedStateHash == daemon.shieldedstatehash == ComputeShieldedReorgStateHash
# (DSR2): binds utreexo forest commitment + shielded tree root/size + nullifier
# set + full anchor history. It is the exact value every fleet node commits to.
#
# ── KNOWN DEFECT (Leg C, XFAIL) ─────────────────────────────────────────────
# A CSN that reorgs ACROSS the epoch-reset height H ends at the correct tip with
# a correct utreexo forest and an empty (0/0) shielded pool, but a shieldedState-
# Hash that DIFFERS from both the bridge and a fresh CSN that syncs the same
# branch directly — i.e. its anchor history is wrong. The full-node path is
# invertible across H (test_shielded_epoch_reset_boundary.sh Leg B passes), so
# this is a CSN-specific divergence in the reset-crossing disconnect/replay undo.
# Leg C stands as the ready regression test; it fails by design until that path
# is fixed. Do NOT downgrade it to a counts check (0/0 == 0/0 is vacuous).

set -uo pipefail

SYNC_TIMEOUT=${SYNC_TIMEOUT:-180}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}
RESET_HEIGHT=${RESET_HEIGHT:-112}   # must sit ABOVE the pre-reset shielded activity (shield@106, spend@107)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then DINEROD="${PROJECT_ROOT}/dinerod"
else echo "dinerod not found"; exit 1; fi

if [[ -x "${PROJECT_ROOT}/build/tests/integration/shielded_tx_builder" ]]; then
    BUILDER="${PROJECT_ROOT}/build/tests/integration/shielded_tx_builder"
elif [[ -x "${PROJECT_ROOT}/build/shielded_tx_builder" ]]; then
    BUILDER="${PROJECT_ROOT}/build/shielded_tx_builder"
else echo "shielded_tx_builder not found (build the target)"; exit 1; fi

command -v jq >/dev/null 2>&1 || { echo "jq required"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 required"; exit 1; }

MATURITY_H=105
SHIELD_VALUE_UNA=500000000       # 5 DIN shielded into note seed 16 (leaf 0)
FEE_UNA=50000
NOTE_SEED=16

# ── datadirs / cleanup ──────────────────────────────────────────────────────
DIRS=()
cleanup() {
    for d in "${DIRS[@]:-}"; do [[ -n "$d" ]] && pkill -9 -f "dinerod.*${d}" 2>/dev/null || true; done
    sleep 1
    if [[ $EXIT_CODE -ne 0 ]]; then
        for d in "${DIRS[@]:-}"; do
            [[ -f "${d}/daemon.log" ]] && { echo "=== ${d}/daemon.log (tail) ==="; tail -50 "${d}/daemon.log"; }
        done
        [[ "$KEEP_TMP_ON_FAIL" == "1" ]] && { echo "kept: ${DIRS[*]:-}"; return; }
    fi
    for d in "${DIRS[@]:-}"; do [[ -d "$d" ]] && rm -rf "$d"; done
}
EXIT_CODE=0
trap 'EXIT_CODE=$?; cleanup' EXIT

# ── result tracking ─────────────────────────────────────────────────────────
FAILED=0
declare -a SUMMARY
leg_pass()  { echo "  [PASS] $1"; SUMMARY+=("PASS  $1"); }
leg_fail()  { echo "  [FAIL] $1"; SUMMARY+=("FAIL  $1"); FAILED=1; }
leg_xfail() { echo "  [XFAIL] (known defect) $1"; SUMMARY+=("XFAIL $1"); }
leg_xpass() { echo "  [XPASS] $1"; SUMMARY+=("XPASS $1"); FAILED=1; }   # defect fixed → alert
info()      { echo "$1"; }

# ── raw JSON-RPC over cookie auth ───────────────────────────────────────────
rpc() {
    local port=$1 datadir=$2 method=$3; shift 3; local params="$*"
    local cookie; cookie=$(cat "${datadir}/.cookie" 2>/dev/null); [[ -z "$cookie" ]] && return 1
    local jp="[]"; [[ -n "$params" ]] && jp="[$params]"
    curl -s --connect-timeout 2 --max-time 30 -u "$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$jp,\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}
ok()      { echo "$1" | tr -d '\n\t ' | grep -q '"error":null'; }
jget()    { echo "$1" | jq -r "$2 // empty"; }
statehash(){ jget "$(rpc "$1" "$2" daemon.shieldedstatehash)" '.result.state_hash'; }
tree_sz() { jget "$(rpc "$1" "$2" blockchain.getsynchealth)" '.result.shielded_tree_size'; }
null_ct() { jget "$(rpc "$1" "$2" blockchain.getsynchealth)" '.result.shielded_nullifier_count'; }
ucommit() { jget "$(rpc "$1" "$2" blockchain.getutreexocommitment)" '.result.commitment'; }
height()  { jget "$(rpc "$1" "$2" getblockcount)" '.result'; }
besthash(){ jget "$(rpc "$1" "$2" getbestblockhash)" '.result'; }
blockhash(){ jget "$(rpc "$1" "$2" getblockhash "$3")" '.result'; }

wait_ready() { local p=$1 d=$2 t=$3 s; s=$(date +%s); while true; do (( $(date +%s)-s > t )) && return 1; [[ -f "$d/.cookie" ]] && { local h; h=$(height "$p" "$d" 2>/dev/null); [[ -n "$h" ]] && return 0; }; sleep 1; done; }
wait_height(){ local p=$1 d=$2 tgt=$3 t=$4 s; s=$(date +%s); while true; do (( $(date +%s)-s > t )) && return 1; [[ "$(height "$p" "$d" 2>/dev/null)" == "$tgt" ]] && return 0; sleep 1; done; }
wait_tip()   { local bp=$1 bd=$2 cp=$3 cd=$4 t=$5 s; s=$(date +%s); while true; do (( $(date +%s)-s > t )) && return 1; local bh ch; bh=$(besthash "$bp" "$bd"); ch=$(besthash "$cp" "$cd"); [[ -n "$bh" && "$bh" == "$ch" ]] && return 0; sleep 1; done; }
mine()       { ok "$(rpc "$1" "$2" generatetoaddress "$3, \"$4\"")"; }
# getnewaddress can transiently return empty while the wallet is initializing;
# retry so we never proceed with an empty (invalid) address — an empty change
# address silently breaks every createrawtransaction for the whole run.
newaddr() {
    local a t
    for t in $(seq 1 20); do
        a=$(jget "$(rpc "$1" "$2" wallet.getnewaddress "\"taproot\",\"$3\"")" '.result.address // .result')
        [[ -n "$a" ]] && { printf '%s' "$a"; return 0; }
        sleep 1
    done
    return 1
}

# assert the CSN log recorded a reorg with disconnect>=1
assert_disconnect() {
    local csn_log=$1 label=$2
    if grep -Eq "\[ABC-CSN\] STATELESS reorg: fork=[0-9]+ disconnect=[1-9]" "$csn_log"; then
        local line; line=$(grep -E "\[ABC-CSN\] STATELESS reorg: fork=[0-9]+ disconnect=[1-9]" "$csn_log" | tail -1)
        info "    reorg proof: ${line#*\]}"
        return 0
    fi
    echo "    NO disconnect>0 reorg found in CSN log for ${label} (all disconnect=0 is NOT a reorg)"
    return 1
}

# Shield SHIELD_VALUE_UNA into note NOTE_SEED as the FIRST shielded output.
# wallet.createrawtransaction briefly errors right after a block is mined (wallet
# still settling) and LOCKS the input it does select, so this retries over TIME
# and over DISTINCT mature UTXOs until one full construction (create+attach+sign)
# succeeds.
build_and_send_shield() {
    local port=$1 datadir=$2 change=$3
    local deadline=$(( $(date +%s) + 50 ))
    local utxos n i utxo utx uvo usp uam uamu chgv raw shtx sign ssh=""
    while [[ $(date +%s) -lt $deadline ]]; do
        local rawlist; rawlist=$(rpc "$port" "$datadir" wallet.listunspent '101,9999999')
        utxos=$(echo "$rawlist" | jq -c '[.result[] | select(.spendable==true and .witness_version==1 and .is_mature==true)]' 2>/dev/null)
        n=$(echo "${utxos:-[]}" | jq 'length' 2>/dev/null || echo 0)
        for i in $(seq 0 $((n - 1))); do
            [[ "$n" -ge 1 ]] || break
            utxo=$(echo "$utxos" | jq -c ".[$i]")
            utx=$(jget "$utxo" '.txid'); uvo=$(jget "$utxo" '.vout'); usp=$(jget "$utxo" '.scriptPubKey')
            uam=$(jget "$utxo" '.amount'); uamu=$(jget "$utxo" '.amount_una')
            # amount_una is occasionally absent in a listunspent snapshot taken
            # while the wallet is still settling; derive it from .amount then.
            [[ -z "$uamu" && -n "$uam" ]] && uamu=$(python3 -c "print(int(round(float('${uam}')*1e8)))" 2>/dev/null)
            [[ -n "$utx" && -n "$usp" && -n "$uamu" && "$uamu" -gt $((SHIELD_VALUE_UNA + FEE_UNA)) ]] \
                || { echo "    [shield i=$i] skip utx=${utx:0:12} uamu=${uamu} usp_empty=$([[ -z "$usp" ]] && echo 1 || echo 0)" >&2; continue; }
            chgv=$(python3 -c "print(f'{(${uamu}-${SHIELD_VALUE_UNA}-${FEE_UNA})/1e8:.8f}')")
            local crr; crr=$(rpc "$port" "$datadir" wallet.createrawtransaction "[{\"txid\":\"$utx\",\"vout\":$uvo}],[{\"$change\":$chgv}]")
            raw=$(jget "$crr" '.result.hex')
            [[ -n "$raw" ]] || { echo "    [shield i=$i] createraw empty chgv=$chgv resp=$(echo "$crr" | jq -c '.' 2>/dev/null | head -c 220)" >&2; continue; }
            shtx=$("$BUILDER" attach-shield-output --raw-tx "$raw" --shield-value-una "$SHIELD_VALUE_UNA" \
                --explicit-fee-una "$FEE_UNA" --note-seed "$NOTE_SEED" 2>/dev/null | jq -r '.hex // empty')
            [[ -n "$shtx" ]] || continue
            sign=$(rpc "$port" "$datadir" wallet.signrawtransaction "\"$shtx\",[{\"txid\":\"$utx\",\"vout\":$uvo,\"scriptPubKey\":\"$usp\",\"amount\":$uam}]")
            [[ "$(jget "$sign" '.result.complete')" == "true" ]] && { ssh=$(jget "$sign" '.result.hex'); break; }
        done
        [[ -n "$ssh" ]] && break
        sleep 2
    done
    [[ -n "$ssh" ]] || { echo "    shield tx construction failed (no UTXO yielded a signed shield tx within the window)"; return 1; }
    ok "$(rpc "$port" "$datadir" sendrawtransaction "\"$ssh\"")" || { echo "    shield sendrawtransaction rejected"; return 1; }
    return 0
}

# Emit a shielded transfer HEX spending note NOTE_SEED (leaf 0) → output <seed>.
# Every call yields the SAME nullifier N (deterministic per note+leaf).
transfer_hex() {
    local out_seed=$1
    "$BUILDER" build-transfer --input-note-seed "$NOTE_SEED" --input-value-una "$SHIELD_VALUE_UNA" \
        --input-leaf-index 0 --output-note-seed "$out_seed" \
        --output-value-una $((SHIELD_VALUE_UNA - FEE_UNA)) --explicit-fee-una "$FEE_UNA" | jq -r '.hex // empty'
}
transfer_nullifier() {
    "$BUILDER" build-transfer --input-note-seed "$NOTE_SEED" --input-value-una "$SHIELD_VALUE_UNA" \
        --input-leaf-index 0 --output-note-seed 200 \
        --output-value-una $((SHIELD_VALUE_UNA - FEE_UNA)) --explicit-fee-una "$FEE_UNA" | jq -r '.input_nullifier // empty'
}

start_bridge() { # port p2p datadir [reset_height]
    local extra=""; [[ -n "${4:-}" ]] && extra="--consensus-shielded-epoch-reset-height=$4"
    "$DINEROD" --regtest --datadir="$3" --rpcport="$1" --port="$2" --wallet-socket-port="$(( $1 + 100 ))" \
        --listen=1 --utreexo=1 --utreexo-bridge=1 $extra >> "$3/daemon.log" 2>&1 &
}
start_csn() { # port p2p datadir bridge_p2p [reset_height]
    local extra=""; [[ -n "${5:-}" ]] && extra="--consensus-shielded-epoch-reset-height=$5"
    "$DINEROD" --regtest --datadir="$3" --rpcport="$1" --port="$2" \
        --listen=1 --utreexo=1 --utreexo-stateless=1 --connect="127.0.0.1:$4" $extra >> "$3/daemon.log" 2>&1 &
}

echo "================================================================="
echo "  CSN SHIELDED REORG INVERTIBILITY  (Legs A/B/C/D)"
echo "================================================================="

# =============================================================================
#  PAIR 1  (no epoch reset):  Leg A (reorg) → Leg B (double-spend) → Leg D (2nd reorg)
# =============================================================================
RB=$((44000 + RANDOM % 400)); PB=$((RB+1)); RC=$((RB+2)); PC=$((RB+3))
DB1=$(mktemp -d -t dinero_csnreorg_bridge_XXXXXX); DC1=$(mktemp -d -t dinero_csnreorg_csn_XXXXXX)
DIRS+=("$DB1" "$DC1")
CLOG="$DC1/daemon.log"

info "[pair1] starting bridge"
start_bridge "$RB" "$PB" "$DB1"
wait_ready "$RB" "$DB1" 30 || { leg_fail "A: bridge did not start"; }
ADDR=$(newaddr "$RB" "$DB1" miner); ADDR_Y=$(newaddr "$RB" "$DB1" minerY); ADDR_Z=$(newaddr "$RB" "$DB1" minerZ)
CHG=$(newaddr "$RB" "$DB1" change)
[[ -n "$ADDR" && -n "$ADDR_Y" && -n "$ADDR_Z" && -n "$CHG" ]] || leg_fail "A: wallet address derivation failed"

info "[pair1] mining to maturity + shielding one note (leaf 0)"
mine "$RB" "$DB1" "$MATURITY_H" "$ADDR" && wait_height "$RB" "$DB1" "$MATURITY_H" 60 || leg_fail "A: mine to maturity"
build_and_send_shield "$RB" "$DB1" "$CHG" || leg_fail "A: shield note construction"
mine "$RB" "$DB1" 1 "$ADDR"; SHIELD_H=$(height "$RB" "$DB1")
info "  shielded note at height ${SHIELD_H} (leaf 0); this is the common prefix"

NULL_N=$(transfer_nullifier)
info "  deterministic nullifier N=${NULL_N}"

# ---- branch X: mine the shielded spend of N + two more blocks ----
info "[pair1] branch X: shielded spend of N, then extend"
ok "$(rpc "$RB" "$DB1" sendrawtransaction "\"$(transfer_hex 64)\"")" || leg_fail "A: branch-X transfer rejected"
mine "$RB" "$DB1" 1 "$ADDR"; SPEND_H=$(height "$RB" "$DB1"); SPEND_HASH=$(blockhash "$RB" "$DB1" "$SPEND_H")
mine "$RB" "$DB1" 2 "$ADDR"; XTIP_H=$(height "$RB" "$DB1")
info "  branch X: spend@${SPEND_H} tip=${XTIP_H}"

info "[pair1] starting CSN, syncing branch X (with the shielded spend)"
start_csn "$RC" "$PC" "$DC1" "$PB"
wait_ready "$RC" "$DC1" 30 || leg_fail "A: CSN did not start"
wait_height "$RC" "$DC1" "$XTIP_H" "$SYNC_TIMEOUT" || leg_fail "A: CSN did not sync branch X to ${XTIP_H}"
X_BSH=$(statehash "$RB" "$DB1"); X_CSH=$(statehash "$RC" "$DC1")
[[ -n "$X_CSH" && "$X_BSH" == "$X_CSH" ]] || leg_fail "A: CSN sh != bridge on branch X (pre-reorg): b=${X_BSH} c=${X_CSH}"

# ---- force the reorg: invalidate branch X's spend block, remine heavier branch Y ----
info "[pair1] forcing reorg: invalidate branch-X spend block, remine heavier branch Y"
ok "$(rpc "$RB" "$DB1" invalidateblock "\"$SPEND_HASH\"")" || leg_fail "A: invalidateblock failed"
rpc "$RB" "$DB1" mempool.clear '' >/dev/null 2>&1
sleep 2
FORK_H=$(height "$RB" "$DB1")
ok "$(rpc "$RB" "$DB1" sendrawtransaction "\"$(transfer_hex 99)\"")" || leg_fail "A: branch-Y transfer rejected"
mine "$RB" "$DB1" 1 "$ADDR_Y"; Y_SPEND_H=$(height "$RB" "$DB1")
mine "$RB" "$DB1" 5 "$ADDR_Y"; YTIP_H=$(height "$RB" "$DB1")
info "  branch Y: fork@${FORK_H} spend@${Y_SPEND_H} tip=${YTIP_H}"
wait_tip "$RB" "$DB1" "$RC" "$DC1" "$SYNC_TIMEOUT" || leg_fail "A: CSN did not converge to branch Y tip (csn=$(besthash "$RC" "$DC1"))"
sleep 2

# ---- Leg A assertions ----
info "[Leg A] reorg invertibility"
if [[ "$FAILED" -eq 0 ]]; then
    A_OK=1
    assert_disconnect "$CLOG" "Leg A" || A_OK=0
    A_BSH=$(statehash "$RB" "$DB1"); A_CSH=$(statehash "$RC" "$DC1")
    [[ -n "$A_CSH" && "$A_BSH" == "$A_CSH" ]] || { echo "    CSN sh != bridge post-reorg: b=${A_BSH} c=${A_CSH}"; A_OK=0; }
    # CSN kept accepting the replayed shielded-spend block (no anchor-invalid post-reorg)
    if grep -q "anchor-invalid" "$CLOG"; then echo "    CSN logged anchor-invalid after the reorg"; A_OK=0; fi
    # forest + counts sanity (informational corroboration)
    info "    post-reorg: tip b=$(besthash "$RB" "$DB1") c=$(besthash "$RC" "$DC1") | tree c=$(tree_sz "$RC" "$DC1") null c=$(null_ct "$RC" "$DC1")"
    [[ "$A_OK" -eq 1 ]] && leg_pass "A: reorg disconnected a shielded branch; CSN shieldedStateHash == bridge; no anchor-invalid" \
                        || leg_fail "A: reorg invertibility"
else
    leg_fail "A: prerequisites failed"
fi

# ---- Leg B: double-spend probe (N's nullifier is in CSN state via the replayed block) ----
info "[Leg B] double-spend probe"
DS_RESP=$(rpc "$RC" "$DC1" sendrawtransaction "\"$(transfer_hex 123)\"")
DS_MSG=$(echo "$DS_RESP" | jq -r '(.result.error.message // .error.message // .result // "accepted")' 2>/dev/null)
info "    CSN double-spend response: ${DS_MSG}"
if echo "$DS_MSG" | grep -q "nullifier-duplicate"; then
    leg_pass "B: CSN REJECTED the re-spend of N (nullifier present via replayed block): ${DS_MSG}"
else
    leg_fail "B: CSN did NOT reject the double-spend (nullifier not present?): ${DS_MSG}"
fi

# ---- Leg D: second reorg disconnects the replay-connected branch-Y blocks ----
info "[Leg D] second reorg / undo durability"
if grep -q "Missing undo data" "$CLOG"; then D_PRE_MISS=1; else D_PRE_MISS=0; fi
Y_SPEND_HASH=$(blockhash "$RB" "$DB1" "$Y_SPEND_H")
ok "$(rpc "$RB" "$DB1" invalidateblock "\"$Y_SPEND_HASH\"")" || leg_fail "D: invalidateblock (branch Y spend) failed"
rpc "$RB" "$DB1" mempool.clear '' >/dev/null 2>&1
sleep 2
# branch Z: transparent, heavier than branch Y, mined to a THIRD address
mine "$RB" "$DB1" $((YTIP_H - FORK_H + 2)) "$ADDR_Z"; ZTIP_H=$(height "$RB" "$DB1")
info "  branch Z: fork@${FORK_H} tip=${ZTIP_H} (was Y tip ${YTIP_H})"
wait_tip "$RB" "$DB1" "$RC" "$DC1" "$SYNC_TIMEOUT" || leg_fail "D: CSN did not converge to branch Z tip (csn=$(besthash "$RC" "$DC1"))"
sleep 2
if [[ "$FAILED" -eq 0 || -n "${A_OK:-}" ]]; then
    D_OK=1
    assert_disconnect "$CLOG" "Leg D" || D_OK=0
    # the SECOND reorg's disconnect must not have hit "Missing undo data"
    if [[ "$D_PRE_MISS" -eq 0 ]] && grep -q "Missing undo data" "$CLOG"; then
        echo "    CSN logged 'Missing undo data' during the second reorg (undo not durable)"; D_OK=0
    fi
    D_BSH=$(statehash "$RB" "$DB1"); D_CSH=$(statehash "$RC" "$DC1")
    [[ -n "$D_CSH" && "$D_BSH" == "$D_CSH" ]] || { echo "    CSN sh != bridge after second reorg: b=${D_BSH} c=${D_CSH}"; D_OK=0; }
    [[ "$D_OK" -eq 1 ]] && leg_pass "D: second reorg disconnected replay-connected blocks with no 'Missing undo data'; hashes reconverged" \
                        || leg_fail "D: undo durability"
fi

# stop pair 1
pkill -9 -f "dinerod.*${DB1}" 2>/dev/null || true
pkill -9 -f "dinerod.*${DC1}" 2>/dev/null || true
sleep 1

# =============================================================================
#  PAIR 2  (epoch reset at H):  Leg C — reorg across the cutover (XFAIL)
# =============================================================================
RB2=$((45000 + RANDOM % 400)); PB2=$((RB2+1)); RC2=$((RB2+2)); PC2=$((RB2+3))
DB2=$(mktemp -d -t dinero_csnreorgC_bridge_XXXXXX); DC2=$(mktemp -d -t dinero_csnreorgC_csn_XXXXXX)
DIRS+=("$DB2" "$DC2")
CLOG2="$DC2/daemon.log"
H=$RESET_HEIGHT

info ""
info "[pair2] Leg C: starting bridge with epoch reset forced at H=${H}"
start_bridge "$RB2" "$PB2" "$DB2" "$H"
wait_ready "$RB2" "$DB2" 30 || leg_fail "C: bridge did not start"
grep -q "shielded epoch reset + cv-binding forced at height ${H}" "$DB2/daemon.log" || leg_fail "C: reset height not honored"
CADDR=$(newaddr "$RB2" "$DB2" miner); CADDR_Y=$(newaddr "$RB2" "$DB2" minerY); CCHG=$(newaddr "$RB2" "$DB2" change)
[[ -n "$CADDR" && -n "$CADDR_Y" && -n "$CCHG" ]] || leg_fail "C: wallet address derivation failed"

info "[pair2] common prefix below H: maturity + shield + spend (populates the pre-reset pool)"
mine "$RB2" "$DB2" "$MATURITY_H" "$CADDR" && wait_height "$RB2" "$DB2" "$MATURITY_H" 60 || leg_fail "C: mine to maturity"
build_and_send_shield "$RB2" "$DB2" "$CCHG" || leg_fail "C: shield note construction"
mine "$RB2" "$DB2" 1 "$CADDR"
ok "$(rpc "$RB2" "$DB2" sendrawtransaction "\"$(transfer_hex 64)\"")" || leg_fail "C: prefix transfer rejected"
mine "$RB2" "$DB2" 1 "$CADDR"                 # spend confirmed (below H)
mine "$RB2" "$DB2" 1 "$CADDR"                 # to the fork point (below H)
C_FORK_H=$(height "$RB2" "$DB2")
info "  fork@${C_FORK_H}; pre-reset pool tree=$(tree_sz "$RB2" "$DB2") null=$(null_ct "$RB2" "$DB2")"
[[ "$C_FORK_H" -lt "$H" ]] || leg_fail "C: fork not below reset height H"
C_INV_H=$((C_FORK_H + 1)); C_INV_HASH=""

# branch X crosses H
mine "$RB2" "$DB2" $((H - C_FORK_H + 2)) "$CADDR"; C_XTIP_H=$(height "$RB2" "$DB2")
C_INV_HASH=$(blockhash "$RB2" "$DB2" "$C_INV_H")
info "  branch X tip=${C_XTIP_H} (crossed H=${H})"

info "[pair2] CSN syncs branch X across H"
start_csn "$RC2" "$PC2" "$DC2" "$PB2" "$H"
wait_ready "$RC2" "$DC2" 30 || leg_fail "C: CSN did not start"
wait_height "$RC2" "$DC2" "$C_XTIP_H" "$SYNC_TIMEOUT" || leg_fail "C: CSN did not sync branch X across H"
CX_BSH=$(statehash "$RB2" "$DB2"); CX_CSH=$(statehash "$RC2" "$DC2")
info "  branch X across H: CSN sh $([[ "$CX_BSH" == "$CX_CSH" ]] && echo == || echo !=) bridge (forward-sync)"

info "[pair2] reorg ACROSS H: invalidate below H (above fork), remine heavier branch Y across H"
ok "$(rpc "$RB2" "$DB2" invalidateblock "\"$C_INV_HASH\"")" || leg_fail "C: invalidateblock failed"
rpc "$RB2" "$DB2" mempool.clear '' >/dev/null 2>&1
sleep 2
mine "$RB2" "$DB2" $((C_XTIP_H - C_FORK_H + 3)) "$CADDR_Y"; C_YTIP_H=$(height "$RB2" "$DB2")
info "  branch Y tip=${C_YTIP_H}"
wait_tip "$RB2" "$DB2" "$RC2" "$DC2" "$SYNC_TIMEOUT" || leg_fail "C: CSN did not converge to branch Y tip"
sleep 2

info "[Leg C] reorg across the epoch reset height (XFAIL — known CSN anchor-history defect)"
C_OK=1
assert_disconnect "$CLOG2" "Leg C" || { echo "    Leg C never achieved disconnect>0"; C_OK=0; }
C_BSH=$(statehash "$RB2" "$DB2"); C_CSH=$(statehash "$RC2" "$DC2")
C_BF=$(ucommit "$RB2" "$DB2"); C_CF=$(ucommit "$RC2" "$DC2")
info "    forest commit: bridge=$C_BF csn=$C_CF ($([[ "$C_BF" == "$C_CF" ]] && echo MATCH || echo DIFFER))"
info "    pool: bridge tree=$(tree_sz "$RB2" "$DB2") null=$(null_ct "$RB2" "$DB2") | csn tree=$(tree_sz "$RC2" "$DC2") null=$(null_ct "$RC2" "$DC2")"
info "    shieldedStateHash: bridge=$C_BSH csn=$C_CSH"
if [[ "$C_OK" -eq 0 ]]; then
    leg_fail "C: reorg across H did not disconnect (harness/topology problem, not the known defect)"
elif [[ -n "$C_BSH" && "$C_BSH" == "$C_CSH" ]]; then
    # If this ever passes, the CSN reset-crossing reorg defect is FIXED — alert to remove XFAIL.
    leg_xpass "C: reorg across H is now invertible (CSN sh == bridge) — remove the XFAIL marker"
else
    leg_xfail "C: reorg-across-H leaves a DIVERGENT CSN shieldedStateHash (b=${C_BSH} c=${C_CSH}); forest+counts match — anchor-history defect"
fi

# =============================================================================
#  SUMMARY
# =============================================================================
echo ""
echo "================================================================="
echo "  SUMMARY"
for s in "${SUMMARY[@]:-}"; do echo "  $s"; done
echo "================================================================="
if [[ "$FAILED" -ne 0 ]]; then
    echo "RESULT: FAIL (a hard leg failed)"
    exit 1
fi
echo "RESULT: PASS (Legs A/B/D green; Leg C XFAIL — known CSN reorg-across-reset anchor-history defect)"
exit 0
