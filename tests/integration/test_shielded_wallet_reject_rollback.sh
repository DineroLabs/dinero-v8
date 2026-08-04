#!/usr/bin/env bash
# Issue #321: every shielded-wallet mutation made before canonical mempool
# ingress must be undone when ingress rejects the transaction. Exercises the
# live RPC paths for self-shield, unshield, single transfer, multi transfer,
# and addressed transfer through a regtest-only forced-rejection point.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="$(mktemp -d /tmp/dinero-wallet-rollback-321.XXXXXX)"
LOG_FILE="${DATA_DIR}/daemon.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }

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
    [[ -f "${LOG_FILE}" ]] && {
        printf -- '--- daemon log tail ---\n' >&2
        tail -160 "${LOG_FILE}" >&2 || true
    }
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
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

pick_ports() {
    local candidate
    for _ in $(seq 1 50); do
        candidate=$((36000 + RANDOM % 11000))
        if ! lsof -nP -iTCP:"${candidate}" -sTCP:LISTEN >/dev/null 2>&1 \
           && ! lsof -nP -iTCP:"$((candidate + 1))" -sTCP:LISTEN >/dev/null 2>&1 \
           && ! lsof -nP -iTCP:"$((candidate + 2))" -sTCP:LISTEN >/dev/null 2>&1; then
            RPC_PORT="${candidate}"
            P2P_PORT="$((candidate + 1))"
            WALLET_PORT="$((candidate + 2))"
            return 0
        fi
    done
    fail "no collision-free port range available"
}

cookie_file() {
    [[ -f "${DATA_DIR}/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/.cookie"; return; }
    [[ -f "${DATA_DIR}/regtest/.cookie" ]] && { printf '%s\n' "${DATA_DIR}/regtest/.cookie"; return; }
    return 1
}

rpc_raw() {
    local cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    curl -s --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":${2:-[]}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_result() {
    local response
    response="$(rpc_raw "$1" "${2:-[]}")" || fail "$1 transport failure"
    jq -e '.error == null and has("result")' <<<"${response}" >/dev/null 2>&1 \
        || fail "$1 failed: ${response}"
    jq -c '.result' <<<"${response}"
}

wait_rpc() {
    for _ in $(seq 1 100); do
        if [[ -n "${PID}" ]] && ! kill -0 "${PID}" 2>/dev/null; then return 1; fi
        if rpc_raw getblockcount '[]' | jq -e '.error == null' >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

start_node() {
    local reject="${1:-0}"
    : >"${LOG_FILE}"
    DINERO_TEST_REJECT_SHIELDED_WALLET_SUBMIT="${reject}" \
        "${DINEROD}" --regtest --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" --listen=0 --utreexo=1 \
        --p2p.offline=1 >"${LOG_FILE}" 2>&1 &
    PID=$!
    wait_rpc || fail "daemon did not reach RPC readiness"
}

wallet_state() {
    local notes balance
    notes="$(rpc_result wallet.listshielded '[]')"
    balance="$(rpc_result wallet.shieldedbalance '[]')"
    jq -cn --argjson notes "${notes}" --argjson balance "${balance}" \
        '{notes:$notes,balance:$balance}' | jq -S -c .
}

assert_rejected_without_mutation() {
    local label="$1" method="$2" params="$3" before response after mempool
    before="$(wallet_state)"
    response="$(rpc_raw "${method}" "${params}")"
    jq -e '.. | strings | select(. == "mempool_rejected")' <<<"${response}" >/dev/null \
        || fail "${label}: expected mempool_rejected: ${response}"
    jq -e '.. | strings | select(. == "regtest-only forced shielded wallet rejection")' \
        <<<"${response}" >/dev/null \
        || fail "${label}: forced-rejection hook did not fire: ${response}"
    jq -e '.. | strings | select(. == "complete")' <<<"${response}" >/dev/null \
        || fail "${label}: wallet rollback did not report complete: ${response}"
    after="$(wallet_state)"
    [[ "${after}" == "${before}" ]] \
        || fail "${label}: wallet state changed across rejection\nbefore=${before}\nafter=${after}"
    mempool="$(rpc_result getrawmempool '[]')"
    [[ "$(jq 'length' <<<"${mempool}")" -eq 0 ]] \
        || fail "${label}: rejected transaction entered mempool: ${mempool}"
    pass "${label}: rejection left inputs spendable and no phantom outputs"
}

pick_ports
start_node 0

MINER_ADDR="$(rpc_result wallet.getnewaddress '["taproot","rollback-321"]' | jq -r '.address // .')"
[[ -n "${MINER_ADDR}" && "${MINER_ADDR}" != "null" ]] || fail "empty miner address"
# At height 101 only two regtest coinbases are mature; seed enough mature
# transparent inputs for three independent shield transactions in one mempool.
rpc_result generatetoaddress "[105,\"${MINER_ADDR}\"]" >/dev/null

info "seeding three confirmed shielded notes"
rpc_result wallet.shield '[1.0]' >/dev/null
rpc_result wallet.shield '[0.5]' >/dev/null
rpc_result wallet.shield '[0.5]' >/dev/null
rpc_result generatetoaddress "[1,\"${MINER_ADDR}\"]" >/dev/null

RECIPIENT="$(rpc_result wallet.getshieldedaddress '{"account":1,"j":0}' | jq -r '.address')"
BASE_STATE="$(wallet_state)"
[[ "$(jq '.notes.count' <<<"${BASE_STATE}")" -eq 3 ]] \
    || fail "expected exactly three seeded notes: ${BASE_STATE}"

stop_node
start_node 1
[[ "$(wallet_state)" == "${BASE_STATE}" ]] || fail "state changed on forced-reject restart"

assert_rejected_without_mutation "single self-transfer" wallet.transfer '{}'
assert_rejected_without_mutation "multi self-transfer" wallet.transfer '{"amount_una":70000000}'
assert_rejected_without_mutation "addressed transfer" wallet.transfer \
    "{\"amount_una\":70000000,\"address\":\"${RECIPIENT}\"}"
assert_rejected_without_mutation "unshield" wallet.unshield '[1.0]'
assert_rejected_without_mutation "self-shield" wallet.shield '[0.25]'

stop_node
start_node 0
[[ "$(wallet_state)" == "${BASE_STATE}" ]] \
    || fail "rolled-back state changed after a clean restart"

SUCCESS="$(rpc_result wallet.transfer '{}')"
jq -e '.status == "transferred"' <<<"${SUCCESS}" >/dev/null \
    || fail "note was not spendable after rollback: ${SUCCESS}"
pass "rolled-back note remains spendable after clean restart"

echo "=== SUCCESS: shielded wallet rejection rollback (#321) ==="
