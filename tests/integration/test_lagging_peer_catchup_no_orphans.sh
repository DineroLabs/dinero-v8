#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${ROOT_DIR}/build/dinerod"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
CHECKER="${ROOT_DIR}/tools/check_seed_consistency.py"
BASE_PORT="${BASE_PORT:-35600}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
BOOTSTRAP_BLOCKS="${BOOTSTRAP_BLOCKS:-120}"
EXTRA_BLOCKS="${EXTRA_BLOCKS:-20}"
DATA_A="/tmp/dinero_lagging_peer_a_$$"
DATA_B="/tmp/dinero_lagging_peer_b_$$"
LOG_A="${DATA_A}.log"
LOG_B="${DATA_B}.log"
REPORT_JSON="/tmp/dinero_lagging_peer_report_$$.json"
PID_A=""
PID_B=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG_A}" ]] && { printf -- '--- node A log tail ---\n' >&2; tail -120 "${LOG_A}" >&2 || true; }
    [[ -f "${LOG_B}" ]] && { printf -- '--- node B log tail ---\n' >&2; tail -160 "${LOG_B}" >&2 || true; }
    exit 1
}

cleanup() {
    local test_rc=$?
    local cleanup_rc=0
    local final_rc=0
    trap - EXIT
    set +e
    dinero_stop_process "${PID_A}" "catchup node A" || cleanup_rc=1
    dinero_stop_process "${PID_B}" "catchup node B" || cleanup_rc=1
    dinero_stop_datadir_processes "${DATA_A}" || cleanup_rc=1
    dinero_stop_datadir_processes "${DATA_B}" || cleanup_rc=1
    if [[ "${KEEP_ON_FAIL}" != "1" && "${cleanup_rc}" -eq 0 ]]; then
        rm -rf "${DATA_A}" "${DATA_B}" "${LOG_A}" "${LOG_B}" "${REPORT_JSON}" || cleanup_rc=1
    fi
    dinero_cleanup_result "${test_rc}" "${cleanup_rc}" || final_rc=$?
    exit "${final_rc}"
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v python3 >/dev/null || fail "python3 is required"
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
    local rpc_port="$1"
    local datadir="$2"
    local method="$3"
    local params_json="$4"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${rpc_port}/"
}

