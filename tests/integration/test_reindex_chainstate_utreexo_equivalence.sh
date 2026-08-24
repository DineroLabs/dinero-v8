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
DATA_DIR="/tmp/dinero_reindex_equiv_$$"
LOG_LIVE="${DATA_DIR}.live.log"
LOG_REINDEX_CRASH="${DATA_DIR}.reindex_crash.log"
LOG_REINDEX="${DATA_DIR}.reindex.log"
PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_LIVE}" ]] && { printf -- '--- live log tail ---\n' >&2; tail -120 "${LOG_LIVE}" >&2 || true; }
    [[ -f "${LOG_REINDEX_CRASH}" ]] && { printf -- '--- reindex crash log tail ---\n' >&2; tail -160 "${LOG_REINDEX_CRASH}" >&2 || true; }
    [[ -f "${LOG_REINDEX}" ]] && { printf -- '--- reindex log tail ---\n' >&2; tail -160 "${LOG_REINDEX}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_LIVE}" "${LOG_REINDEX_CRASH}" "${LOG_REINDEX}"
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
    local datadir="$1"
    local method="$2"
    local params_json="$3"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

rpc_has_error() {
    local compact
    compact="$(echo "$1" | tr -d '\n\t ')"
    [[ "${compact}" == *"\"error\":null"* ]] && return 1
    [[ "${compact}" == *"\"error\":"* ]] && return 0
    return 1
}

wait_rpc() {
    for _ in $(seq 1 90); do
        if rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 60); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --wallet-socket-port="${WALLET_PORT}" \
        --listen=0 \
        --utreexo=1 \
        --p2p.offline=1 \
        "$@" \
        >"${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    local stop_result
    stop_result="$(rpc_call "${DATA_DIR}" "stop" '[]' 2>/dev/null || true)"
    if [[ -n "${stop_result}" ]] && rpc_has_error "${stop_result}"; then
        kill "${PID}" 2>/dev/null || true
    fi
    wait_dead "${PID}" || kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

rpc_address() {
    local response="$1"
    jq -r '.result.address // .result // empty' <<<"${response}"
}

mine_blocks() {
    local blocks="$1"
    local address="$2"
    local result
    result="$(rpc_call "${DATA_DIR}" "generatetoaddress" "[${blocks},\"${address}\"]")"
    rpc_has_error "${result}" && fail "generatetoaddress failed: ${result}"
    return 0
}

send_to_address() {
    local address="$1"
    local amount="$2"
    local result
    result="$(rpc_call "${DATA_DIR}" "wallet.sendtoaddress" "[\"${address}\",${amount}]")"
    rpc_has_error "${result}" && fail "wallet.sendtoaddress failed: ${result}"
    local txid
    txid="$(jq -r '.result.txid // .result // empty' <<<"${result}")"
    [[ -n "${txid}" && "${txid}" != "null" ]] || fail "wallet.sendtoaddress returned empty txid"
    return 0
}

capture_state_json() {
    local health commitment roots
    health="$(rpc_call "${DATA_DIR}" "blockchain.getsynchealth" '[]')"
    commitment="$(rpc_call "${DATA_DIR}" "blockchain.getutreexocommitment" '[]')"
    roots="$(rpc_call "${DATA_DIR}" "blockchain.getutreexoroots" '[]')"

    rpc_has_error "${health}" && fail "blockchain.getsynchealth failed: ${health}"
    rpc_has_error "${commitment}" && fail "blockchain.getutreexocommitment failed: ${commitment}"
    rpc_has_error "${roots}" && fail "blockchain.getutreexoroots failed: ${roots}"

    jq -n \
        --argjson health "$(echo "${health}" | jq -c '.result')" \
        --argjson commitment "$(echo "${commitment}" | jq -c '.result')" \
        --argjson roots "$(echo "${roots}" | jq -c '.result')" \
        '{
            active_height: ($health.active_height // -1),
            active_best_hash: ($health.active_best_hash // ""),
            chaindb_tip_height: ($health.chaindb_tip_height // -1),
            chaindb_tip_hash: ($health.chaindb_tip_hash // ""),
            canonical_state_aligned: ($health.canonical_state_aligned // false),
            latest_utreexo_checkpoint_found: ($health.latest_utreexo_checkpoint_found // false),
            latest_utreexo_checkpoint_height: ($health.latest_utreexo_checkpoint_height // -1),
            latest_utreexo_checkpoint_has_checksum: ($health.latest_utreexo_checkpoint_has_checksum // false),
            utreexo_checksum_version: ($health.utreexo_checksum_version // ""),
            forest_tip_marker_found: ($health.forest_tip_marker_found // false),
            forest_tip_marker_height: ($health.forest_tip_marker_height // -1),
            forest_tip_marker_hash: ($health.forest_tip_marker_hash // ""),
            forest_tip_marker_root: ($health.forest_tip_marker_root // ""),
            commitment: ($commitment.commitment // ""),
            num_leaves: ($commitment.num_leaves // 0),
            num_roots: ($commitment.num_roots // 0),
            roots: ($roots.roots // [])
        }'
}

assert_same_state() {
    local lhs_json="$1"
    local rhs_json="$2"
    local label="$3"
    local mismatch
    mismatch="$(jq -n \
        --argjson lhs "${lhs_json}" \
        --argjson rhs "${rhs_json}" \
        '{
            active_height: ($lhs.active_height == $rhs.active_height),
            active_best_hash: ($lhs.active_best_hash == $rhs.active_best_hash),
            chaindb_tip_height: ($lhs.chaindb_tip_height == $rhs.chaindb_tip_height),
            chaindb_tip_hash: ($lhs.chaindb_tip_hash == $rhs.chaindb_tip_hash),
            canonical_state_aligned: ($lhs.canonical_state_aligned == $rhs.canonical_state_aligned),
            latest_utreexo_checkpoint_found: ($lhs.latest_utreexo_checkpoint_found == $rhs.latest_utreexo_checkpoint_found),
            # Checkpoint HEIGHT and checksum-version are storage layout, not
            # chain state: with utreexo.checkpoint_interval > 1 (campaign
            # phase 3 default) the live node holds its last interval
            # checkpoint while a reindexed datadir anchors at its final tip.
            # Equivalence requires each side to hold SOME valid checkpoint at
            # or below its (already-compared-equal) tip.
            latest_utreexo_checkpoint_sane:
                ($lhs.latest_utreexo_checkpoint_height <= $lhs.chaindb_tip_height and
                 $rhs.latest_utreexo_checkpoint_height <= $rhs.chaindb_tip_height),
            latest_utreexo_checkpoint_has_checksum: ($lhs.latest_utreexo_checkpoint_has_checksum == $rhs.latest_utreexo_checkpoint_has_checksum),
            forest_tip_marker_found: ($lhs.forest_tip_marker_found == $rhs.forest_tip_marker_found),
            forest_tip_marker_height: ($lhs.forest_tip_marker_height == $rhs.forest_tip_marker_height),
            forest_tip_marker_hash: ($lhs.forest_tip_marker_hash == $rhs.forest_tip_marker_hash),
            forest_tip_marker_root: ($lhs.forest_tip_marker_root == $rhs.forest_tip_marker_root),
            commitment: ($lhs.commitment == $rhs.commitment),
            num_leaves: ($lhs.num_leaves == $rhs.num_leaves),
            num_roots: ($lhs.num_roots == $rhs.num_roots),
            roots: ($lhs.roots == $rhs.roots)
        }')"
    if [[ "$(echo "${mismatch}" | jq -r 'all(.[]; . == true)')" != "true" ]]; then
        echo "${label} mismatch:"
        echo "lhs=$(echo "${lhs_json}" | jq -c '.')"
        echo "rhs=$(echo "${rhs_json}" | jq -c '.')"
        echo "eq =$(echo "${mismatch}" | jq -c '.')"
        fail "${label} state mismatch"
    fi
}

require_tools

RPC_PORT=$((34000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))
REINDEX_PROMOTION_CRASH="${REINDEX_PROMOTION_CRASH:-0}"

start_node "${LOG_LIVE}"
wait_rpc || fail "live daemon did not reach RPC readiness"

ADDR_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${ADDR_RESULT}" && fail "wallet.getnewaddress failed: ${ADDR_RESULT}"
MINER_ADDR="$(rpc_address "${ADDR_RESULT}")"
[[ -n "${MINER_ADDR}" ]] || fail "wallet.getnewaddress returned empty mining address"

RECIPIENT_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${RECIPIENT_RESULT}" && fail "wallet.getnewaddress failed for recipient: ${RECIPIENT_RESULT}"
RECIPIENT_ADDR="$(rpc_address "${RECIPIENT_RESULT}")"
[[ -n "${RECIPIENT_ADDR}" ]] || fail "wallet.getnewaddress returned empty recipient address"

SECOND_RECIPIENT_RESULT="$(rpc_call "${DATA_DIR}" "wallet.getnewaddress" '[]')"
rpc_has_error "${SECOND_RECIPIENT_RESULT}" && fail "wallet.getnewaddress failed for second recipient: ${SECOND_RECIPIENT_RESULT}"
SECOND_RECIPIENT_ADDR="$(rpc_address "${SECOND_RECIPIENT_RESULT}")"
[[ -n "${SECOND_RECIPIENT_ADDR}" ]] || fail "wallet.getnewaddress returned empty second recipient address"

info "Building a nontrivial regtest chain for reindex equivalence"
mine_blocks 110 "${MINER_ADDR}"
send_to_address "${RECIPIENT_ADDR}" "1.25"
mine_blocks 1 "${MINER_ADDR}"
send_to_address "${SECOND_RECIPIENT_ADDR}" "0.75"
mine_blocks 2 "${MINER_ADDR}"

STATE_BEFORE="$(capture_state_json)"
jq -e '.canonical_state_aligned == true' <<<"${STATE_BEFORE}" >/dev/null || fail "pre-reindex canonical state not aligned: ${STATE_BEFORE}"
pass "Captured pre-reindex canonical state"

stop_node

if [[ "${REINDEX_PROMOTION_CRASH}" == "1" ]]; then
    info "Restarting with --reindex-chainstate and crashing during promotion"
    DINERO_CRASH_AT=after_reindex_backup_before_promote start_node "${LOG_REINDEX_CRASH}" --reindex-chainstate
    wait_dead "${PID}" || fail "reindex daemon did not crash at after_reindex_backup_before_promote"
    PID=""
    grep -q "DINERO_CRASH" "${LOG_REINDEX_CRASH}" || fail "reindex crash log did not show named crash hook"
    pass "Crash hook triggered during reindex promotion after moving live data aside"

    info "Restarting cleanly to recover interrupted reindex promotion"
    start_node "${LOG_REINDEX}"
else
    info "Restarting with --reindex-chainstate"
    start_node "${LOG_REINDEX}" --reindex-chainstate
fi
wait_rpc || fail "reindex daemon did not reach RPC readiness"

if [[ "${REINDEX_PROMOTION_CRASH}" == "1" ]]; then
    grep -q "Recovered interrupted reindex promotion" "${LOG_REINDEX}" || fail "restart log missing interrupted-promotion recovery banner"
else
    grep -q "REINDEX OPERATION REQUESTED" "${LOG_REINDEX}" || fail "reindex log missing reindex start banner"
    grep -q "REINDEX COMPLETE" "${LOG_REINDEX}" || fail "reindex log missing completion banner"
fi

STATE_AFTER="$(capture_state_json)"
jq -e '.canonical_state_aligned == true' <<<"${STATE_AFTER}" >/dev/null || fail "post-reindex canonical state not aligned: ${STATE_AFTER}"
assert_same_state "${STATE_BEFORE}" "${STATE_AFTER}" "reindex-chainstate equivalence"
pass "Reindex preserved canonical ChainDB + Utreexo state"

HEIGHT_BEFORE="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
mine_blocks 1 "${MINER_ADDR}"
HEIGHT_AFTER="$(rpc_call "${DATA_DIR}" "getblockcount" '[]' | jq -r '.result')"
[[ "${HEIGHT_AFTER}" == "$((HEIGHT_BEFORE + 1))" ]] || fail "height did not advance after post-reindex mining: before=${HEIGHT_BEFORE} after=${HEIGHT_AFTER}"

STATE_FINAL="$(capture_state_json)"
jq -e '.canonical_state_aligned == true' <<<"${STATE_FINAL}" >/dev/null || fail "canonical state drifted after post-reindex mining: ${STATE_FINAL}"
pass "Post-reindex mining preserved canonical alignment"

stop_node
