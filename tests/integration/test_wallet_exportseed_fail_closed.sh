#!/usr/bin/env bash
# Issue #518: the legacy HDWallet sidecar is not the active WalletManager
# identity. Mnemonic export must fail closed rather than return recovery
# material that cannot reproduce the active wallet.
set -euo pipefail

DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
DATA_DIR="$(mktemp -d -t dinero_exportseed_XXXXXX)"
LOG_FILE="${DATA_DIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0

stop_node() {
    if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
        kill "${PID}" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "${PID}" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "${PID}" 2>/dev/null || true
    fi
    PID=""
}

fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    stop_node
    if [[ -f "${LOG_FILE}" ]]; then
        printf -- '--- daemon log tail ---\n' >&2
        tail -120 "${LOG_FILE}" >&2 || true
    fi
    exit 1
}

cleanup() {
    stop_node
    [[ "${KEEP_ON_FAIL}" -eq 0 ]] && rm -rf "${DATA_DIR}" || true
}
trap cleanup EXIT

for tool in curl jq lsof; do
    command -v "${tool}" >/dev/null || fail "${tool} is required"
done
[[ -x "${DINEROD}" ]] || fail "dinerod not executable at ${DINEROD}"

for _ in $(seq 1 50); do
    candidate=$((36000 + RANDOM % 11000))
    if ! lsof -nP -iTCP:"${candidate}" -sTCP:LISTEN >/dev/null 2>&1 \
       && ! lsof -nP -iTCP:"$((candidate + 1))" -sTCP:LISTEN >/dev/null 2>&1 \
       && ! lsof -nP -iTCP:"$((candidate + 2))" -sTCP:LISTEN >/dev/null 2>&1; then
        RPC_PORT="${candidate}"
        P2P_PORT="$((candidate + 1))"
        WALLET_PORT="$((candidate + 2))"
        break
    fi
done
[[ -n "${RPC_PORT:-}" ]] || fail "no collision-free port range available"

cookie_file() {
    [[ -f "${DATA_DIR}/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/.cookie"; return; }
    [[ -f "${DATA_DIR}/regtest/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/regtest/.cookie"; return; }
    return 1
}

rpc_raw() {
    local method="$1" cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    jq -nc --arg method "${method}" \
        '{jsonrpc:"2.0",id:1,method:$method,params:[]}' |
        curl -fsS --max-time 20 --user "${cookie}" \
            -H 'Content-Type: application/json' --data-binary @- \
            "http://127.0.0.1:${RPC_PORT}/"
}

"${DINEROD}" --regtest --datadir="${DATA_DIR}" \
    --rpcport="${RPC_PORT}" --port="${P2P_PORT}" \
    --wallet-socket-port="${WALLET_PORT}" --listen=0 \
    >"${LOG_FILE}" 2>&1 &
PID=$!

for _ in $(seq 1 100); do
    if ! kill -0 "${PID}" 2>/dev/null; then
        fail "daemon exited before RPC became ready"
    fi
    if rpc_raw getblockcount | jq -e '.error == null' >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done
rpc_raw getblockcount | jq -e '.error == null' >/dev/null 2>&1 \
    || fail "daemon RPC did not become ready"

ADDRESSES="$(rpc_raw wallet.listaddresses)"
jq -e '.error == null and (.result | length) >= 1' <<<"${ADDRESSES}" >/dev/null \
    || fail "fresh active wallet has no addresses: ${ADDRESSES}"

for method in wallet.exportseed wallet.exportmnemonic; do
    response="$(rpc_raw "${method}")"
    jq -e '
        .error != null and
        ((.result // {}) | has("mnemonic") | not) and
        ((.error.message // .error // "") | contains("Mnemonic export is unavailable"))
    ' <<<"${response}" >/dev/null \
        || fail "${method} did not fail closed without mnemonic material: ${response}"
    printf '[PASS] %s fails closed for runtime wallets\n' "${method}"
done

printf 'ALL WALLET EXPORT-SEED FAIL-CLOSED ASSERTIONS PASSED\n'