wait_rpc() {
    local rpc_port="$1"
    local datadir="$2"
    for _ in $(seq 1 60); do
        if rpc_call "${rpc_port}" "${datadir}" "getblockcount" '[]' | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_condition() {
    local cmd="$1"
    local message="$2"
    for _ in $(seq 1 90); do
        if eval "${cmd}"; then
            return 0
        fi
        sleep 1
    done
    fail "${message}"
}

count_matches() {
    local pattern="$1"
    local file="$2"
    local count
    count="$(grep -F -c "${pattern}" "${file}" 2>/dev/null || true)"
    printf '%s\n' "${count:-0}"
}

start_node() {
    local datadir="$1"
    local rpc_port="$2"
    local p2p_port="$3"
    local logfile="$4"

    mkdir -p "${datadir}"
    "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

get_height() {
    local rpc_port="$1"
    local datadir="$2"
    rpc_call "${rpc_port}" "${datadir}" "getblockcount" '[]' | jq -r '.result // -1'
}

get_best_hash() {
    local rpc_port="$1"
    local datadir="$2"
    rpc_call "${rpc_port}" "${datadir}" "getbestblockhash" '[]' | jq -r '.result // empty'
}

require_tools

PID_A="$(start_node "${DATA_A}" "${NODE_A_RPC}" "${NODE_A_P2P}" "${LOG_A}")"
wait_rpc "${NODE_A_RPC}" "${DATA_A}" || fail "Node A RPC did not come up"
pass "Node A is up"

rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.createhd" "[\"lagging-peer\"]" >/dev/null 2>&1 || true
ADDR_A="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "wallet.getnewaddress" '[]' | jq -r '.result.address // .result // empty')"
[[ -n "${ADDR_A}" ]] || fail "Failed to obtain mining address on node A"

info "Mining ${BOOTSTRAP_BLOCKS} bootstrap blocks on node A while node B is offline"
BOOTSTRAP_RESULT="$(rpc_call "${NODE_A_RPC}" "${DATA_A}" "generatetoaddress" "[${BOOTSTRAP_BLOCKS},\"${ADDR_A}\"]")"
jq -e '.error == null' <<<"${BOOTSTRAP_RESULT}" >/dev/null || fail "Node A bootstrap mining failed: ${BOOTSTRAP_RESULT}"
pass "Node A reached height $(get_height "${NODE_A_RPC}" "${DATA_A}")"

PID_B="$(start_node "${DATA_B}" "${NODE_B_RPC}" "${NODE_B_P2P}" "${LOG_B}")"
wait_rpc "${NODE_B_RPC}" "${DATA_B}" || fail "Node B RPC did not come up"
pass "Node B is up"

rpc_call "${NODE_B_RPC}" "${DATA_B}" "addnode" "[\"127.0.0.1:${NODE_A_P2P}\",\"onetry\"]" >/dev/null
wait_condition "[[ \$(rpc_call \"${NODE_B_RPC}\" \"${DATA_B}\" \"getconnectioncount\" '[]' | jq -r '.result // 0') -ge 1 ]]" \
    "Node B did not connect to node A"
pass "Node B connected to node A"

info "Mining ${EXTRA_BLOCKS} more blocks on node A during node B catch-up"
(
    for _ in $(seq 1 "${EXTRA_BLOCKS}"); do
        rpc_call "${NODE_A_RPC}" "${DATA_A}" "generatetoaddress" "[1,\"${ADDR_A}\"]" >/dev/null || exit 1
        sleep 0.2
    done
) &
MINER_PID=$!

wait "${MINER_PID}" || fail "Background mining failed during catch-up"
wait_condition "[[ \$(get_height \"${NODE_B_RPC}\" \"${DATA_B}\") -eq \$(get_height \"${NODE_A_RPC}\" \"${DATA_A}\") && \"\$(get_best_hash \"${NODE_B_RPC}\" \"${DATA_B}\")\" = \"\$(get_best_hash \"${NODE_A_RPC}\" \"${DATA_A}\")\" ]]" \
    "Node B did not converge to node A tip"
pass "Lagging follower converged to node A tip"

python3 "${CHECKER}" \
    --seed a=http://127.0.0.1:${NODE_A_RPC},cookie=$(cookie_file "${DATA_A}") \
    --seed b=http://127.0.0.1:${NODE_B_RPC},cookie=$(cookie_file "${DATA_B}") \
    --samples 6 \
    --height "$(get_height "${NODE_A_RPC}" "${DATA_A}")" \
    --out "${REPORT_JSON}" >/dev/null
pass "Seed consistency checker passed at shared tip"

ORPHAN_COUNT="$(count_matches "[OrphanBlockPool] Added orphan" "${LOG_B}")"
MISSING_PARENT_COUNT="$(count_matches "missing-parent: Block parent" "${LOG_B}")"
UNSOLICITED_COUNT="$(count_matches "source=UNSOLICITED" "${LOG_B}")"
DROP_COUNT="$(count_matches "DROPPING unsolicited block" "${LOG_B}")"
CATCHUP_REFRESH_COUNT="$(count_matches "Block sync still catching up; requesting headers refresh instead of routing block inv" "${LOG_B}")"

[[ "${ORPHAN_COUNT}" -eq 0 ]] || fail "Node B added orphan blocks during catch-up (${ORPHAN_COUNT})"
[[ "${MISSING_PARENT_COUNT}" -eq 0 ]] || fail "Node B hit missing-parent rejects during catch-up (${MISSING_PARENT_COUNT})"
[[ "${UNSOLICITED_COUNT}" -eq 0 ]] || fail "Node B accepted unsolicited block traffic during catch-up (${UNSOLICITED_COUNT})"
[[ "${DROP_COUNT}" -eq 0 ]] || fail "Node B had to drop unsolicited blocks during catch-up (${DROP_COUNT})"
[[ "${CATCHUP_REFRESH_COUNT}" -ge 1 ]] || fail "Catch-up header refresh path did not trigger"

pass "Lagging follower catch-up stayed ordered and orphan-free"
