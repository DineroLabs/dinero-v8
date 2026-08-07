#!/usr/bin/env bash
#
# reorg_harness.sh — shared single-node regtest machinery for reorg-forcing
# integration tests.
#
# Extracted from the invalidateblock/reconsiderblock/generatetoaddress
# machinery already used by test_utreexo_reorg_state_restoration.sh,
# test_interrupted_reorg_fail_safe.sh and test_mine_after_invalidate.sh —
# same cookie-auth curl RPC pattern, same start/stop-with-wait-for-exit
# discipline, same random-port picking. This file adds nothing new to that
# machinery except `force_reorg`, which sequences it to produce a reorg of an
# EXACT, caller-chosen depth.
#
# Source this from a test script:
#   . "$(dirname "$0")/reorg_harness.sh"
#
# Exposes:
#   start_node                          — start (or restart) the daemon
#   stop_node                           — graceful stop, waits for exit
#   rpc <method> [params_json]          — raw JSON-RPC call, prints the
#                                          full response envelope
#   rpc_result <method> [params_json]   — same call, prints only .result
#   force_reorg --disconnect N --connect M
#                                       — force exactly one reorg event with
#                                          disconnected=N, connected=M
#                                          (requires M > N; see below)
#   extend_chain --blocks N             — mine N blocks with no fork
#                                          (connect-only, not a reorg)
#
# Requires: curl, jq. Requires DINEROD (env) or ${PROJECT_ROOT}/build/dinerod.

set -eu

# NOTE on `A && { ...; return N; }` / `A || { ...; return N; }` guards below:
# safe as an interior statement, but NEVER write one as the LAST statement of
# a function. Under `set -e`, a function's return status is the status of its
# last-executed command; if that command is such a guard and the guard does
# NOT fire, the guard's own (nonzero) short-circuit status becomes the
# function's return status, and the caller sees a spurious failure. Hit this
# once in extend_chain() during development — end functions with an explicit
# `return 0` (or a command whose natural success status is what you want)
# instead.

REORG_HARNESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REORG_HARNESS_PROJECT_ROOT="$(cd "${REORG_HARNESS_DIR}/../.." && pwd)"

if [ -n "${DINEROD:-}" ] && [ -x "${DINEROD}" ]; then
    : # already set by caller/CTest ENVIRONMENT
elif [ -x "${REORG_HARNESS_PROJECT_ROOT}/build/dinerod" ]; then
    DINEROD="${REORG_HARNESS_PROJECT_ROOT}/build/dinerod"
else
    echo "reorg_harness.sh: dinerod not found (set DINEROD or build it)" >&2
    exit 1
fi

command -v curl >/dev/null 2>&1 || { echo "reorg_harness.sh: curl is required" >&2; exit 1; }
command -v jq   >/dev/null 2>&1 || { echo "reorg_harness.sh: jq is required" >&2; exit 1; }

REORG_HARNESS_DATA_DIR=""
REORG_HARNESS_LOG=""
REORG_HARNESS_PID=""
REORG_HARNESS_RPC_PORT=""
REORG_HARNESS_P2P_PORT=""
REORG_HARNESS_WALLET_PORT=""
REORG_HARNESS_MINER_ADDR=""

_reorg_harness_pick_ports() {
    local candidate
    for _ in $(seq 1 40); do
        candidate=$((38000 + RANDOM % 8000))
        if command -v lsof >/dev/null 2>&1; then
            lsof -nP -iTCP:"${candidate}" -sTCP:LISTEN >/dev/null 2>&1 && continue
            lsof -nP -iTCP:"$((candidate + 1))" -sTCP:LISTEN >/dev/null 2>&1 && continue
        fi
        REORG_HARNESS_RPC_PORT="${candidate}"
        REORG_HARNESS_P2P_PORT="$((candidate + 1))"
        REORG_HARNESS_WALLET_PORT="$((candidate + 2))"
        return 0
    done
    echo "reorg_harness.sh: unable to find a free port pair" >&2
    exit 1
}

