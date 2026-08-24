#!/usr/bin/env bash
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
MINING_ADDRESS="din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44"
BASE_PORT="${BASE_PORT:-34100}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
DATA_A="/tmp/dinero_peer_meta_a_$$"
DATA_B="/tmp/dinero_peer_meta_b_$$"
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
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -60 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -60 "${LOG_B}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID_A}" ]] && kill "${PID_A}" 2>/dev/null || true
    [[ -n "${PID_B}" ]] && kill "${PID_B}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_A}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_B}" 2>/dev/null || true
    # Wait for the daemons to actually exit before deleting their datadirs:
    # rm -rf racing a still-writing daemon intermittently fails with
    # "Directory not empty", turning an all-assertions-passed run into a
    # ctest failure (the only failure mode this test has shown).
    for _i in $(seq 1 20); do
        pgrep -f "dinerod.*(${DATA_A}|${DATA_B})" >/dev/null 2>&1 || break
        sleep 0.5
    done
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_A}" "${DATA_B}" "${LOG_A}" "${LOG_B}" 2>/dev/null || true
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
        --allow-external-mining=1 \
        --mine-during-ibd=1 \
        --mining.readiness.allow_isolated=false \
        --mining.readiness.min_peers=1 \
        --mining.readiness.require_recent_peer_activity=false \
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
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" "Node B never connected to node A"
pass "Node B connected to node A"

ADDR_A="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.createhd" '["peer-metadata"]' | jq -r '.result.first_address')"
[[ -n "${ADDR_A}" && "${ADDR_A}" != "null" ]] || fail "Failed to create mining wallet on node A"

rpc_call "${NODE_A_RPC}" "${DATA_A}" "mining.setaddress" "[\"${ADDR_A}\"]" >/dev/null
rpc_call "${NODE_A_RPC}" "${DATA_A}" "mining.start" '[1]' >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_A_RPC}\" \"${DATA_A}\" \"getblockcount\" '[]' | jq -r '.result // 0') -ge 3 ]]" "Node A failed to mine 3 blocks"
rpc_call "${NODE_A_RPC}" "${DATA_A}" "mining.stop" '[]' >/dev/null
pass "Node A mined blocks"

wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getblockcount\" '[]' | jq -r '.result // 0') -ge 3 ]]" "Node B did not sync mined blocks"
pass "Node B synced mined blocks"

PEER_JSON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getpeerinfo" '[]' | jq -c '.result[0]')"
[[ -n "${PEER_JSON}" && "${PEER_JSON}" != "null" ]] || fail "Node B getpeerinfo returned no peer entries"

jq -e '.startingheight >= 0' <<<"${PEER_JSON}" >/dev/null || fail "startingheight missing from getpeerinfo: ${PEER_JSON}"
jq -e '.start_height == .startingheight' <<<"${PEER_JSON}" >/dev/null || fail "start_height alias mismatch: ${PEER_JSON}"
jq -e '.bestknownheight >= 3' <<<"${PEER_JSON}" >/dev/null || fail "bestknownheight did not advance: ${PEER_JSON}"
jq -e '.best_known_height == .bestknownheight' <<<"${PEER_JSON}" >/dev/null || fail "best_known_height alias mismatch: ${PEER_JSON}"
jq -e '.synced_headers >= 3' <<<"${PEER_JSON}" >/dev/null || fail "synced_headers did not advance: ${PEER_JSON}"
jq -e '.synced_blocks >= 0' <<<"${PEER_JSON}" >/dev/null || fail "synced_blocks missing: ${PEER_JSON}"
jq -e '.conntime > 0' <<<"${PEER_JSON}" >/dev/null || fail "conntime missing: ${PEER_JSON}"
jq -e '.connected_since == .conntime' <<<"${PEER_JSON}" >/dev/null || fail "connected_since alias mismatch: ${PEER_JSON}"
jq -e '.lastrecv > 0' <<<"${PEER_JSON}" >/dev/null || fail "lastrecv missing: ${PEER_JSON}"
jq -e '.last_message_at == .lastrecv' <<<"${PEER_JSON}" >/dev/null || fail "last_message_at alias mismatch: ${PEER_JSON}"
jq -e '.services | type == "string"' <<<"${PEER_JSON}" >/dev/null || fail "services missing: ${PEER_JSON}"
jq -e '.service_flags >= 0' <<<"${PEER_JSON}" >/dev/null || fail "service_flags missing: ${PEER_JSON}"
jq -e '.subver | contains("/dinerod:")' <<<"${PEER_JSON}" >/dev/null || fail "subver missing build user agent: ${PEER_JSON}"
jq -e '.subver | contains("0.6.0") | not' <<<"${PEER_JSON}" >/dev/null || fail "subver still hardcoded to 0.6.0: ${PEER_JSON}"
jq -e '.subver | contains("0.1.0") | not' <<<"${PEER_JSON}" >/dev/null || fail "subver still hardcoded to 0.1.0: ${PEER_JSON}"
pass "Peer metadata fields and user agent are live and non-stale"

READINESS_JSON="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "mining.info" '[]')"
READINESS_REASON="$(jq -r '.result.mining_readiness.reason // "missing"' <<<"${READINESS_JSON}")"
[[ "${READINESS_REASON}" != "ahead_of_network_view" ]] || fail "Readiness still paused due to peer-advertised height: ${READINESS_JSON}"
pass "Mining readiness is not driven by stale peer-advertised height"
