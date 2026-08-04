#!/usr/bin/env bash
#
# #357 — stateless epoch-reset persistence must be one atomic commit.
#
# Build a real pre-reset nullifier set on a bridge and synchronize it into a
# CSN. Force the reset block to be stored but not connected, so restart enters
# the real stateless-recovery caller of CommitConnectedBlockBookkeeping; then
# crash immediately after that function commits the reset block but before it
# publishes the in-memory tip. On restart with no peers, the durable state must
# be wholly post-reset: reset tip, empty nullifier CF, matching marker, and the
# same composite shielded state as the bridge at that height.
#
# The precondition is load-bearing: the CSN itself must report a non-zero
# durable nullifier count at H-1. Without it, an empty-to-empty reset would pass
# while never exercising the purge that caused #357.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
RESET_HEIGHT=${RESET_HEIGHT:-115}
MATURITY_HEIGHT=${MATURITY_HEIGHT:-105}
SYNC_TIMEOUT=${SYNC_TIMEOUT:-180}
RUN_ID=$$
BRIDGE_DIR="/tmp/dinero_357_bridge_${RUN_ID}"
CSN_DIR="/tmp/dinero_357_csn_${RUN_ID}"
BRIDGE_LOG="${BRIDGE_DIR}.log"
STORE_LOG="${CSN_DIR}.store.log"
CRASH_LOG="${CSN_DIR}.crash.log"
RESTART_LOG="${CSN_DIR}.restart.log"
CONTINUE_LOG="${CSN_DIR}.continue.log"
BRIDGE_PID=""
CSN_PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    return 1
}

stop_pid() {
    local pid="${1:-}"
    [[ -n "${pid}" ]] || return 0
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 30); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 1
    done
    kill -9 "${pid}" 2>/dev/null || true
}

cleanup() {
    local rc=$?
    stop_pid "${CSN_PID}"
    stop_pid "${BRIDGE_PID}"
    if [[ "${rc}" -ne 0 || "${KEEP_ON_FAIL}" == "1" ]]; then
        for log in "${BRIDGE_LOG}" "${STORE_LOG}" "${CRASH_LOG}" "${RESTART_LOG}" "${CONTINUE_LOG}"; do
            if [[ -f "${log}" ]]; then
                printf -- '--- %s (tail) ---\n' "${log}" >&2
                tail -120 "${log}" >&2 || true
            fi
        done
        printf '[INFO] preserved datadirs: %s %s\n' "${BRIDGE_DIR}" "${CSN_DIR}" >&2
        return
    fi
    rm -rf "${BRIDGE_DIR}" "${CSN_DIR}"
    rm -f "${BRIDGE_LOG}" "${STORE_LOG}" "${CRASH_LOG}" "${RESTART_LOG}" "${CONTINUE_LOG}"
}
trap cleanup EXIT

require_tools() {
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v lsof >/dev/null || fail "lsof is required for port preflight"
}

