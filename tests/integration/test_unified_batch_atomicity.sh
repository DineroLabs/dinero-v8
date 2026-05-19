#!/usr/bin/env bash
#
# Phase 3b step 5 — crash-injection coverage for the unified-batch
# §1 atomic-unit law in ConnectTip.
#
# After phase 3b step 3 part 3 (slices 1–6) and step 4, ConnectTip's
# rocksdb::WriteBatch carries every disk write needed to advance the
# chain by one block: UTXO + txindex + forest checkpoint + CHECKSUM_VERSION
# + ForestTipMarker + BLOCK_HAVE_UNDO metadata + ShieldedTipMarker +
# canonical setTip + height index + (flag-on) consensus journal row.
# §1's law says: a crash before writeBatch returns leaves NONE of the
# above on disk; a crash after leaves ALL of them on disk. This test
# verifies that empirically by crashing at two boundaries and probing
# every container's height for alignment.
#
#   Boundary A: DINERO_CRASH_AT=after_undo_before_tip — fires
#               immediately BEFORE chain_db_->writeBatch() in
#               ConnectTip's slice-6 staging block. Pre-batch crash.
#               At restart, every container must agree on the
#               pre-block height.
#
#   Boundary B: DINERO_CRASH_AT=after_tip_before_checkpoint — fires
#               AFTER the unified batch + frontier flat-file write.
#               Post-batch crash. At restart, every container must
#               agree on the post-block height.
#
# Each boundary runs twice: once with consensus.atomic_persist=0 (the
# baseline, journal row absent) and once with consensus.atomic_persist=1
# (journal row carried by the same batch). The flag-on path adds the
# implicit assertion "if the journal row didn't follow the §1 law,
# VerifyConsensusJournalAtActiveTip would trip safe mode at startup."
# The test asserts safe mode is NOT active after either restart.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_unified_batch_atomicity_$$"
LOG_BASE="${DATA_DIR}.base.log"
LOG_CRASH="${DATA_DIR}.crash.log"
LOG_RESTART="${DATA_DIR}.restart.log"
PID=""
KEEP_ON_FAIL=0
RPC_PORT=$((33700 + RANDOM % 200))
P2P_PORT=$((RPC_PORT + 1))

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_CRASH}" ]] && { printf -- '--- crash log tail ---\n' >&2; tail -120 "${LOG_CRASH}" >&2 || true; }
    [[ -f "${LOG_RESTART}" ]] && { printf -- '--- restart log tail ---\n' >&2; tail -120 "${LOG_RESTART}" >&2 || true; }
    exit 1
}
cleanup() {
    [[ -n "${PID}" ]] && kill "${PID}" 2>/dev/null || true
    pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null || true
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${DATA_DIR}" "${LOG_BASE}" "${LOG_CRASH}" "${LOG_RESTART}"
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
    [[ -f "${datadir}/.cookie" ]] && { printf '%s\n' "${datadir}/.cookie"; return 0; }
    [[ -f "${datadir}/regtest/.cookie" ]] && { printf '%s\n' "${datadir}/regtest/.cookie"; return 0; }
    return 1
}

rpc_call() {
    local method="$1" params="$2"
    local cookie_path
    cookie_path="$(cookie_file "${DATA_DIR}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

wait_rpc() {
    for _ in $(seq 1 60); do
        if rpc_call "getblockcount" '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_dead() {
    local pid="$1"
    for _ in $(seq 1 30); do
        kill -0 "${pid}" 2>/dev/null || return 0
        sleep 1
    done
    return 1
}

start_node() {
    local log_file="$1"
    shift
    mkdir -p "${DATA_DIR}"
    local atomic_flag=()
    case "${ATOMIC_PERSIST:-0}" in
        1|true|yes|on) atomic_flag=(-consensus.atomic_persist=1) ;;
    esac
    env "$@" "${DINEROD}" \
        -regtest \
        -datadir="${DATA_DIR}" \
        -rpcport="${RPC_PORT}" \
        -port="${P2P_PORT}" \
        -listen=0 \
        ${atomic_flag[@]+"${atomic_flag[@]}"} \
        > "${log_file}" 2>&1 &
    PID=$!
}

