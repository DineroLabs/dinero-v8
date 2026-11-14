#!/usr/bin/env bash
# An isolated daemon supplies real named-wallet mined history to the Qt
# Overview panel. This fixture never discovers or changes a user datadir.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=../../tests/integration/helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
DINEROD="${DINERO_QT_REWARDS_DINEROD:-${1:?dinerod binary required}}"
QTTEST="${2:?Qt Overview test binary required}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dinero-qt-rewards.XXXXXX")"
DATADIR="${WORK}/node"
LOG="${WORK}/dinerod.log"
PID=""

cleanup() {
    local rc=$?
    trap - EXIT
    set +e
    if [[ -n "${PID}" ]]; then
        dinero_stop_process "${PID}" "isolated rewards daemon" 30 5 || rc=1
    fi
    if (( rc == 0 )); then
        rm -rf -- "${WORK}"
    else
        printf '[FAIL] isolated rewards fixture retained at %s\n' "${WORK}" >&2
        tail -80 "${LOG}" >&2 || true
    fi
    exit "${rc}"
}
trap cleanup EXIT

command -v curl >/dev/null
command -v jq >/dev/null
[[ -x "${DINEROD}" && -x "${QTTEST}" ]]
mkdir -m 700 "${DATADIR}"
printf 'qt-mining-rewards-regtest-v1\n' > "${DATADIR}/owner.marker"
read -r RPC_PORT P2P_PORT WALLET_PORT < <(dinero_allocate_port_triplet)

"${DINEROD}" --regtest --datadir="${DATADIR}" \
    --rpcport="${RPC_PORT}" --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
    --p2p.offline=1 --listen=0 --consensus-state-commitment-height=4294967295 \
    >"${LOG}" 2>&1 &
PID=$!

rpc() {
    local method="$1" params="$2" cookie response
    cookie="$(tr -d '\n' < "${COOKIE_PATH}")"
    response="$(curl -fsS --max-time 30 --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${RPC_PORT}/")"
    jq -e '.error == null or .error == false' <<<"${response}" >/dev/null || {
        printf '[FAIL] %s returned an RPC error\n' "${method}" >&2
        return 1
    }
    printf '%s\n' "${response}"
}

ready=0
for _ in $(seq 1 90); do
    COOKIE_PATH=""
    if [[ -f "${DATADIR}/.cookie" ]]; then COOKIE_PATH="${DATADIR}/.cookie";
    elif [[ -f "${DATADIR}/regtest/.cookie" ]]; then COOKIE_PATH="${DATADIR}/regtest/.cookie"; fi
    if [[ -n "${COOKIE_PATH}" ]] &&
        rpc getblockcount '[]' >/dev/null 2>&1; then ready=1; break; fi
    sleep 1
done
(( ready == 1 )) || { printf '[FAIL] isolated daemon did not start\n' >&2; exit 1; }

first="$(rpc wallet.createhd '["reward-first"]' | jq -r '.result.first_address')"
[[ "${first}" == din1* ]]
rpc generatetoaddress "[2,\"${first}\"]" >/dev/null
second="$(rpc wallet.createhd '["reward-second"]' | jq -r '.result.first_address')"
[[ "${second}" == din1* && "${second}" != "${first}" ]]
rpc generatetoaddress "[1,\"${second}\"]" >/dev/null
rpc wallet.open '["reward-first"]' >/dev/null

first_count="$(rpc wallet.listtransactions '{"count":1600,"type":"mined"}' | jq '.result | length')"
[[ "${first_count}" == 2 ]] || { printf '[FAIL] first wallet has %s mined rows\n' "${first_count}" >&2; exit 1; }
rpc wallet.open '["reward-second"]' >/dev/null
second_count="$(rpc wallet.listtransactions '{"count":1600,"type":"mined"}' | jq '.result | length')"
[[ "${second_count}" == 1 ]] || { printf '[FAIL] second wallet has %s mined rows\n' "${second_count}" >&2; exit 1; }
rpc wallet.open '["reward-first"]' >/dev/null

export DINERO_QT_REWARDS_REGTEST=1
export DINERO_QT_REWARDS_DATADIR="${DATADIR}"
export DINERO_RPC_URL="http://127.0.0.1:${RPC_PORT}/"
export QT_QPA_PLATFORM=offscreen
"${QTTEST}" realDaemonMinedHistoryFeedsRewardsPanel
