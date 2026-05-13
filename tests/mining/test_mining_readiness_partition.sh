#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"
MINING_ADDRESS="din1pmvnrlwkk87phdekfs65gfxv69qgjcnupanyyzw894rwd8e76n66q6cey44"

BASE_PORT="${BASE_PORT:-32100}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))

DATA_A="/tmp/dinero_readiness_a_$$"
DATA_B="/tmp/dinero_readiness_b_$$"
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
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -40 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -40 "${LOG_B}" >&2 || true; }
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

wait_peer_count() {
    local rpc_port="$1"
    local datadir="$2"
    local expected="$3"
    for _ in $(seq 1 40); do
        local count
        count="$(rpc_call "${rpc_port}" "${datadir}" "getconnectioncount" '[]' | jq -r '.result // -1')"
        if [[ "${count}" == "${expected}" ]]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_readiness() {
    local rpc_port="$1"
    local datadir="$2"
    local expected_ready="$3"
    local expected_reason="$4"
    local last_response=""
    for _ in $(seq 1 40); do
        local response ready reason
        response="$(rpc_call "${rpc_port}" "${datadir}" "mining.info" '[]')"
        last_response="${response}"
        ready="$(jq -r 'if (.result.mining_readiness | has("ready")) then (.result.mining_readiness.ready | tostring) else "missing" end' <<<"${response}")"
        reason="$(jq -r '.result.mining_readiness.reason // "missing"' <<<"${response}")"
        if [[ "${ready}" == "${expected_ready}" && "${reason}" == "${expected_reason}" ]]; then
            return 0
        fi
        sleep 1
    done
    printf '[INFO] Last mining.info response from rpc=%s: %s\n' "${rpc_port}" "${last_response}" >&2
    return 1
}

start_node() {
    local name="$1"
    local datadir="$2"
    local rpc_port="$3"
    local p2p_port="$4"
    local logfile="$5"

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
        --mining.readiness.max_tip_lag=8 \
        --mining.readiness.max_tip_ahead=1 \
        >"${logfile}" 2>&1 &

    local pid=$!
    info "Started ${name} pid=${pid} rpc=${rpc_port} p2p=${p2p_port}" >&2
    printf '%s\n' "${pid}"
}

require_tools

PID_A="$(start_node A "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}")"
PID_B="$(start_node B "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"

wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
pass "Both regtest nodes are up"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_peer_count "${NODE_B_RPC}" "${DATA_B}" "1" || fail "Node B never connected"
pass "Node B connected to peer A"

wait_readiness "${NODE_B_RPC}" "${DATA_B}" "true" "ready" || fail "Node B did not become mining-ready"
pass "Connected miner-side node reports mining_readiness.ready=true"

peer_b_addr="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "getpeerinfo" '[]' | jq -r '.result[0].addr')"
[[ -n "${peer_b_addr}" && "${peer_b_addr}" != "null" ]] || fail "Node B peer addr unavailable"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "disconnectnode" "[\"${peer_b_addr}\"]" >/dev/null

wait_peer_count "${NODE_B_RPC}" "${DATA_B}" "0" || fail "Node B did not disconnect"
pass "Partition created"

wait_readiness "${NODE_B_RPC}" "${DATA_B}" "false" "insufficient_peers" || fail "Isolated node B did not flip to paused readiness"
pass "Isolated node reports mining_readiness.ready=false reason=insufficient_peers"

job_response="$(rpc_call "${NODE_B_RPC}" "${DATA_B}" "mining.getjob" "{\"address\":\"${MINING_ADDRESS}\"}")"
job_code="$(jq -r '.result.code // .result.error.code // .error.code // "missing"' <<<"${job_response}")"
job_reason="$(jq -r '.result.reason // .result.mining_safety.reason // "missing"' <<<"${job_response}")"
[[ "${job_code}" == "mining-safety-gate" ]] || fail "Expected mining.getjob safety gate, got code=${job_code} response=${job_response}"
[[ "${job_reason}" == "insufficient_peers" ]] || fail "Expected insufficient_peers, got reason=${job_reason} response=${job_response}"
pass "mining.getjob is rejected on isolated node by the safety gate"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null

wait_peer_count "${NODE_B_RPC}" "${DATA_B}" "1" || fail "Node B did not reconnect"
wait_readiness "${NODE_B_RPC}" "${DATA_B}" "true" "ready" || fail "Node B did not recover readiness after reconnect"
pass "Readiness recovered after reconnect"

info "Node B mining.info after recovery:"
rpc_call "${NODE_B_RPC}" "${DATA_B}" "mining.info" '[]' | jq '.result.mining_readiness'

pass "Two-node partition readiness test passed"
