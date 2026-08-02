#!/usr/bin/env bash
#
# Regression: after one node restarts on an old tip, mines a competing longer
# fork, and reconnects, both nodes must converge back to identical headers,
# Utreexo roots, and filters on the winning chain.
#

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# CTest injects the exact in-tree target path. Keep the conventional fallback
# for developers invoking this script directly.
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
CHECKER="${ROOT_DIR}/tools/check_seed_consistency.py"
BASE_PORT="${BASE_PORT:-}"
NODE_A_RPC=""
NODE_B_RPC=""
NODE_A_P2P=""
NODE_B_P2P=""
PRELOAD_BLOCKS="${PRELOAD_BLOCKS:-120}"
SHORT_FORK_BLOCKS="${SHORT_FORK_BLOCKS:-2}"
LONG_FORK_BLOCKS="${LONG_FORK_BLOCKS:-4}"
DATA_A="/tmp/dinero_restart_reorg_a_$$"
DATA_B="/tmp/dinero_restart_reorg_b_$$"
LOG_A="${DATA_A}.log"
LOG_B="${DATA_B}.log"
REPORT_JSON="/tmp/dinero_restart_reorg_report_$$.json"
PID_A=""
PID_B=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2

    # Capture the live sync state before shutdown flushes the logs.  This is
    # deliberately best-effort: launch failures may occur before RPC/cookies
    # exist, while a sync livelock needs the in-memory selector/scheduler state
    # that is lost as soon as the daemons exit.
    local _health
    for _node in A B; do
        if [[ "${_node}" = "A" ]]; then
            _health="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "blockchain.getsynchealth" '[]' 2>/dev/null || true)"
        else
            _health="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "blockchain.getsynchealth" '[]' 2>/dev/null || true)"
        fi
        if [[ -n "${_health}" ]]; then
            printf -- '--- node %s live sync health ---\n' "${_node}" >&2
            jq '.result // .' <<<"${_health}" >&2 2>/dev/null || printf '%s\n' "${_health}" >&2
        fi
    done

    # Stop the daemons and wait for them to exit BEFORE reading their logs.
    #
    # Their stdout is block-buffered when redirected to a file, so nothing is
    # flushed until the process exits. Tailing while they are still running
    # printed two empty sections on every failure:
    #
    #     --- node A log tail ---
    #     --- node B log tail ---
    #
    # which is why this test's CI output has never carried usable diagnostics.
    # The logs were fine on disk; they simply had not been written yet.
    for _pid in "${PID_A}" "${PID_B}"; do
        [[ -n "${_pid}" ]] && kill "${_pid}" 2>/dev/null || true
    done
    for _pid in "${PID_A}" "${PID_B}"; do
        [[ -n "${_pid}" ]] || continue
        for _ in $(seq 1 40); do
            kill -0 "${_pid}" 2>/dev/null || break
            sleep 0.25
        done
        kill -9 "${_pid}" 2>/dev/null || true
    done
    PID_A=""
    PID_B=""

    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -160 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -160 "${LOG_B}" >&2 || true; }
    exit 1
}

