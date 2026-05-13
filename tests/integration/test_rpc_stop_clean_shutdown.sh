#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"
BASE_PORT="${BASE_PORT:-35900}"
RPC_PORT=$((BASE_PORT + 0))
P2P_PORT=$((BASE_PORT + 100))
DATA_DIR="/tmp/dinero_rpc_stop_shutdown_$$"
LOG_FILE="${DATA_DIR}.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -f "${LOG_FILE}" ]]; then
        printf -- '--- dinerod log tail ---\n' >&2
        tail -120 "${LOG_FILE}" >&2 || true
    fi
    exit 1
}

cleanup() {
    if [[ -n "${PID}" ]]; then
        kill "${PID}" 2>/dev/null || true
        wait "${PID}" 2>/dev/null || true
    fi
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_FILE}"
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
    local method="$1"
    local params_json="$2"
    local cookie_path
    cookie_path="$(cookie_file "${DATA_DIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

wait_rpc() {
    for _ in $(seq 1 60); do
        if rpc_call "getblockcount" '[]' | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

mkdir -p "${DATA_DIR}"
"${DINEROD}" \
    --regtest \
    --datadir="${DATA_DIR}" \
    --rpc \
    --server \
    --listen=0 \
    --rpcport="${RPC_PORT}" \
    --port="${P2P_PORT}" \
    >"${LOG_FILE}" 2>&1 &
PID="$!"

wait_rpc || fail "RPC server did not come up"
pass "RPC server came up"

STOP_JSON="$(rpc_call "stop" '[]')"
jq -e '.result.message == "Shutdown requested"' <<<"${STOP_JSON}" >/dev/null \
    || fail "stop RPC did not acknowledge shutdown: ${STOP_JSON}"
pass "stop RPC acknowledged shutdown"

for _ in $(seq 1 30); do
    if ! kill -0 "${PID}" 2>/dev/null; then
        break
    fi
    sleep 1
done

if kill -0 "${PID}" 2>/dev/null; then
    fail "dinerod did not exit after stop RPC"
fi

wait "${PID}" || fail "dinerod exited with non-zero status after stop RPC"
PID=""

if rg -n "libc\\+\\+abi: terminating|Abort trap: 6|std::terminate|SIGABRT" "${LOG_FILE}" >/dev/null; then
    fail "shutdown log still contains abort markers"
fi

pass "stop RPC shutdown exited cleanly without abort"
echo "RPC_STOP_CLEAN_SHUTDOWN=PASS"