stop_node() {
    [[ -n "${PID}" ]] || return 0
    kill "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
    PID=""
}

# Probe every container that the unified batch + post-batch frontier
# write touches. Returns a single composite line so callers can
# assert all heights agree:
#
#   <chaindb_tip>:<forest_marker>:<utreexo_checkpoint>:<shielded_marker>:<active>:<safe_mode>
probe_state() {
    local sync
    sync="$(rpc_call "blockchain.getsynchealth" '[]')"
    jq -e '.error == null' <<<"${sync}" >/dev/null \
        || fail "blockchain.getsynchealth failed: ${sync}"
    local chaindb_tip forest_marker checkpoint shielded_marker active safe_mode
    chaindb_tip="$(jq -r '.result.chaindb_tip_height // -1' <<<"${sync}")"
    forest_marker="$(jq -r '.result.forest_tip_marker_height // -1' <<<"${sync}")"
    checkpoint="$(jq -r '.result.latest_utreexo_checkpoint_height // -1' <<<"${sync}")"
    shielded_marker="$(jq -r '.result.shielded_tip_marker_height // -1' <<<"${sync}")"
    active="$(jq -r '.result.active_height // -1' <<<"${sync}")"

    local sm
    sm="$(rpc_call "safemode.status" '[]')"
    if jq -e '.error == null' <<<"${sm}" >/dev/null 2>&1; then
        safe_mode="$(jq -r '.result.active // false' <<<"${sm}")"
    else
        # safemode.status RPC absent — treat as not in safe mode.
        safe_mode="false"
    fi

    printf '%s:%s:%s:%s:%s:%s\n' \
        "${chaindb_tip}" "${forest_marker}" "${checkpoint}" \
        "${shielded_marker}" "${active}" "${safe_mode}"
}

# Assert all five container heights match and safe mode is not
# active. Returns the (matched) height on success.
assert_atomic_alignment() {
    local label="$1"
    local probe="$2"
    local chaindb_tip forest_marker checkpoint shielded_marker active safe_mode
    IFS=: read -r chaindb_tip forest_marker checkpoint shielded_marker active safe_mode \
        <<<"${probe}"

    [[ "${safe_mode}" == "false" ]] \
        || fail "${label}: safe mode active after restart (probe=${probe})"

    [[ "${chaindb_tip}" == "${forest_marker}" ]] \
        || fail "${label}: chaindb_tip ${chaindb_tip} != forest_tip_marker ${forest_marker}"
    [[ "${chaindb_tip}" == "${checkpoint}" ]] \
        || fail "${label}: chaindb_tip ${chaindb_tip} != utreexo_checkpoint ${checkpoint}"
    [[ "${chaindb_tip}" == "${shielded_marker}" ]] \
        || fail "${label}: chaindb_tip ${chaindb_tip} != shielded_tip_marker ${shielded_marker}"
    [[ "${chaindb_tip}" == "${active}" ]] \
        || fail "${label}: chaindb_tip ${chaindb_tip} != active_height ${active}"

    printf '%s\n' "${chaindb_tip}"
}

# Mine `count` blocks and refuse to proceed if generatetoaddress
# returns an error. Returns the new tip height.
mine_blocks() {
    local count="$1" addr="$2"
    local result
    result="$(rpc_call "mining.generatetoaddress" "[${count},\"${addr}\"]")"
    jq -e '.error == null' <<<"${result}" >/dev/null \
        || fail "generatetoaddress failed: ${result}"
}