cleanup() {
    [[ -n "${PID_A}" ]] && kill "${PID_A}" 2>/dev/null || true
    [[ -n "${PID_B}" ]] && kill "${PID_B}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_A}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_B}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_A}" "${DATA_B}" "${LOG_A}" "${LOG_B}" "${REPORT_JSON}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v lsof >/dev/null || fail "lsof is required for collision-free port selection"
    command -v python3 >/dev/null || fail "python3 is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

port_range_free() {
    local base="$1"
    local port
    for port in "${base}" "$((base + 1))" "$((base + 100))" "$((base + 101))"; do
        if lsof -nP -iTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1; then
            return 1
        fi
    done
    return 0
}

pick_base_port() {
    local candidate
    for _ in $(seq 1 40); do
        candidate=$((36000 + RANDOM % 12000))
        if port_range_free "${candidate}"; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    printf '[FAIL] unable to find a free RPC/P2P port range after 40 attempts\n' >&2
    return 1
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
    local rpc_port="$1"
    local datadir="$2"
    local method="$3"
    local params_json="$4"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${rpc_port}/"
}

wait_rpc() {
    local rpc_port="$1"
    local datadir="$2"
    for _ in $(seq 1 60); do
        if rpc_call "${rpc_port}" "${datadir}" "getblockcount" '[]' | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_condition() {
    local cmd="$1"
    local message="$2"
    for _ in $(seq 1 90); do
        if eval "${cmd}"; then
            return 0
        fi
        sleep 1
    done
    fail "${message}"
}

start_node() {
    local datadir="$1"
    local rpc_port="$2"
    local p2p_port="$3"
    local logfile="$4"

    mkdir -p "${datadir}"
    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

stop_node() {
    local pid="$1"
    local datadir="$2"
    if [[ -n "${pid}" ]]; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    pkill -f "dinerod.*${datadir}" 2>/dev/null || true
    for _ in $(seq 1 30); do
        if ! pgrep -f "dinerod.*${datadir}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    fail "Timed out waiting for node at ${datadir} to stop"
}

get_height() {
    local rpc_port="$1"
    local datadir="$2"
    local response
    response="$(rpc_call "${rpc_port}" "${datadir}" "getblockcount" '[]' 2>/dev/null || true)"
    if [[ -z "${response}" ]]; then
        printf '%s\n' "-1"
        return 0
    fi
    jq -r '.result // -1' <<<"${response}" 2>/dev/null || printf '%s\n' "-1"
}

get_best_hash() {
    local rpc_port="$1"
    local datadir="$2"
    local response
    response="$(rpc_call "${rpc_port}" "${datadir}" "getbestblockhash" '[]' 2>/dev/null || true)"
    if [[ -z "${response}" ]]; then
        printf '\n'
        return 0
    fi
    jq -r '.result // empty' <<<"${response}" 2>/dev/null || printf '\n'
}

ensure_wallet_address() {
    local rpc_port="$1"
    local datadir="$2"
    local wallet_name="$3"
    rpc_call "${rpc_port}" "${datadir}" "wallet.createhd" "[\"${wallet_name}\"]" >/dev/null 2>&1 || true
    rpc_call "${rpc_port}" "${datadir}" "wallet.getnewaddress" '[]' | jq -r '.result.address // .result // empty'
}

mine_blocks() {
    local rpc_port="$1"
    local datadir="$2"
    local address="$3"
    local blocks="$4"
    local result
    result="$(rpc_call "${rpc_port}" "${datadir}" "generatetoaddress" "[${blocks},\"${address}\"]")"
    jq -e '.error == null' <<<"${result}" >/dev/null || fail "generatetoaddress failed: ${result}"
}

require_tools

if [[ -z "${BASE_PORT}" ]]; then
    BASE_PORT="$(pick_base_port)" || fail "No collision-free port range available"
elif ! port_range_free "${BASE_PORT}"; then
    fail "Requested BASE_PORT range is already in use: ${BASE_PORT}"
fi
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
info "Using collision-free port range rooted at ${BASE_PORT}"

PID_A="$(start_node "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}")"
wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
pass "Both nodes are up"

ADDR_A="$(ensure_wallet_address "${NODE_A_RPC}" "${DATA_A}" "restart-reorg-a")"
[[ -n "${ADDR_A}" ]] || fail "Failed to obtain mining address on node A"
ADDR_B="$(ensure_wallet_address "${NODE_B_RPC}" "${DATA_B}" "restart-reorg-b")"
[[ -n "${ADDR_B}" ]] || fail "Failed to obtain mining address on node B"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B did not connect to node A"
pass "Node B connected to node A"

info "Mining ${PRELOAD_BLOCKS} base blocks on node A"
mine_blocks "${NODE_A_RPC}" "${DATA_A}" "${ADDR_A}" "${PRELOAD_BLOCKS}"
wait_condition "[[ \$(get_height \"${NODE_B_RPC}\" \"${DATA_B}\") -eq \$(get_height \"${NODE_A_RPC}\" \"${DATA_A}\") && \"\$(get_best_hash \"${NODE_B_RPC}\" \"${DATA_B}\")\" = \"\$(get_best_hash \"${NODE_A_RPC}\" \"${DATA_A}\")\" ]]" \
    "Node B did not synchronize the base chain"
BASE_HEIGHT="$(get_height "${NODE_A_RPC}" "${DATA_A}")"
BASE_HASH="$(get_best_hash "${NODE_A_RPC}" "${DATA_A}")"
pass "Base chain synchronized at height ${BASE_HEIGHT}"

python3 "${CHECKER}" \
    --seed a=http://127.0.0.1:${NODE_A_RPC},cookie=$(cookie_file "${DATA_A}") \
    --seed b=http://127.0.0.1:${NODE_B_RPC},cookie=$(cookie_file "${DATA_B}") \
    --samples 6 \
    --height "${BASE_HEIGHT}" \
    --out "${REPORT_JSON}" >/dev/null
pass "Seed consistency checker passed on the shared base chain"

info "Stopping node B to force a restart from the old shared tip"
stop_node "${PID_B}" "${DATA_B}"
PID_B=""

info "Mining ${SHORT_FORK_BLOCKS} short-fork blocks on node A"
mine_blocks "${NODE_A_RPC}" "${DATA_A}" "${ADDR_A}" "${SHORT_FORK_BLOCKS}"
SHORT_HASH_A="$(get_best_hash "${NODE_A_RPC}" "${DATA_A}")"
SHORT_HEIGHT_A="$(get_height "${NODE_A_RPC}" "${DATA_A}")"
[[ "${SHORT_HEIGHT_A}" -eq $((BASE_HEIGHT + SHORT_FORK_BLOCKS)) ]] || fail "Node A short fork height mismatch"

PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not return after restart"
wait_condition "[[ \$(get_height \"${NODE_B_RPC}\" \"${DATA_B}\") -eq ${BASE_HEIGHT} && \"\$(get_best_hash \"${NODE_B_RPC}\" \"${DATA_B}\")\" = \"${BASE_HASH}\" ]]" \
    "Node B did not expose the old shared tip after restart"
RESTART_HEIGHT_B="$(get_height "${NODE_B_RPC}" "${DATA_B}")"
RESTART_HASH_B="$(get_best_hash "${NODE_B_RPC}" "${DATA_B}")"
[[ "${RESTART_HEIGHT_B}" -eq "${BASE_HEIGHT}" ]] || fail "Node B height changed unexpectedly across restart"
[[ "${RESTART_HASH_B}" = "${BASE_HASH}" ]] || fail "Node B tip hash changed unexpectedly across restart"
pass "Node B restarted at the old shared tip"

info "Mining ${LONG_FORK_BLOCKS} longer competing fork on restarted node B"
mine_blocks "${NODE_B_RPC}" "${DATA_B}" "${ADDR_B}" "${LONG_FORK_BLOCKS}"
LONG_HASH_B="$(get_best_hash "${NODE_B_RPC}" "${DATA_B}")"
LONG_HEIGHT_B="$(get_height "${NODE_B_RPC}" "${DATA_B}")"
[[ "${LONG_HEIGHT_B}" -gt "${SHORT_HEIGHT_A}" ]] || fail "Node B fork is not longer than node A fork"
[[ "${LONG_HASH_B}" != "${SHORT_HASH_A}" ]] || fail "Expected distinct competing fork hashes"

rpc_call "${NODE_A_RPC}" "${DATA_A}" "addnode" "[\"127.0.0.1:${NODE_B_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(get_height \"${NODE_A_RPC}\" \"${DATA_A}\") -eq \$(get_height \"${NODE_B_RPC}\" \"${DATA_B}\") && \"\$(get_best_hash \"${NODE_A_RPC}\" \"${DATA_A}\")\" = \"\$(get_best_hash \"${NODE_B_RPC}\" \"${DATA_B}\")\" ]]" \
    "Nodes did not converge after reconnecting the competing fork"

FINAL_HEIGHT="$(get_height "${NODE_A_RPC}" "${DATA_A}")"
FINAL_HASH="$(get_best_hash "${NODE_A_RPC}" "${DATA_A}")"
[[ "${FINAL_HEIGHT}" -eq "${LONG_HEIGHT_B}" ]] || fail "Converged height does not match the longer restarted fork"
[[ "${FINAL_HASH}" = "${LONG_HASH_B}" ]] || fail "Nodes did not converge to node B's longer restarted fork"
[[ "${FINAL_HASH}" != "${SHORT_HASH_A}" ]] || fail "Node A failed to leave its shorter fork"
pass "Nodes converged to the restarted node's longer fork"

python3 "${CHECKER}" \
    --seed a=http://127.0.0.1:${NODE_A_RPC},cookie=$(cookie_file "${DATA_A}") \
    --seed b=http://127.0.0.1:${NODE_B_RPC},cookie=$(cookie_file "${DATA_B}") \
    --samples 6 \
    --height "${BASE_HEIGHT}" \
    --height "${FINAL_HEIGHT}" \
    --height $((BASE_HEIGHT + 1)) \
    --height $((FINAL_HEIGHT - 1)) \
    --out "${REPORT_JSON}" >/dev/null
pass "Headers, Utreexo roots, and filters match after restart + reorg convergence"
