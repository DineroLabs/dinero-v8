#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# CTest injects the exact in-tree target path.  Keep the source-tree fallback
# for developers invoking this script directly from the conventional build.
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
# Ports are randomized per run. Fixed ports collided between back-to-back runs
# and with anything else on the host; see #470/#459.
pick_base_port() {
    local base
    command -v lsof >/dev/null || {
        printf '%s\n' "lsof is required for port preflight" >&2
        return 1
    }
    for _ in $(seq 1 40); do
        base=$(( 35200 + (RANDOM % 200) * 10 ))
        local busy=0
        for off in 0 1 2 100 101 102; do
            if lsof -nP -iTCP:$((base + off)) -sTCP:LISTEN >/dev/null 2>&1; then busy=1; break; fi
        done
        [[ "${busy}" == "0" ]] && { printf '%s\n' "${base}"; return 0; }
    done
    printf '%s\n' "could not find six free test ports after 40 attempts" >&2
    return 1
}
BASE_PORT="${BASE_PORT:-$(pick_base_port)}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_C_RPC=$((BASE_PORT + 2))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
NODE_C_P2P=$((BASE_PORT + 102))
DATA_A="/tmp/dinero_parallel_dl_a_$$"
DATA_B="/tmp/dinero_parallel_dl_b_$$"
DATA_C="/tmp/dinero_parallel_dl_c_$$"
LOG_A="${DATA_A}.log"
LOG_B="${DATA_B}.log"
LOG_C="${DATA_C}.log"
PID_A=""
PID_B=""
PID_C=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -80 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -80 "${LOG_B}" >&2 || true; }
    [[ -f "${LOG_C}" ]] && { printf -- '--- node C log tail ---\n' >&2; tail -80 "${LOG_C}" >&2 || true; }
    exit 1
}
# Wait for a pid to actually exit. kill() only *sends* SIGTERM; the daemon then
# flushes RocksDB and writes logs. Removing the datadir while it is still
# writing is what produced "rm: ...: Directory not empty" (#470 mode 1): rm -rf
# empties a directory, the daemon creates a new file in it, and the final rmdir
# fails. That failure alone became the script exit status even when every
# assertion had passed.
wait_pid_exit() {
    local pid="$1"
    [[ -n "${pid}" ]] || return 0
    for _ in $(seq 1 40); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 0.25
    done
    kill -9 "${pid}" 2>/dev/null || true
    sleep 0.5
    return 0
}

cleanup() {
    [[ -n "${PID_A}" ]] && kill "${PID_A}" 2>/dev/null || true
    [[ -n "${PID_B}" ]] && kill "${PID_B}" 2>/dev/null || true
    [[ -n "${PID_C}" ]] && kill "${PID_C}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_A}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_B}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_C}" 2>/dev/null || true

    wait_pid_exit "${PID_A}"
    wait_pid_exit "${PID_B}"
    wait_pid_exit "${PID_C}"
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        # Never let teardown decide the test result.
        rm -rf "${DATA_A}" "${DATA_B}" "${DATA_C}" "${LOG_A}" "${LOG_B}" "${LOG_C}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v lsof >/dev/null || fail "lsof is required for port preflight"
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
    for _ in $(seq 1 60); do
        if eval "${cmd}"; then
            return 0
        fi
        sleep 1
    done
    fail "${message}"
}

wait_log_contains() {
    local logfile="$1"
    local pattern="$2"
    local message="$3"
    for _ in $(seq 1 60); do
        if grep -Fq "${pattern}" "${logfile}" 2>/dev/null; then
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
    # --connect puts the node in connect-only mode, which suppresses peer
    # discovery (p2p_service.cpp gates anchor/seed bootstrap on !connect_only).
    #
    # Without it this test drifted onto the public network: when node B lost its
    # link to A, the zero-peer reconnect kick dialled real mainnet nodes
    # (173.249.200.59:20999, 172.93.167.32:20999, 92.118.190.62:20999) from a
    # REGTEST node. Those handshakes cannot succeed, B never recovered a local
    # peer, and wait_condition burned its full 60s -- the ~69s failure mode in
    # #470. P2PService itself is correct here (it registered 0 anchors and only
    # localhost seeds for regtest); the mainnet dial originates below it, so the
    # test defends itself rather than relying on that layer.
    local connect_args=()
    if [[ -n "${5:-}" ]]; then
        connect_args+=(--connect="$5")
    fi

    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        ${connect_args[@]+"${connect_args[@]}"} \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

require_tools

PID_A="$(start_node "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}" "127.0.0.1:${NODE_B_P2P}")"
PID_C="$(start_node "${DATA_C}" "${NODE_C_RPC}" "${NODE_C_P2P}" "${LOG_C}" "127.0.0.1:${NODE_B_P2P}")"
# B is the node under test: pin it to A and C so a transient drop reconnects to
# them rather than falling back to public peer discovery.
PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}" "127.0.0.1:${NODE_A_P2P},127.0.0.1:${NODE_C_P2P}")"

wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
wait_rpc "${NODE_C_RPC}" "${DATA_C}" || fail "Node C RPC did not come up"
pass "All regtest nodes are up"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_C_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 2 ]]" \
    "Node B never established both peer connections"
pass "Node B connected to nodes A and C"

rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.createhd" "[\"parallel-download\"]" >/dev/null 2>&1 || true
ADDR_A="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.getnewaddress" '[]' | jq -r '.result.address // .result // empty')"
[[ -n "${ADDR_A}" ]] || fail "Failed to obtain mining address on node A"

MINE_RESULT="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "generatetoaddress" "[1,\"${ADDR_A}\"]")"
jq -e '.error == null' <<<"${MINE_RESULT}" >/dev/null || fail "Node A failed to mine a block: ${MINE_RESULT}"
pass "Node A mined one block"

wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getblockcount\" '[]' | jq -r '.result // -1') -ge 1 ]]" \
    "Node B did not download the mined block"
pass "Node B downloaded the announced block"

if grep -Fq "[Relay] OnGetData received from" "${LOG_A}" 2>/dev/null; then
    wait_log_contains "${LOG_A}" "[BlockRelayManager::HandleGetData] Block SENT to" \
        "Node A received GETDATA but never served the announced block back to node B"
    if grep -Fq "[Relay] OnGetData received from" "${LOG_C}" 2>/dev/null; then
        fail "Node C unexpectedly received GETDATA despite never announcing the block"
    fi
    pass "Parallel block download requested data from the announcing peer only"
else
    wait_log_contains "${LOG_A}" "[BlockRelay] Broadcast cmpctblock to" \
        "Node A neither received GETDATA nor broadcast a compact block"
    wait_log_contains "${LOG_B}" "cmd='cmpctblock'" \
        "Node B did not receive a compact block announcement from the announcer"
    pass "Announcing peer delivered the block directly via compact-block relay"
fi