_reorg_harness_cookie_file() {
    if [ -f "${REORG_HARNESS_DATA_DIR}/.cookie" ]; then
        printf '%s\n' "${REORG_HARNESS_DATA_DIR}/.cookie"
        return 0
    fi
    if [ -f "${REORG_HARNESS_DATA_DIR}/regtest/.cookie" ]; then
        printf '%s\n' "${REORG_HARNESS_DATA_DIR}/regtest/.cookie"
        return 0
    fi
    return 1
}

# rpc <method> [params_json] — prints the raw JSON-RPC response envelope.
rpc() {
    local method="$1"
    local params="${2:-[]}"
    local cookie_path cookie
    cookie_path="$(_reorg_harness_cookie_file 2>/dev/null || true)"
    [ -n "${cookie_path}" ] || { echo "reorg_harness.sh: no RPC cookie yet (node not started?)" >&2; return 1; }
    cookie="$(tr -d '\n' < "${cookie_path}")"
    curl -s --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${REORG_HARNESS_RPC_PORT}/"
}

# rpc_result <method> [params_json] — the RPC call's bare .result value
# (e.g. a number or string), unquoted. Used by positive controls that need
# to compare a scalar (height before/after) rather than inspect the envelope.
rpc_result() {
    rpc "$1" "${2:-[]}" | jq -r '.result'
}

_reorg_harness_rpc_failed() {
    local compact
    compact="$(printf '%s' "$1" | tr -d '\n\t ')"
    [ -z "${compact}" ] && return 0
    case "${compact}" in
        *'"error":null'*) return 1 ;;
        *'"error":{'*) return 0 ;;
    esac
    return 1
}

_reorg_harness_result() {
    jq -c '.result' <<<"$1"
}

_reorg_harness_wait_rpc() {
    local i
    for i in $(seq 1 120); do
        rpc getblockcount '[]' 2>/dev/null | jq -e '.error == null and (.result >= 0)' >/dev/null 2>&1 && return 0
        sleep 0.5
    done
    echo "reorg_harness.sh: daemon RPC did not become ready" >&2
    return 1
}

_reorg_harness_wait_height() {
    local want="$1" i h
    for i in $(seq 1 240); do
        h="$(rpc getblockcount '[]' 2>/dev/null | jq -r '.result // empty')"
        [ -n "${h}" ] && [ "${h}" -eq "${want}" ] && return 0
        sleep 0.25
    done
    echo "reorg_harness.sh: height did not reach ${want} (currently ${h:-?})" >&2
    return 1
}

## return 0 iff the process exited on its own within budget; return 1 if it
## had to be escalated to SIGKILL. Callers that care whether shutdown was
## CLEAN (not just "gone") must check this rather than only checking "gone".
_reorg_harness_wait_exit() {
    local pid="$1" i
    for i in $(seq 1 100); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 0.2
    done
    kill -9 "${pid}" 2>/dev/null || true
    return 1
}

_reorg_harness_ensure_miner_addr() {
    [ -n "${REORG_HARNESS_MINER_ADDR}" ] && return 0
    local result
    result="$(rpc wallet.getnewaddress '[]')"
    _reorg_harness_rpc_failed "${result}" && { echo "reorg_harness.sh: wallet.getnewaddress failed: ${result}" >&2; return 1; }
    REORG_HARNESS_MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${result}")"
    [ -n "${REORG_HARNESS_MINER_ADDR}" ] || { echo "reorg_harness.sh: empty mining address" >&2; return 1; }
}

