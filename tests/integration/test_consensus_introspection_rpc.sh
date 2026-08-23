#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
BASE_PORT="${BASE_PORT:-37240}"
RPC_PORT="${BASE_PORT}"
P2P_PORT="$((BASE_PORT + 1))"
DATA_DIR="$(mktemp -d -t dinero_consensus_rpc_XXXXXX)"
LOG_FILE="${DATA_DIR}/dinerod.log"
PID=""
KEEP_ON_FAIL=0

fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    tail -120 "${LOG_FILE}" >&2 2>/dev/null || true
    exit 1
}

cleanup() {
    if [[ -n "${PID}" ]]; then
        kill "${PID}" 2>/dev/null || true
        wait "${PID}" 2>/dev/null || true
    fi
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}"
    else
        printf '[INFO] artifacts retained at %s\n' "${DATA_DIR}" >&2
    fi
}
trap cleanup EXIT

cookie_file() {
    if [[ -f "${DATA_DIR}/.cookie" ]]; then
        printf '%s\n' "${DATA_DIR}/.cookie"
    elif [[ -f "${DATA_DIR}/regtest/.cookie" ]]; then
        printf '%s\n' "${DATA_DIR}/regtest/.cookie"
    else
        return 1
    fi
}

rpc_call() {
    local method="$1"
    local cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    curl -sS --max-time 10 --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":[]}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if rpc_call getblockcount | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

"${DINEROD}" \
    --regtest \
    --datadir="${DATA_DIR}" \
    --rpc --server --listen=0 --p2p.offline=1 \
    --rpcport="${RPC_PORT}" --port="${P2P_PORT}" \
    >"${LOG_FILE}" 2>&1 &
PID="$!"

wait_rpc || fail "RPC server did not become ready"

for method in blockchain.getchaintips getchaintips; do
    response="$(rpc_call "${method}")"
    jq -e '.error == null and (.result | type == "array") and (.result | length >= 1)' \
        <<<"${response}" >/dev/null || fail "${method} unavailable or malformed: ${response}"
done

for method in blockchain.getchainwork getchainwork; do
    response="$(rpc_call "${method}")"
    jq -e '.error == null and (.result | type == "string") and (.result | length >= 1)' \
        <<<"${response}" >/dev/null || fail "${method} unavailable or malformed: ${response}"
done

for method in blockchain.getreorgstatus getreorgstatus; do
    response="$(rpc_call "${method}")"
    jq -e '.error == null and (.result | type == "object") and
        (.result.total | type == "number") and
        (.result.safe_mode.active | type == "boolean")' \
        <<<"${response}" >/dev/null || fail "${method} unavailable or malformed: ${response}"
done

for method in daemon.getstatus getdaemonstatus; do
    response="$(rpc_call "${method}")"
    jq -e '.error == null and (.result.sync | type == "object") and
        (.result.sync.active_tip.hash | type == "string") and
        (.result.sync.best_header.hash | type == "string") and
        (.result.sync.convergence == "unknown" or
         .result.sync.convergence == "mismatch" or
         .result.sync.convergence == "converged") and
        (.result.sync.converged | type == "boolean")' \
        <<<"${response}" >/dev/null || fail "${method} lacks authoritative sync state: ${response}"
done

printf 'CONSENSUS_INTROSPECTION_RPC=PASS\n'
