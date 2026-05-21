#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
BASE_PORT="${BASE_PORT:-35720}"
RPC_PORT="${BASE_PORT}"
P2P_PORT=$((BASE_PORT + 100))
DATADIR="/tmp/dinero_testnet_cold_start_$$"
LOGFILE="${DATADIR}.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOGFILE}" ]] && { printf -- '--- daemon log tail ---\n' >&2; tail -120 "${LOGFILE}" >&2 || true; }
    exit 1
}

cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATADIR}" "${LOGFILE}"
    fi
}
trap cleanup EXIT

rpc_call() {
    local method="$1"
    local cookie_path="${DATADIR}/.cookie"
    [[ -f "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":[]}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

mkdir -p "${DATADIR}"
"${DINEROD}" \
    --testnet \
    --datadir="${DATADIR}" \
    --rpcport="${RPC_PORT}" \
    --p2pport="${P2P_PORT}" \
    --p2p.offline=1 \
    >"${LOGFILE}" 2>&1 &
PID="$!"

for _ in $(seq 1 60); do
    NETINFO="$(rpc_call getnetworkinfo 2>/dev/null || true)"
    if jq -e '.result.network == "testnet" and .result.listen == false and .result.networkactive == false' \
        <<<"${NETINFO}" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "${PID}" 2>/dev/null; then
        fail "testnet daemon exited before becoming ready"
    fi
    sleep 1
done

if ! jq -e '.result.network == "testnet" and .result.listen == false and .result.networkactive == false' \
    <<<"${NETINFO}" >/dev/null 2>&1; then
    fail "testnet daemon did not become ready"
fi
pass "testnet daemon cold-started and RPC reports isolated testnet offline mode"

if grep -q "GENESIS MISMATCH\\|genesis hash mismatch" "${LOGFILE}"; then
    fail "testnet startup logged a genesis mismatch"
fi
pass "testnet startup did not log a genesis mismatch"

grep -q "Network: testnet" "${LOGFILE}" || fail "genesis init did not run under testnet params"
grep -q "Genesis invariant verified: DB matches code" "${LOGFILE}" ||
    fail "testnet genesis invariant was not verified"
pass "testnet genesis identity is internally consistent"