# start_node — starts the daemon fresh on first call. On a later call after
# stop_node it restarts against the SAME datadir (so callers can test restart
# behaviour), reusing the ports already picked.
start_node() {
    if [ -z "${REORG_HARNESS_DATA_DIR}" ]; then
        REORG_HARNESS_DATA_DIR="$(mktemp -d -t dinero_reorg_feed_XXXXXX)"
        REORG_HARNESS_LOG="${REORG_HARNESS_DATA_DIR}.log"
        _reorg_harness_pick_ports
    fi

    "${DINEROD}" \
        --regtest \
        --datadir="${REORG_HARNESS_DATA_DIR}" \
        --rpcport="${REORG_HARNESS_RPC_PORT}" \
        --port="${REORG_HARNESS_P2P_PORT}" \
        --wallet-socket-port="${REORG_HARNESS_WALLET_PORT}" \
        --listen=0 \
        >>"${REORG_HARNESS_LOG}" 2>&1 &
    REORG_HARNESS_PID=$!

    _reorg_harness_wait_rpc || {
        echo "--- daemon log tail ---" >&2
        tail -80 "${REORG_HARNESS_LOG}" >&2 || true
        return 1
    }
    _reorg_harness_ensure_miner_addr
}

# stop_node — graceful RPC stop, falling back to signal + wait-for-exit.
#
# Returns 1 if the daemon had to be SIGKILLed to make it exit. This matters
# beyond cleanup hygiene: Gate 3 (a restart records nothing) is only a
# meaningful test of the reorg recorder if the shutdown it restarts from was
# clean. A SIGKILLed process can leave a partially-written datadir, and a
# restart over that could show total:0 for the wrong reason (a botched
# reload) rather than the right one (no replay). Callers that depend on a
# clean shutdown — as the gate script does, calling this as a plain
# statement under `set -e` — get a real failure here instead of a silently
# meaningless pass.
stop_node() {
    [ -n "${REORG_HARNESS_PID}" ] || return 0
    local pid="${REORG_HARNESS_PID}"
    local forced=0

    rpc stop '[]' >/dev/null 2>&1 || true
    _reorg_harness_wait_exit "${pid}" || forced=1

    if kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        _reorg_harness_wait_exit "${pid}" || forced=1
    fi
    REORG_HARNESS_PID=""

    if [ "${forced}" = "1" ]; then
        echo "reorg_harness.sh: stop_node had to SIGKILL the daemon (dirty shutdown)" >&2
        return 1
    fi
    return 0
}

# extend_chain --blocks N — mine N ordinary blocks on top of the current tip.
# Pure connect, no fork: must NOT be recorded as a reorg.
extend_chain() {
    local blocks=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --blocks) blocks="$2"; shift 2 ;;
            *) echo "extend_chain: unknown arg $1" >&2; return 1 ;;
        esac
    done
    [ -n "${blocks}" ] || { echo "extend_chain: --blocks is required" >&2; return 1; }

    _reorg_harness_ensure_miner_addr
    local result
    result="$(rpc generatetoaddress "[${blocks},\"${REORG_HARNESS_MINER_ADDR}\"]")"
    if _reorg_harness_rpc_failed "${result}"; then
        echo "extend_chain: generatetoaddress failed: ${result}" >&2
        return 1
    fi
    return 0
}