choose_ports() {
    for _ in $(seq 1 100); do
        local base=$((30000 + RANDOM % 20000))
        local busy=0
        for port in "${base}" "$((base + 1))" "$((base + 2))" "$((base + 3))"; do
            if lsof -nP -iTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1; then
                busy=1
                break
            fi
        done
        if [[ "${busy}" == "0" ]]; then
            RPC_BRIDGE=${base}
            P2P_BRIDGE=$((base + 1))
            RPC_CSN=$((base + 2))
            P2P_CSN=$((base + 3))
            return 0
        fi
    done
    fail "could not allocate four unused TCP ports"
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
    elif [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
    else
        return 1
    fi
}

rpc() {
    local port="$1" datadir="$2" method="$3" params="$4"
    local cookie_path cookie
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    curl -sS --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${port}/"
}

rpc_ok() {
    jq -e '.error == null and has("result")' >/dev/null <<<"$1"
}

rpc_result_number() {
    jq -er '.result | numbers' <<<"$1"
}

rpc_result_string() {
    jq -er '.result | strings' <<<"$1"
}

wait_rpc() {
    local port="$1" datadir="$2" timeout="$3" start
    start=$(date +%s)
    while (( $(date +%s) - start <= timeout )); do
        local response
        response="$(rpc "${port}" "${datadir}" getblockcount '[]' 2>/dev/null || true)"
        if [[ -n "${response}" ]] && rpc_ok "${response}"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_height() {
    local port="$1" datadir="$2" target="$3" timeout="$4" start
    start=$(date +%s)
    while (( $(date +%s) - start <= timeout )); do
        local response height
        response="$(rpc "${port}" "${datadir}" getblockcount '[]' 2>/dev/null || true)"
        height="$(rpc_result_number "${response}" 2>/dev/null || true)"
        if [[ -n "${height}" && "${height}" -ge "${target}" ]]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1" timeout="$2" start
    start=$(date +%s)
    while (( $(date +%s) - start <= timeout )); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 1
    done
    return 1
}

start_bridge() {
    mkdir -p "${BRIDGE_DIR}"
    "${DINEROD}" --regtest --datadir="${BRIDGE_DIR}" \
        --rpcport="${RPC_BRIDGE}" --port="${P2P_BRIDGE}" --listen=1 \
        --utreexo=1 --utreexo-bridge=1 \
        --consensus-shielded-epoch-reset-height="${RESET_HEIGHT}" \
        >"${BRIDGE_LOG}" 2>&1 &
    BRIDGE_PID=$!
}

start_csn_connected() {
    local log="$1"
    shift
    mkdir -p "${CSN_DIR}"
    env "$@" "${DINEROD}" --regtest --datadir="${CSN_DIR}" \
        --rpcport="${RPC_CSN}" --port="${P2P_CSN}" --listen=1 \
        --utreexo=1 --utreexo-stateless=1 \
        --connect="127.0.0.1:${P2P_BRIDGE}" \
        --consensus-shielded-epoch-reset-height="${RESET_HEIGHT}" \
        >"${log}" 2>&1 &
    CSN_PID=$!
}

start_csn_offline() {
    local log="$1"
    mkdir -p "${CSN_DIR}"
    "${DINEROD}" --regtest --datadir="${CSN_DIR}" \
        --rpcport="${RPC_CSN}" --port="${P2P_CSN}" --listen=0 \
        --p2p.offline=1 --utreexo=1 --utreexo-stateless=1 \
        --consensus-shielded-epoch-reset-height="${RESET_HEIGHT}" \
        >"${log}" 2>&1 &
    CSN_PID=$!
}

stop_csn_cleanly() {
    rpc "${RPC_CSN}" "${CSN_DIR}" stop '[]' >/dev/null 2>&1 || true
    wait_dead "${CSN_PID}" 30 || stop_pid "${CSN_PID}"
    CSN_PID=""
}

mine_to() {
    local target="$1" address="$2"
    local response current count
    response="$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" getblockcount '[]')"
    rpc_ok "${response}" || fail "bridge getblockcount failed: ${response}"
    current="$(rpc_result_number "${response}")"
    count=$((target - current))
    (( count <= 0 )) && return 0
    response="$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" mining.generatetoaddress "[${count},\"${address}\"]")"
    rpc_ok "${response}" || fail "mining to ${target} failed: ${response}"
    wait_height "${RPC_BRIDGE}" "${BRIDGE_DIR}" "${target}" 90
}

shielded_state_hash() {
    local response
    response="$(rpc "$1" "$2" daemon.shieldedstatehash '[]')"
    jq -er '.error == null and (.result.state_hash | strings) as $h | $h' <<<"${response}"
}

require_tools
choose_ports
rm -rf "${BRIDGE_DIR}" "${CSN_DIR}"
rm -f "${BRIDGE_LOG}" "${STORE_LOG}" "${CRASH_LOG}" "${RESTART_LOG}" "${CONTINUE_LOG}"

info "starting bridge; reset height=${RESET_HEIGHT}"
start_bridge
wait_rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" 45 || fail "bridge did not reach RPC readiness"

wallet="$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" wallet.createhd '["issue357"]')"
rpc_ok "${wallet}" || fail "wallet.createhd failed: ${wallet}"
address="$(jq -er '.result.first_address | strings' <<<"${wallet}")"

info "building a real pre-reset nullifier set"
mine_to "${MATURITY_HEIGHT}" "${address}" || fail "mine to maturity failed"
shield="$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" wallet.shield '[5.0]')"
jq -e '.error == null and .result.status == "shielded"' >/dev/null <<<"${shield}" \
    || fail "wallet.shield failed: ${shield}"
mine_to "$((MATURITY_HEIGHT + 2))" "${address}" || fail "shield confirmation failed"
unshield="$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" wallet.unshield '[2.0]')"
jq -e '.error == null and .result.status == "unshielded"' >/dev/null <<<"${unshield}" \
    || fail "wallet.unshield failed: ${unshield}"
mine_to "$((MATURITY_HEIGHT + 4))" "${address}" || fail "unshield confirmation failed"
mine_to "$((RESET_HEIGHT - 1))" "${address}" || fail "mine to reset-1 failed"

info "synchronizing CSN to H-1 and proving its durable nullifier set is non-empty"
start_csn_connected "${CONTINUE_LOG}"
wait_rpc "${RPC_CSN}" "${CSN_DIR}" 45 || fail "CSN did not reach RPC readiness"
wait_height "${RPC_CSN}" "${CSN_DIR}" "$((RESET_HEIGHT - 1))" "${SYNC_TIMEOUT}" \
    || fail "CSN did not synchronize to reset-1"
health_before="$(rpc "${RPC_CSN}" "${CSN_DIR}" blockchain.getsynchealth '[]')"
jq -e '.error == null and
       .result.shielded_nullifier_count > 0 and
       .result.shielded_tip_marker_nullifier_count > 0' >/dev/null <<<"${health_before}" \
    || fail "precondition failed: CSN has no durable pre-reset nullifiers: ${health_before}"
pass "precondition: CSN persisted non-zero pre-reset nullifier rows"
stop_csn_cleanly

