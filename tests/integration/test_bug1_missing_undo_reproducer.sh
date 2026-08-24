#!/usr/bin/env bash
#
# Bug 1 reproducer — "missing undo data for active tip" after clean stop/start.
#
# Per memory note project_real_bugs_apr18.md: LA + MO nodes wedged at height
# 6083 with "chainstate recovery required: missing undo data for active tip"
# after a systemd stop/start cycle. Recovered via manual --reindex-chainstate.
#
# This test tries to reproduce that state on a single-node regtest:
#   1. Mine a chain on a fresh node
#   2. Clean SIGTERM stop (systemd-equivalent graceful shutdown)
#   3. Restart
#   4. Issue `invalidateblock(tip_hash)` — forces DisconnectTip, which is the
#      code path that fires "missing undo data for active tip" when persisted
#      metadata is inconsistent.
#
# Variants exercise different crash boundaries in ConnectTip to map which
# crash points leave the persisted state in a wedge-inducing condition.
#
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
DATA_DIR="/tmp/dinero_bug1_reproducer_${RUN_ID}"
LOG_INIT="${DATA_DIR}.init.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_RESTART="${DATA_DIR}.restart.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
note() { printf '[NOTE] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for f in "${LOG_INIT}" "${LOG_CRASH}" "${LOG_RESTART}"; do
        [[ -f "${f}" ]] || continue
        printf -- '--- %s tail ---\n' "$(basename "${f}")" >&2
        tail -120 "${f}" >&2 || true
    done
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_INIT}" "${LOG_CRASH}" "${LOG_RESTART}"
    else
        printf '[INFO] Keeping artifacts for inspection: %s\n' "${DATA_DIR}" >&2
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
        return 0
    fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
        return 0
    fi
    return 1
}