# force_reorg --disconnect N --connect M
#
# Produces exactly ONE reorg event with disconnected=N, connected=M.
# Requires M > N: the technique below wins the reorg on cumulative work
# (more blocks == more work at regtest's fixed difficulty), so the branch
# being connected must be strictly longer than the branch being disconnected.
#
# How it works, and why it is one event and not two or zero:
#
#   1. Mine M blocks from the current tip ("branch NEW"). Remember the hash
#      of NEW's first block (NEW1) and the fork height/hash.
#   2. invalidateblock(NEW1). ChainstateService::InvalidateBlock disconnects
#      NEW's blocks itself, directly via DisconnectTip in its own loop, and
#      only THEN calls ActivateBestChain() (chainstate_service.cpp:10607) to
#      pick up any new best candidate. It DOES call ActivateBestChain — the
#      reason this step produces no reorg.status event is that by the time
#      it runs, InvalidateBlock's manual loop has already moved active_tip_
#      back to the fork itself. ActivateBestChain computes disconnect_path by
#      walking from the CURRENT active_tip_ to the fork point (chainstate_
#      service.cpp:7712-7717) — with active_tip_ already AT the fork, that
#      walk is zero-length, so disconnect_path is empty regardless of what
#      ActivateBestChain does next (confirmed empirically: total stays 0
#      here). This is load-bearing for Gate 2's exact total==1: if a future
#      refactor moves the manual disconnect out of InvalidateBlock (or drops
#      it), ActivateBestChain would see active_tip_ still on NEW and compute
#      a real disconnect_path here too, and total would read 2, not 1.
#   3. Mine N blocks from the fork ("branch OLD"). This is a plain connect-
#      only extension (is_reorg is keyed on a nonempty disconnect_path, and
#      there isn't one) — not recorded either, and it establishes what will
#      become the disconnected branch.
#   4. reconsiderblock(NEW1). This revalidates NEW and re-adds its tip as a
#      candidate WITHOUT touching the active tip itself, then calls
#      ActivateBestChain with the active tip still on OLD. Because NEW (M
#      blocks) now outweighs OLD (N blocks), ActivateBestChain computes a
#      real disconnect_path (OLD's N blocks, back to the fork) and connect_
#      path (NEW's M blocks, forward from the fork) in the SAME call, and
#      that is what reorg_log_.Record(N, M) fires on.
#
# Verified interactively against this build: total goes 0 -> 0 -> 0 -> 1
# across steps 2/3/4, with the single recorded event carrying exactly
# disconnected=N, connected=M.
force_reorg() {
    local disconnect="" connect=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --disconnect) disconnect="$2"; shift 2 ;;
            --connect) connect="$2"; shift 2 ;;
            *) echo "force_reorg: unknown arg $1" >&2; return 1 ;;
        esac
    done
    [ -n "${disconnect}" ] && [ -n "${connect}" ] || {
        echo "force_reorg: --disconnect and --connect are both required" >&2
        return 1
    }
    if [ "${connect}" -le "${disconnect}" ]; then
        echo "force_reorg: --connect (${connect}) must be > --disconnect (${disconnect})" \
            "for the connected branch to win on cumulative work" >&2
        return 1
    fi

    _reorg_harness_ensure_miner_addr

    local fork_height result new1_hash
    fork_height="$(rpc getblockcount '[]' | jq -r '.result')"

    # Step 1: mine branch NEW (M blocks).
    result="$(rpc generatetoaddress "[${connect},\"${REORG_HARNESS_MINER_ADDR}\"]")"
    _reorg_harness_rpc_failed "${result}" && { echo "force_reorg: mining NEW branch failed: ${result}" >&2; return 1; }
    _reorg_harness_wait_height "$((fork_height + connect))"

    new1_hash="$(rpc getblockhash "[$((fork_height + 1))]" | jq -r '.result')"
    [ -n "${new1_hash}" ] && [ "${new1_hash}" != "null" ] || {
        echo "force_reorg: could not resolve NEW branch's first block hash" >&2
        return 1
    }

    # Step 2: invalidate NEW's first block — rolls back to the fork, no
    # reorg.status event (see comment above).
    result="$(rpc invalidateblock "[\"${new1_hash}\"]")"
    _reorg_harness_rpc_failed "${result}" && { echo "force_reorg: invalidateblock failed: ${result}" >&2; return 1; }
    _reorg_harness_wait_height "${fork_height}"

    # Step 3: mine branch OLD (N blocks) — plain connect-only extension.
    result="$(rpc generatetoaddress "[${disconnect},\"${REORG_HARNESS_MINER_ADDR}\"]")"
    _reorg_harness_rpc_failed "${result}" && { echo "force_reorg: mining OLD branch failed: ${result}" >&2; return 1; }
    _reorg_harness_wait_height "$((fork_height + disconnect))"

    # Step 4: reconsider NEW — since M > N, this wins on work and triggers a
    # single combined disconnect(N)/connect(M) activation.
    result="$(rpc reconsiderblock "[\"${new1_hash}\"]")"
    _reorg_harness_rpc_failed "${result}" && { echo "force_reorg: reconsiderblock failed: ${result}" >&2; return 1; }
    _reorg_harness_wait_height "$((fork_height + connect))"
}
