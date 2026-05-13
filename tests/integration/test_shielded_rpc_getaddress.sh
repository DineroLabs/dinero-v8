#!/usr/bin/env bash
# Phase 5 Wave 3c — wallet.getshieldedaddress regtest sanity.
#
# Asserts:
#   - Default params (account=0, j=0) returns rdins1… on regtest
#   - Different j produces a different address
#   - Different account produces a different address
#   - Returned d_hex is 22 hex chars, pk_d_hex is 64 hex chars
#   - Available pre-shield (no funds, no notes — pure derivation)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${ROOT_DIR}/build/dinerod"

DATA_DIR="/tmp/dinero_getaddr_$$"
LOG_FILE="${DATA_DIR}.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_FILE}" ]] && { printf -- '--- daemon log tail ---\n' >&2; tail -120 "${LOG_FILE}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_FILE}"
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null   || fail "jq is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then printf '%s\n' "${datadir}/.cookie"; return 0; fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then printf '%s\n' "${datadir}/regtest/.cookie"; return 0; fi
    return 1
}

rpc_call() {
    local method="$1"; local params_json="$2"
    local cookie_path; cookie_path="$(cookie_file "${DATA_DIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie; cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_result() {
    local method="$1"; local params_json="$2"
    local result; result="$(rpc_call "${method}" "${params_json}")"
    if echo "$result" | jq -e '.error != null and .error != false' >/dev/null 2>&1; then
        fail "${method} failed: ${result}"
    fi
    if echo "$result" | jq -e '.result.error // empty' >/dev/null 2>&1; then
        if echo "$result" | jq -e '.result.error' >/dev/null 2>&1; then
            local inner_err
            inner_err="$(jq -r '.result.error // ""' <<<"${result}")"
            if [[ -n "${inner_err}" && "${inner_err}" != "null" && "${inner_err}" != "false" ]]; then
                fail "${method} returned result.error=${inner_err}: ${result}"
            fi
        fi
    fi
    printf '%s\n' "${result}"
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if [[ -n "${PID}" ]] && ! kill -0 "${PID}" 2>/dev/null; then return 1; fi
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}

start_node() {
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" \
        --regtest --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 --utreexo=1 --p2p.offline=1 \
        >"${LOG_FILE}" 2>&1 &
    PID=$!
}

require_tools
RPC_PORT=$((40000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

start_node
wait_rpc || fail "daemon did not reach RPC readiness"

# Wallet just needs to exist; mining 1 block is enough to ensure the
# wallet manager initialized and the master seed is loaded.
MINER_RES="$(rpc_result "wallet.getnewaddress" '["taproot","getaddr-init"]')"
MINER_ADDR="$(jq -r '.result.address // .result // empty' <<<"${MINER_RES}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty"
rpc_result "generatetoaddress" "[1,\"${MINER_ADDR}\"]" >/dev/null

# ── Default params: account=0, j=0 ────────────────────────────────────
info "wallet.getshieldedaddress (default account=0, j=0)"
A_DEFAULT="$(rpc_result "wallet.getshieldedaddress" '[]')"
ADDR_A="$(jq -r '.result.address' <<<"${A_DEFAULT}")"
HRP_A="$(jq -r '.result.hrp' <<<"${A_DEFAULT}")"
D_HEX_A="$(jq -r '.result.d_hex' <<<"${A_DEFAULT}")"
PK_HEX_A="$(jq -r '.result.pk_d_hex' <<<"${A_DEFAULT}")"
ACCT_A="$(jq -r '.result.account' <<<"${A_DEFAULT}")"
J_A="$(jq -r '.result.j' <<<"${A_DEFAULT}")"
[[ -n "${ADDR_A}" && "${ADDR_A}" != "null" ]] || fail "missing address: ${A_DEFAULT}"
[[ "${HRP_A}" == "rdins" ]] || fail "expected hrp=rdins, got ${HRP_A}"
[[ "${ACCT_A}" == "0" ]] || fail "expected account=0, got ${ACCT_A}"
[[ "${J_A}" == "0" ]] || fail "expected j=0, got ${J_A}"
[[ "${ADDR_A}" =~ ^rdins1 ]] || fail "address must start with rdins1: ${ADDR_A}"
[[ "${#D_HEX_A}" == "22" ]] || fail "d_hex must be 22 chars (11 bytes), got ${#D_HEX_A}"
[[ "${#PK_HEX_A}" == "64" ]] || fail "pk_d_hex must be 64 chars (32 bytes), got ${#PK_HEX_A}"
pass "default address ${ADDR_A:0:26}… (j=0,acct=0,d=${D_HEX_A})"

# ── Different j must yield different address ──────────────────────────
B="$(rpc_result "wallet.getshieldedaddress" '{"j": 1}')"
ADDR_B="$(jq -r '.result.address' <<<"${B}")"
D_HEX_B="$(jq -r '.result.d_hex' <<<"${B}")"
[[ "${ADDR_B}" != "${ADDR_A}" ]] || fail "j=1 produced same address as j=0"
[[ "${D_HEX_B}" != "${D_HEX_A}" ]] || fail "j=1 produced same d as j=0"
pass "j=1 address differs (d=${D_HEX_B})"

# ── Different account must yield different address ────────────────────
C="$(rpc_result "wallet.getshieldedaddress" '{"account": 1, "j": 0}')"
ADDR_C="$(jq -r '.result.address' <<<"${C}")"
[[ "${ADDR_C}" != "${ADDR_A}" ]] || fail "account=1 j=0 same as account=0 j=0"
ACCT_C="$(jq -r '.result.account' <<<"${C}")"
[[ "${ACCT_C}" == "1" ]] || fail "expected account=1, got ${ACCT_C}"
pass "account=1 address differs"

# ── Idempotence: same params → same address ───────────────────────────
A_AGAIN="$(rpc_result "wallet.getshieldedaddress" '[]')"
ADDR_A2="$(jq -r '.result.address' <<<"${A_AGAIN}")"
[[ "${ADDR_A}" == "${ADDR_A2}" ]] || fail "non-deterministic: ${ADDR_A} vs ${ADDR_A2}"
pass "deterministic across repeated calls"

# ── Locked wallet policy: receive/view remains available ──────────────
rpc_result "wallet.encrypt" '["pw"]' >/dev/null
LOCKED_INFO="$(rpc_result "wallet.status" '[]')"
LOCKED_STATE="$(jq -r '.result.locked // .result.encrypted // empty' <<<"${LOCKED_INFO}")"
[[ "${LOCKED_STATE}" == "true" ]] || fail "wallet should be locked after wallet.encrypt: ${LOCKED_INFO}"

LOCKED_ADDR_RES="$(rpc_result "wallet.getshieldedaddress" '[]')"
LOCKED_ADDR="$(jq -r '.result.address' <<<"${LOCKED_ADDR_RES}")"
LOCKED_CACHED="$(jq -r '.result.cached // false' <<<"${LOCKED_ADDR_RES}")"
[[ "${LOCKED_ADDR}" == "${ADDR_A}" ]] || fail "locked wallet returned different cached address"
[[ "${LOCKED_CACHED}" == "true" ]] || fail "locked getshieldedaddress should use cached receive metadata"
pass "locked wallet still returns cached shielded receive address"

LOCKED_BAL="$(rpc_result "wallet.shieldedbalance" '[]')"
[[ "$(jq -r '.result.balance_una' <<<"${LOCKED_BAL}")" == "0" ]] || fail "locked balance query returned nonzero on empty wallet"
LOCKED_LIST="$(rpc_result "wallet.listshielded" '[]')"
[[ "$(jq -r '.result.count' <<<"${LOCKED_LIST}")" == "0" ]] || fail "locked listshielded expected no notes"
pass "locked wallet still exposes read-only shielded balance/list"

LOCKED_SHIELD="$(rpc_call "wallet.shield" '{"amount": 1.0, "fee_una": 10000}')"
LOCKED_SHIELD_ERR="$(jq -r '.result.error // empty' <<<"${LOCKED_SHIELD}")"
[[ "${LOCKED_SHIELD_ERR}" == "wallet_locked" ]] || fail "locked wallet.shield should reject spends: ${LOCKED_SHIELD}"
pass "locked wallet still rejects shield spend path"

echo "=== SUCCESS: wallet.getshieldedaddress regtest sanity ==="
