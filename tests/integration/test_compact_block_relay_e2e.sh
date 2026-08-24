#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${ROOT_DIR}/build/dinerod"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
BASE_PORT="${BASE_PORT:-35500}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
DATA_A="/tmp/dinero_compact_a_$$"
DATA_B="/tmp/dinero_compact_b_$$"
LOG_A="${DATA_A}.log"
LOG_B="${DATA_B}.log"
PID_A=""
PID_B=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -120 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -120 "${LOG_B}" >&2 || true; }
    exit 1
}
cleanup() {
    local test_rc=$?
    local cleanup_rc=0
    local final_rc=0
    trap - EXIT
    set +e
    dinero_stop_process "${PID_A}" "compact-relay node A" || cleanup_rc=1
    dinero_stop_process "${PID_B}" "compact-relay node B" || cleanup_rc=1
    dinero_stop_datadir_processes "${DATA_A}" || cleanup_rc=1
    dinero_stop_datadir_processes "${DATA_B}" || cleanup_rc=1
    if [[ "${KEEP_ON_FAIL}" != "1" && "${cleanup_rc}" -eq 0 ]]; then
        rm -rf "${DATA_A}" "${DATA_B}" "${LOG_A}" "${LOG_B}" || cleanup_rc=1
    fi
    dinero_cleanup_result "${test_rc}" "${cleanup_rc}" || final_rc=$?
    exit "${final_rc}"
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
    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

require_tools

PID_A="$(start_node "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}")"
PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"

wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
pass "Both regtest nodes are up"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B did not connect to node A"
pass "Node B connected to node A"

wait_log_contains "${LOG_A}" "Received sendcmpct from" "Node A never received sendcmpct negotiation"
wait_log_contains "${LOG_B}" "Received sendcmpct from" "Node B never received sendcmpct negotiation"
pass "Compact block negotiation is live on both peers"

rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.createhd" "[\"compact-a\"]" >/dev/null 2>&1 || true
rpc_call "${NODE_B_RPC}" "${DATA_B}" "wallet.createhd" "[\"compact-b\"]" >/dev/null 2>&1 || true
ADDR_A="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.getnewaddress" '[]' | jq -r '.result.address // .result // empty')"
ADDR_B="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "wallet.getnewaddress" '[]' | jq -r '.result.address // .result // empty')"
[[ -n "${ADDR_A}" ]] || fail "Failed to obtain mining address on node A"
[[ -n "${ADDR_B}" ]] || fail "Failed to obtain recipient address on node B"

info "Mining maturity on node A"
MINE_RESULT="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "generatetoaddress" "[110,\"${ADDR_A}\"]")"
jq -e '.error == null' <<<"${MINE_RESULT}" >/dev/null || fail "Node A failed to mine maturity blocks: ${MINE_RESULT}"
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getblockcount\" '[]' | jq -r '.result // -1') -ge 110 ]]" \
    "Node B did not sync maturity blocks"
wait_log_contains "${LOG_B}" "cmd='cmpctblock'" "Node B never received cmpctblock during steady-state sync"
if grep -Fq "requesting via getblocktxn" "${LOG_B}" 2>/dev/null; then
    fail "Steady-state compact relay unexpectedly required getblocktxn"
fi
if grep -Fq "cmd='getblocktxn'" "${LOG_A}" 2>/dev/null; then
    fail "Steady-state compact relay unexpectedly triggered getblocktxn on node A"
fi
pass "Compact blocks relay directly when mempool reconstruction is trivial"

PEER_B_ADDR="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getpeerinfo" '[]' | jq -r '.result[0].addr // empty')"
[[ -n "${PEER_B_ADDR}" ]] || fail "Failed to resolve node A peer address on node B"
rpc_call "${NODE_B_RPC}" "${DATA_B}" "disconnectnode" "[\"${PEER_B_ADDR}\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // -1') -eq 0 ]]" \
    "Node B did not disconnect from node A"
pass "Node B disconnected to create a mempool split"

SEND_RESULT="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.sendtoaddress" "[\"${ADDR_B}\",1.0,\"\",\"\",true]")"
jq -e '.error == null' <<<"${SEND_RESULT}" >/dev/null || fail "Node A failed to create hidden mempool transaction: ${SEND_RESULT}"

wait_condition "[[ \$(rpc_call \"${NODE_A_RPC}\" \"${DATA_A}\" \"getmempoolinfo\" '[]' | jq -r '.result.size // .result.tx_count // 0') -ge 1 ]]" \
    "Node A never recorded the hidden transaction in mempool"
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getmempoolinfo\" '[]' | jq -r '.result.size // .result.tx_count // 0') -eq 0 ]]" \
    "Node B unexpectedly learned the hidden transaction before reconnect"
pass "Hidden transaction exists only in node A mempool"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B did not reconnect to node A"
pass "Node B reconnected before the compact-relayed block"

BASE_GETBLOCKTXN_A="$(grep -Fc "cmd='getblocktxn'" "${LOG_A}" 2>/dev/null || true)"
BASE_BLOCKTXN_B="$(grep -Fc "cmd='blocktxn'" "${LOG_B}" 2>/dev/null || true)"

BLOCK_RESULT="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "generatetoaddress" "[1,\"${ADDR_A}\"]")"
jq -e '.error == null' <<<"${BLOCK_RESULT}" >/dev/null || fail "Node A failed to mine compact round-trip block: ${BLOCK_RESULT}"

wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getblockcount\" '[]' | jq -r '.result // -1') -ge 111 ]]" \
    "Node B did not accept the compact-relayed block with missing transaction"
wait_condition "[[ \$(grep -Fc \"cmd='getblocktxn'\" \"${LOG_A}\" 2>/dev/null || true) -gt ${BASE_GETBLOCKTXN_A} ]]" \
    "Node A never received a getblocktxn request for the missing transaction"
wait_condition "[[ \$(grep -Fc \"cmd='blocktxn'\" \"${LOG_B}\" 2>/dev/null || true) -gt ${BASE_BLOCKTXN_B} ]]" \
    "Node B never received the blocktxn response"
pass "Compact block fallback round trip succeeded end-to-end"

STATS_B="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "compactblocks.stats" '[]')"
jq -e '.error == null' <<<"${STATS_B}" >/dev/null || fail "compactblocks.stats failed on node B"
jq -e '.result.blocks_processed >= 1' <<<"${STATS_B}" >/dev/null || fail "compactblocks.stats did not record processed compact blocks"
pass "compactblocks.stats reflects live compact relay activity"