rpc_call() {
    local method="$1"
    local params_json="$2"
    local cookie_path cookie
    cookie_path="$(cookie_file "${DATA_DIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_has_error() {
    local compact
    compact="$(echo "$1" | tr -d '\n\t ')"
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 60); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    env "$@" "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        >"${log_file}" 2>&1 &
    PID=$!
}

graceful_stop() {
    # Systemd-equivalent clean stop: SIGTERM, wait for daemon to exit on its own.
    [[ -n "${PID}" ]] || return 0
    kill -TERM "${PID}" 2>/dev/null || true
    wait_dead "${PID}" || fail "daemon did not exit after SIGTERM"
    wait "${PID}" 2>/dev/null || true
    PID=""
}

mine_blocks() {
    local n="$1" addr="$2"
    local result
    result="$(rpc_call "generatetoaddress" "[${n},\"${addr}\"]")"
    rpc_has_error "${result}" && fail "generatetoaddress failed: ${result}"
}

get_tip_hash() {
    local result
    result="$(rpc_call "getbestblockhash" '[]')"
    rpc_has_error "${result}" && fail "getbestblockhash failed: ${result}"
    jq -r '.result' <<<"${result}"
}

get_tip_height() {
    local result
    result="$(rpc_call "getblockcount" '[]')"
    rpc_has_error "${result}" && fail "getblockcount failed: ${result}"
    jq -r '.result' <<<"${result}"
}

assert_tip_height() {
    local expected="$1" label="$2"
    local actual
    actual="$(get_tip_height)"
    [[ "${actual}" == "${expected}" ]] || fail "${label}: expected height ${expected}, got ${actual}"
}

sync_health() {
    local result
    result="$(rpc_call "blockchain.getsynchealth" '[]')"
    rpc_has_error "${result}" && fail "blockchain.getsynchealth failed: ${result}"
    jq -c '.result' <<<"${result}"
}

# ─────────────────────────────────────────────────────────────────────────────
# Test core: clean-stop / restart / invalidate-tip scenario.
#
# Arguments:
#   $1 — label for this run
#   $2 — DINERO_CRASH_AT env value for the INITIAL run ("" means no crash)
#   $3 — expected init-run outcome ("mine-ok" | "mine-crash")
#
# Returns 0 if the tip's undo is still reachable post-restart (invalidateblock
# succeeds); returns 1 if Bug 1 manifested (invalidateblock fails with
# "missing undo data"); any other failure aborts with fail().
# ─────────────────────────────────────────────────────────────────────────────
run_scenario() {
    local label="$1"
    local crash_hook="$2"
    local expected_outcome="$3"

    info "═══ Scenario: ${label} ═══"

    # Fresh datadir each run so scenarios don't bleed state.
    rm -rf "${DATA_DIR}"
    : >"${LOG_INIT}"
    : >"${LOG_CRASH}"
    : >"${LOG_RESTART}"

    # Step 1: build a chain with NO crash hook. This mirrors the fleet's
    # baseline state (nodes have been running for a while before the stop).
    start_node "${LOG_INIT}"
    wait_rpc || fail "[${label}] init daemon did not reach RPC readiness"

    local addr_result miner_addr
    addr_result="$(rpc_call "wallet.getnewaddress" '[]')"
    rpc_has_error "${addr_result}" && fail "[${label}] wallet.getnewaddress failed: ${addr_result}"
    miner_addr="$(jq -r '.result.address // .result // empty' <<<"${addr_result}")"
    [[ -n "${miner_addr}" ]] || fail "[${label}] empty miner address"

    info "[${label}] pre-mining 10 baseline blocks"
    mine_blocks 10 "${miner_addr}"
    assert_tip_height 10 "[${label}] post-baseline-mine"
    local tip_hash_pre
    tip_hash_pre="$(get_tip_hash)"
    info "[${label}] pre-stop tip=${tip_hash_pre} height=10"

    if [[ "${expected_outcome}" == "mine-ok" ]]; then
        # Clean SIGTERM stop, no further mining.
        graceful_stop
    else
        # Stop cleanly, then restart WITH crash hook and try to mine one block.
        # This matches the fleet: long-running node gets restarted, then a new
        # block needs to connect and something goes wrong mid-ConnectTip.
        graceful_stop
        start_node "${LOG_CRASH}" "DINERO_CRASH_AT=${crash_hook}"
        wait_rpc || fail "[${label}] crash-hook daemon did not reach RPC readiness"
        assert_tip_height 10 "[${label}] crash-hook daemon started at pre-stop tip"
        info "[${label}] triggering crash at hook '${crash_hook}'"
        set +e
        rpc_call "generatetoaddress" "[1,\"${miner_addr}\"]" >/dev/null 2>&1
        set -e
        wait_dead "${PID}" || fail "[${label}] daemon did not crash at hook ${crash_hook}"
        PID=""
        grep -q "DINERO_CRASH" "${LOG_CRASH}" || fail "[${label}] crash hook did not fire as expected"
    fi

    # ── Restart phase ──
    start_node "${LOG_RESTART}"
    wait_rpc || fail "[${label}] daemon did not reach RPC readiness after restart"

    local height_post
    height_post="$(get_tip_height)"
    info "[${label}] post-restart height=${height_post}"

    if [[ "${height_post}" == "0" ]]; then
        note "[${label}] restart landed at height 0 — nothing to disconnect, skipping wedge probe"
        graceful_stop
        return 0
    fi

    local tip_hash_post
    tip_hash_post="$(get_tip_hash)"
    info "[${label}] post-restart tip=${tip_hash_post}"

    # ── Wedge probe: ask daemon to disconnect its own tip ──
    info "[${label}] issuing invalidateblock(${tip_hash_post}) to exercise DisconnectTip"
    local inv_result
    inv_result="$(rpc_call "invalidateblock" "[\"${tip_hash_post}\"]")"

    if rpc_has_error "${inv_result}"; then
        local err_msg
        err_msg="$(jq -r '.error.message // .error // empty' <<<"${inv_result}")"
        if [[ "${err_msg}" == *"missing undo"* ]] || [[ "${err_msg}" == *"chainstate recovery"* ]]; then
            printf '[BUG1-REPRODUCED] [%s] invalidateblock failed: %s\n' "${label}" "${err_msg}"
            graceful_stop
            return 1
        fi
        fail "[${label}] invalidateblock failed for a different reason: ${err_msg}"
    fi

    local health_after
    health_after="$(sync_health)"
    local canonical_aligned
    canonical_aligned="$(jq -r '.canonical_state_aligned // false' <<<"${health_after}")"
    if [[ "${canonical_aligned}" != "true" ]]; then
        note "[${label}] sync health not aligned after disconnect: ${health_after}"
    fi

    pass "[${label}] DisconnectTip succeeded — tip's undo was reachable"
    graceful_stop
    return 0
}

require_tools

RPC_PORT=$((34000 + RUN_ID % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

# ─────────────────────────────────────────────────────────────────────────────
# Scenario A: the production path. Mine → clean SIGTERM → restart → invalidate.
# If this triggers the wedge, we've reproduced Bug 1 as it manifests in the
# fleet (no crash, just clean stop).
# ─────────────────────────────────────────────────────────────────────────────
REPRODUCED_A=0
if ! run_scenario "A: clean-stop then invalidate" "" "mine-ok"; then
    REPRODUCED_A=1
fi

# ─────────────────────────────────────────────────────────────────────────────
# Scenario B: crash AFTER writeUndo but BEFORE setTip. The new block's undo is
# in the flatfile but the persisted tip is unchanged. Restart should land at
# prev tip and invalidate should succeed for whatever block IS at tip.
# ─────────────────────────────────────────────────────────────────────────────
REPRODUCED_B=0
if ! run_scenario "B: crash after_undo_before_tip" "after_undo_before_tip" "mine-crash"; then
    REPRODUCED_B=1
fi

# ─────────────────────────────────────────────────────────────────────────────
# Scenario C: crash AFTER setTip but BEFORE side-state (height index, header
# CF, utreexo checkpoint). Restart should replay forward and land cleanly.
# invalidate should succeed.
# ─────────────────────────────────────────────────────────────────────────────
REPRODUCED_C=0
if ! run_scenario "C: crash after_tip_before_checkpoint" "after_tip_before_checkpoint" "mine-crash"; then
    REPRODUCED_C=1
fi

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
printf '\n=== Bug 1 reproducer summary (run_id=%s) ===\n' "${RUN_ID}"
printf '  A (clean stop)                  : %s\n' \
    "$([[ "${REPRODUCED_A}" == 1 ]] && echo BUG_REPRODUCED || echo ok)"
printf '  B (after_undo_before_tip)       : %s\n' \
    "$([[ "${REPRODUCED_B}" == 1 ]] && echo BUG_REPRODUCED || echo ok)"
printf '  C (after_tip_before_checkpoint) : %s\n' \
    "$([[ "${REPRODUCED_C}" == 1 ]] && echo BUG_REPRODUCED || echo ok)"

if [[ "${REPRODUCED_A}${REPRODUCED_B}${REPRODUCED_C}" == "000" ]]; then
    pass "No wedge reproduced in any scenario. Fleet wedge has a different trigger."
    exit 0
fi

# If we got here, the bug reproduced in at least one scenario — exit 2 so CI
# flags this distinctly from "tooling failure" (exit 1).
exit 2
