#!/usr/bin/env bash
#
# CSN shielded-spend sync regression test.
#
# A stateless / CSN node must be able to sync a chain that contains a shielded
# SPEND. Before the fix, stateless mode early-returned from ConnectBlockInternal
# before the shielded-apply section, so a CSN never built the commitment tree /
# anchor history / nullifier set — and failed "anchor-invalid" on the first
# shielded spend (and its shielded double-spend detection was non-functional).
#
# This test: a bridge node mines a chain containing a shield + an unshield (a
# real shielded spend); a stateless node syncs the whole chain. Asserts:
#   1. the CSN reaches the bridge tip (it did NOT stall at the spend block), and
#   2. the CSN's shieldedStateHash now MATCHES the bridge's (it builds the same
#      shielded state as a full node — the fix).
#
# No epoch reset here — this is the plain CSN shielded-spend path.

set -euo pipefail

SYNC_TIMEOUT=${SYNC_TIMEOUT:-180}
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
SHIELD_H=$((MATURITY_H + 2))
UNSHIELD_H=$((MATURITY_H + 4))
FINAL_H=$((MATURITY_H + 6))

DATADIR_BRIDGE=""; DATADIR_CSN=""
cleanup() {
    [[ -n "$DATADIR_BRIDGE" ]] && pkill -9 -f "dinerod.*${DATADIR_BRIDGE}" 2>/dev/null || true
    [[ -n "$DATADIR_CSN" ]] && pkill -9 -f "dinerod.*${DATADIR_CSN}" 2>/dev/null || true
    sleep 1
    if [[ $EXIT_CODE -ne 0 ]]; then
        echo "=== CSN daemon.log (tail) ==="; [[ -f "${DATADIR_CSN}/daemon.log" ]] && tail -40 "${DATADIR_CSN}/daemon.log" || true
        [[ "$KEEP_TMP_ON_FAIL" == "1" ]] && { echo "kept: $DATADIR_BRIDGE $DATADIR_CSN"; return; }
    fi
    [[ -d "$DATADIR_BRIDGE" ]] && rm -rf "$DATADIR_BRIDGE"; [[ -d "$DATADIR_CSN" ]] && rm -rf "$DATADIR_CSN"
}
EXIT_CODE=0; trap 'EXIT_CODE=$?; cleanup' EXIT
fail() { echo "FAILED: $1"; exit 1; }
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
DATADIR_BRIDGE=$(mktemp -d -t dinero_spendsync_bridge_XXXXXX)
DATADIR_CSN=$(mktemp -d -t dinero_spendsync_csn_XXXXXX)

info "[bridge] starting"
"$DINEROD" --regtest --datadir="$DATADIR_BRIDGE" --rpcport="$RPC_B" --port="$P2P_B" --listen=1 \
    --utreexo=1 --utreexo-bridge=1 >> "${DATADIR_BRIDGE}/daemon.log" 2>&1 &
wait_ready "$RPC_B" "$DATADIR_BRIDGE" 30 || fail "bridge did not start"
ADDR=$(field "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.createhd '"spendsync"')" first_address)
[[ -n "$ADDR" ]] || fail "wallet create failed"

info "[bridge] mining to maturity, then shield + unshield (a real shielded spend)"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$MATURITY_H" "$ADDR" || fail "mine maturity"
shield_ok "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.shield "5.0")" || fail "shield"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$SHIELD_H" "$ADDR" || fail "mine after shield"
shield_ok "$(rpc "$RPC_B" "$DATADIR_BRIDGE" wallet.unshield "2.0")" || fail "unshield"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$UNSHIELD_H" "$ADDR" || fail "mine after unshield"
mine_to "$RPC_B" "$DATADIR_BRIDGE" "$FINAL_H" "$ADDR" || fail "mine to final"
B_TIP=$(besthash "$RPC_B" "$DATADIR_BRIDGE"); B_SH=$(statehash "$RPC_B" "$DATADIR_BRIDGE")
info "  bridge tip=${FINAL_H} sh=${B_SH}"

info "[csn] starting stateless node, syncing the spend-bearing chain"
"$DINEROD" --regtest --datadir="$DATADIR_CSN" --rpcport="$RPC_C" --port="$P2P_C" --listen=1 \
    --utreexo=1 --utreexo-stateless=1 --connect="127.0.0.1:${P2P_B}" >> "${DATADIR_CSN}/daemon.log" 2>&1 &
wait_ready "$RPC_C" "$DATADIR_CSN" 30 || fail "CSN did not start"

wait_height "$RPC_C" "$DATADIR_CSN" "$FINAL_H" "$SYNC_TIMEOUT" \
    || fail "CSN did not sync the spend-bearing chain to ${FINAL_H} (stalled at the shielded spend? got $(height "$RPC_C" "$DATADIR_CSN")) — check for anchor-invalid"
[[ "$(besthash "$RPC_C" "$DATADIR_CSN")" == "$B_TIP" ]] || fail "CSN tip hash != bridge"
pass "CSN synced a chain containing a shielded spend (no anchor-invalid stall)"

C_SH=$(statehash "$RPC_C" "$DATADIR_CSN")
info "  csn sh=${C_SH}"
[[ -n "$C_SH" ]] || fail "CSN shieldedstatehash empty"
[[ "$C_SH" == "$B_SH" ]] \
    || fail "CSN shieldedStateHash != bridge: the CSN did not build identical shielded state (bridge=${B_SH} csn=${C_SH})"
pass "CSN shieldedStateHash matches the full node (CSN now builds the shielded tree/anchors/nullifiers)"

echo
pass "CSN validates + tracks shielded spends (spend-sync regression closed)"
exit 0
