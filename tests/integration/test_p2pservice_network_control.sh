#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
if [[ ! -x "${DINEROD}" && -x "${ROOT_DIR}/build-release/dinerod" ]]; then
    DINEROD="${ROOT_DIR}/build-release/dinerod"
fi
BASE_PORT="${BASE_PORT:-35600}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
DATA_A="/tmp/dinero_p2psvc_a_$$"
DATA_B="/tmp/dinero_p2psvc_b_$$"
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
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -80 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -80 "${LOG_B}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID_A}" ]] && kill "${PID_A}" 2>/dev/null || true
    [[ -n "${PID_B}" ]] && kill "${PID_B}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_A}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_B}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_A}" "${DATA_B}" "${LOG_A}" "${LOG_B}"
    fi
}
trap cleanup EXIT

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

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

PID_A="$(start_node "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}")"
PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"

wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
pass "Both regtest nodes are up"

NETINFO_BOOT="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getnetworkinfo" '[]')"
jq -e --argjson port "${NODE_B_P2P}" \
    '.result.networkactive == true and .result.listen == true and .result.listen_port == $port and (.result.port_mapping | type == "object")' \
    <<<"${NETINFO_BOOT}" >/dev/null \
    || fail "getnetworkinfo did not expose listen/port-mapping status: ${NETINFO_BOOT}"
pass "getnetworkinfo exposes listen and port-mapping status"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"add\"]" >/dev/null
ADDED_JSON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getaddednodeinfo" '[]')"
jq -e --arg endpoint "127.0.0.1:${NODE_A_P2P}" '.result[] | select(.addednode == $endpoint)' <<<"${ADDED_JSON}" >/dev/null \
    || fail "Runtime addnode entry missing from getaddednodeinfo: ${ADDED_JSON}"
pass "Runtime added node is surfaced via getaddednodeinfo"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B never connected to node A"
pass "One-shot connect works through P2PService path"
NETINFO_CONNECTED="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getnetworkinfo" '[]')"
jq -e '.result.connections >= 1 and .result.connections_out >= 1' <<<"${NETINFO_CONNECTED}" >/dev/null \
    || fail "getnetworkinfo did not expose outbound connection count: ${NETINFO_CONNECTED}"
pass "getnetworkinfo exposes directional peer counts"

SET_OFF="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "setnetworkactive" '[false]')"
jq -e '.result.applied == true and .result.networkactive == false and .result.requested_state == false' <<<"${SET_OFF}" >/dev/null \
    || fail "setnetworkactive(false) did not acknowledge request: ${SET_OFF}"
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // -1') -eq 0 ]]" \
    "Node B did not drop peers after setnetworkactive(false)"
pass "setnetworkactive(false) disconnects peers"

SET_ON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "setnetworkactive" '[true]')"
jq -e '.result.applied == true and .result.networkactive == true and .result.requested_state == true' <<<"${SET_ON}" >/dev/null \
    || fail "setnetworkactive(true) did not acknowledge request: ${SET_ON}"
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B did not reconnect to runtime added node after re-enabling network"
pass "setnetworkactive(true) reconnects through runtime seed list"

BAN_JSON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "setban" "[\"127.0.0.1\",\"add\",60,false]")"
jq -e '.result.success == true and .result.address == "127.0.0.1"' <<<"${BAN_JSON}" >/dev/null \
    || fail "setban add did not report success: ${BAN_JSON}"
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // -1') -eq 0 ]]" \
    "Node B did not disconnect the banned peer"
LIST_BANNED="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "listbanned" '[]')"
jq -e '.result[] | select(.address == "127.0.0.1" and .banned_until > .ban_created)' <<<"${LIST_BANNED}" >/dev/null \
    || fail "listbanned missing live P2P ban entry: ${LIST_BANNED}"
pass "setban add disconnects and listbanned exposes the ban"

UNBAN_JSON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "setban" "[\"127.0.0.1\",\"remove\"]")"
jq -e '.result.success == true and .result.address == "127.0.0.1"' <<<"${UNBAN_JSON}" >/dev/null \
    || fail "setban remove did not report success: ${UNBAN_JSON}"
rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B did not reconnect after removing the ban"
pass "setban remove allows reconnection"

REMOVE_JSON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"remove\"]")"
jq -e '.result.success == true and .result.removed == true' <<<"${REMOVE_JSON}" >/dev/null \
    || fail "addnode remove did not report success: ${REMOVE_JSON}"
ADDED_AFTER_REMOVE="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getaddednodeinfo" '[]')"
jq -e --arg endpoint "127.0.0.1:${NODE_A_P2P}" 'all(.result[]?; .addednode != $endpoint)' <<<"${ADDED_AFTER_REMOVE}" >/dev/null \
    || fail "Removed node still appears in getaddednodeinfo: ${ADDED_AFTER_REMOVE}"
pass "Runtime addnode removal is reflected via P2PService"

echo "P2PSERVICE_NETWORK_CONTROL=PASS"