info "mining exactly the reset block on the bridge"
mine_to "${RESET_HEIGHT}" "${address}" || fail "mine reset block failed"
bridge_tip="$(rpc_result_string "$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" getbestblockhash '[]')")"
bridge_state="$(shielded_state_hash "${RPC_BRIDGE}" "${BRIDGE_DIR}")"

info "forcing reset block H to be stored but not connected"
start_csn_connected "${CRASH_LOG}" \
    DINERO_DEBUG_ABORT_AFTER_STORE_HEIGHT="${RESET_HEIGHT}"
wait_dead "${CSN_PID}" "${SYNC_TIMEOUT}" || fail "CSN did not abort after storing the reset block"
set +e
wait "${CSN_PID}"
store_exit=$?
set -e
CSN_PID=""
[[ "${store_exit}" == "70" ]] || fail "store-ahead abort exit ${store_exit}, expected 70"
grep -q "aborting after block store+index at height ${RESET_HEIGHT}" "${CRASH_LOG}" \
    || fail "store-ahead log does not prove reset block was stored before exit"
grep -qF "ConnectTip SUCCEEDED for height ${RESET_HEIGHT}" "${CRASH_LOG}" \
    && fail "reset block connected before the store-ahead abort"
pass "reset block is durable but unconnected, forcing stateless recovery on restart"
mv "${CRASH_LOG}" "${STORE_LOG}"

info "crashing stateless recovery after the reset batch commit and before tip publication"
start_csn_connected "${CRASH_LOG}" \
    DINERO_CRASH_AT=after_csn_epoch_reset_batch_before_publish
wait_dead "${CSN_PID}" "${SYNC_TIMEOUT}" || fail "CSN did not hit the reset crash hook"
set +e
wait "${CSN_PID}"
set -e
CSN_PID=""
grep -q "aborting at hook 'after_csn_epoch_reset_batch_before_publish'" "${CRASH_LOG}" \
    || fail "crash log does not prove the named hook fired"
grep -q "Stateless replay path: advancing shared forest from stored proof data at height ${RESET_HEIGHT}" "${CRASH_LOG}" \
    || fail "recovery branch did not run for the reset block; test would be vacuous"
pass "reset crash hook fired after the unified durable commit"

info "restarting CSN offline; no peer may repair or advance its state"
start_csn_offline "${RESTART_LOG}"
wait_rpc "${RPC_CSN}" "${CSN_DIR}" 60 \
    || fail "offline CSN restart failed (post-reset marker/nullifier mismatch)"

height_after="$(rpc_result_number "$(rpc "${RPC_CSN}" "${CSN_DIR}" getblockcount '[]')")"
tip_after="$(rpc_result_string "$(rpc "${RPC_CSN}" "${CSN_DIR}" getbestblockhash '[]')")"
state_after="$(shielded_state_hash "${RPC_CSN}" "${CSN_DIR}")"
health_after="$(rpc "${RPC_CSN}" "${CSN_DIR}" blockchain.getsynchealth '[]')"

[[ "${height_after}" == "${RESET_HEIGHT}" ]] \
    || fail "offline restart height ${height_after}, expected reset height ${RESET_HEIGHT}"
[[ "${tip_after}" == "${bridge_tip}" ]] \
    || fail "offline restart tip differs from bridge reset tip"
[[ "${state_after}" == "${bridge_state}" ]] \
    || fail "offline restart shielded state differs from bridge at reset height"
jq -e '.error == null and
       .result.canonical_state_aligned == true and
       .result.shielded_nullifier_count == 0 and
       .result.shielded_tip_marker_nullifier_count == 0' >/dev/null <<<"${health_after}" \
    || fail "offline restart retained/resurrected pre-reset nullifiers: ${health_after}"
pass "offline restart observed the complete post-reset state with zero nullifiers"

stop_csn_cleanly
info "continuing two blocks proves the recovered CSN remains usable"
mine_to "$((RESET_HEIGHT + 2))" "${address}" || fail "bridge continuation failed"
bridge_final_tip="$(rpc_result_string "$(rpc "${RPC_BRIDGE}" "${BRIDGE_DIR}" getbestblockhash '[]')")"
start_csn_connected "${CONTINUE_LOG}"
wait_rpc "${RPC_CSN}" "${CSN_DIR}" 45 || fail "continued CSN did not reach RPC readiness"
wait_height "${RPC_CSN}" "${CSN_DIR}" "$((RESET_HEIGHT + 2))" "${SYNC_TIMEOUT}" \
    || fail "continued CSN did not synchronize beyond the reset"
csn_final_tip="$(rpc_result_string "$(rpc "${RPC_CSN}" "${CSN_DIR}" getbestblockhash '[]')")"
[[ "${csn_final_tip}" == "${bridge_final_tip}" ]] || fail "continued CSN diverged from bridge"
pass "CSN continued synchronizing after the crash-safe reset restart"

stop_csn_cleanly
pass "#357 reset purge, marker, tip, and height index are crash-atomic"