# Run one crash-boundary scenario.
#
#   $1 — descriptive label
#   $2 — DINERO_CRASH_AT hook name
#   $3 — expected height delta for boundary ("0" = pre-batch boundary,
#        chain stays at baseline; "1" = post-batch boundary, chain
#        advances by one)
run_boundary() {
    local label="$1" hook="$2" expect_delta="$3"

    info "[${label}] starting baseline daemon"
    start_node "${LOG_BASE}"
    wait_rpc || fail "[${label}] baseline daemon did not reach RPC readiness"

    local addr_result miner_addr
    addr_result="$(rpc_call "getnewaddress" '[]')"
    jq -e '.error == null' <<<"${addr_result}" >/dev/null \
        || fail "[${label}] getnewaddress failed: ${addr_result}"
    miner_addr="$(jq -r '.result.address // .result // empty' <<<"${addr_result}")"
    [[ -n "${miner_addr}" && "${miner_addr}" != "null" ]] \
        || fail "[${label}] empty miner address"

    info "[${label}] mining baseline of 5 blocks"
    mine_blocks 5 "${miner_addr}"

    local baseline_count
    baseline_count="$(rpc_call "getblockcount" '[]' | jq -r '.result')"
    [[ "${baseline_count}" -ge 5 ]] \
        || fail "[${label}] baseline did not reach height 5: ${baseline_count}"
    pass "[${label}] baseline at height ${baseline_count}"

    stop_node

    info "[${label}] starting crash daemon with DINERO_CRASH_AT=${hook}"
    start_node "${LOG_CRASH}" "DINERO_CRASH_AT=${hook}"
    wait_rpc || fail "[${label}] crash daemon did not reach RPC readiness"

    set +e
    rpc_call "mining.generatetoaddress" "[1,\"${miner_addr}\"]" >/dev/null 2>&1
    set -e
    wait_dead "${PID}" || fail "[${label}] daemon did not crash at ${hook}"
    PID=""
    grep -q "DINERO_CRASH" "${LOG_CRASH}" \
        || fail "[${label}] crash log did not show named crash hook"
    pass "[${label}] crash hook ${hook} fired"

    info "[${label}] restarting daemon"
    start_node "${LOG_RESTART}"
    wait_rpc || fail "[${label}] restarted daemon did not reach RPC readiness"

    local probe
    probe="$(probe_state)"
    local recovered
    recovered="$(assert_atomic_alignment "${label} restart" "${probe}")"
    pass "[${label}] all containers aligned at height ${recovered} (probe=${probe})"

    local expected=$((baseline_count + expect_delta))
    [[ "${recovered}" == "${expected}" ]] \
        || fail "[${label}] recovered height ${recovered} != expected ${expected}" \
                "(baseline=${baseline_count} delta=${expect_delta} probe=${probe})"
    pass "[${label}] §1 atomic-unit law upheld: chain at ${recovered}, baseline + ${expect_delta}"

    info "[${label}] forward-progress sanity: mine one more block"
    mine_blocks 1 "${miner_addr}"
    local final_count
    final_count="$(rpc_call "getblockcount" '[]' | jq -r '.result')"
    [[ "${final_count}" == $((recovered + 1)) ]] \
        || fail "[${label}] post-restart mining did not advance: before=${recovered} after=${final_count}"

    local final_probe
    final_probe="$(probe_state)"
    assert_atomic_alignment "${label} post-mine" "${final_probe}" >/dev/null
    pass "[${label}] forward-progress preserved alignment at height ${final_count}"

    stop_node
    rm -rf "${DATA_DIR}"
    rm -f "${LOG_BASE}" "${LOG_CRASH}" "${LOG_RESTART}"
}

require_tools

# ATOMIC_PERSIST is honored by start_node and routes both daemon
# instances through ConsensusWriteBatch. The harness drives both
# scenarios — flag-off and flag-on are scheduled as separate ctests
# so a flag-on regression trips CI loud independently.
info "ATOMIC_PERSIST=${ATOMIC_PERSIST:-0}"

# Boundary A: pre-batch. Chain MUST stay at baseline.
run_boundary "boundary-A pre-batch" "after_undo_before_tip" 0

# Boundary B: post-batch. Chain MUST advance by one.
run_boundary "boundary-B post-batch" "after_tip_before_checkpoint" 1

pass "Phase 3b step 5: §1 atomic-unit law upheld at both boundaries"
