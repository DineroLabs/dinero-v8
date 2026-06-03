#!/usr/bin/env bash
# Integration repro for issue #214 — in-daemon staleness recovery.
#
# Reproduces the REAL stall and proves the fix end-to-end over the wire, with
# two real regtest dinerod processes:
#
#   - Node B (the ahead peer) runs with DINERO_TEST_SUPPRESS_ANNOUNCEMENTS=1, a
#     regtest-only hook that suppresses the spontaneous block PUSH (cmpctblock/inv
#     in BlockRelayManager::AnnounceBlock). B still ANSWERS getheaders/getdata —
#     only the unsolicited announcement is dropped. This is exactly the field
#     symptom: a peer mines ahead but its announcements stop reaching us.
#   - Node A (under test) first syncs to height H via the normal on-connect
#     getheaders PULL (suppression does not block pulls), so its header tip is
#     > 0 (the recovery has a height-0 guard). Then B mines N more blocks that
#     it never announces, freezing A's header tip with B still connected.
#
# Two scenarios make the contrast the proof:
#   CONTROL    — A's staleness recovery effectively OFF (threshold = 1 day):
#                A must STAY stuck at H, and must NOT log a recovery. This proves
#                the suppression genuinely induces the stall and nothing else
#                rescues A (no pre-existing periodic getheaders).
#   EXPERIMENT — A's recovery threshold shrunk to a few seconds:
#                A must re-issue getheaders (log line), pull the missing headers,
#                and catch up to H+N — recovered in-daemon, no external watchdog.
#
# Assertions are layered so a failure localizes the broken link:
#   1. log "Stale tip: ... re-issued getheaders"  -> the #214 fix FIRED
#   2. headers tip advances H -> H+N              -> the pull SUCCEEDED (fix effect)
#   3. blocks tip advances H -> H+N               -> end-to-end catch-up (bonus)
#
# Timing note: MaybeRecoverStaleTip is only CALLED on the ~5s scheduler tick, so
# with an 8s threshold it fires on the first tick past 8s (~10s). All waits poll
# (wait_condition), never fixed sleeps tuned to the threshold.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
if [[ ! -x "${DINEROD}" && -x "${ROOT_DIR}/build-release/dinerod" ]]; then
    DINEROD="${ROOT_DIR}/build-release/dinerod"
fi

PRESYNC_H=5          # height A reaches via on-connect pull before the stall
EXTRA_N=5            # blocks B mines (and hides) after A is synced -> target H+N
TARGET_H=$((PRESYNC_H + EXTRA_N))

EXP_THRESHOLD_SECS=8        # experiment: recovery fires ~10s (first tick past 8s)
CONTROL_THRESHOLD_SECS=86400  # control: recovery effectively disabled (1 day)
GETHEADERS_INTERVAL_SECS=2

PIDS=()
DIRS=()
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for d in "${DIRS[@]}"; do
        [[ -f "${d}.log" ]] && { printf -- '--- %s log tail ---\n' "${d}" >&2; tail -60 "${d}.log" >&2 || true; }
    done
    exit 1
}
cleanup() {
    for p in "${PIDS[@]}"; do kill "${p}" 2>/dev/null || true; done
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        for d in "${DIRS[@]}"; do rm -rf "${d}" "${d}.log"; done
    fi
}
trap cleanup EXIT

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then printf '%s\n' "${datadir}/.cookie"; return 0; fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then printf '%s\n' "${datadir}/regtest/.cookie"; return 0; fi
    return 1
}

rpc_call() {
    local rpc_port="$1" datadir="$2" method="$3" params_json="$4"
    local cookie_path cookie
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${rpc_port}/"
}

# Convenience: read a numeric field of getblockchaininfo (.blocks or .headers).
chain_field() {
    local rpc_port="$1" datadir="$2" field="$3"
    rpc_call "${rpc_port}" "${datadir}" "getblockchaininfo" '[]' \
        | jq -r ".result.${field} // -1" 2>/dev/null || echo -1
}

wait_rpc() {
    local rpc_port="$1" datadir="$2"
    for _ in $(seq 1 60); do
        if rpc_call "${rpc_port}" "${datadir}" "getblockcount" '[]' | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_condition() {
    local cmd="$1" message="$2" tries="${3:-60}"
    for _ in $(seq 1 "${tries}"); do
        if eval "${cmd}"; then return 0; fi
        sleep 1
    done
    fail "${message}"
}

# start_node DATADIR RPC_PORT P2P_PORT [ENV_KV ...] -> echoes PID
start_node() {
    local datadir="$1" rpc_port="$2" p2p_port="$3"; shift 3
    mkdir -p "${datadir}"
    env "$@" "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        >"${datadir}.log" 2>&1 &
    printf '%s\n' "$!"
}

# run_scenario NAME BASE_PORT A_THRESHOLD_SECS MODE(recover|stuck)
run_scenario() {
    local name="$1" base="$2" a_threshold="$3" mode="$4"
    local a_rpc=$((base + 0)) b_rpc=$((base + 1))
    local a_p2p=$((base + 100)) b_p2p=$((base + 101))
    local data_a="/tmp/dinero_stale_a_${name}_$$" data_b="/tmp/dinero_stale_b_${name}_$$"

    info "=== scenario '${name}' (mode=${mode}, A threshold=${a_threshold}s) ==="

    # Node A (under test): shrunk/raised staleness clock.
    local pid_a
    pid_a="$(start_node "${data_a}" "${a_rpc}" "${a_p2p}" \
        "DINERO_TEST_STALENESS_THRESHOLD_SECS=${a_threshold}" \
        "DINERO_TEST_STALENESS_GETHEADERS_INTERVAL_SECS=${GETHEADERS_INTERVAL_SECS}")"
    PIDS+=("${pid_a}"); DIRS+=("${data_a}")

    # Node B (ahead peer): suppress spontaneous block announcements.
    local pid_b
    pid_b="$(start_node "${data_b}" "${b_rpc}" "${b_p2p}" \
        "DINERO_TEST_SUPPRESS_ANNOUNCEMENTS=1")"
    PIDS+=("${pid_b}"); DIRS+=("${data_b}")

    wait_rpc "${a_rpc}" "${data_a}" || fail "[${name}] Node A RPC did not come up"
    wait_rpc "${b_rpc}" "${data_b}" || fail "[${name}] Node B RPC did not come up"

    # Create a wallet on B and mine to its (taproot) address. createhd returns the
    # first address directly, so one call gives us a valid regtest coinbase target.
    local mine_addr
    mine_addr="$(rpc_call "${b_rpc}" "${data_b}" "wallet.createhd" '["miner"]' | jq -r '.result.first_address // empty')"
    [[ -n "${mine_addr}" ]] || fail "[${name}] could not create wallet/address on B"

    # B mines to PRESYNC_H (announcements suppressed, but nobody's listening yet).
    rpc_call "${b_rpc}" "${data_b}" "generatetoaddress" "[${PRESYNC_H},\"${mine_addr}\"]" >/dev/null
    wait_condition "[[ \$(chain_field ${b_rpc} ${data_b} blocks) -eq ${PRESYNC_H} ]]" \
        "[${name}] Node B did not mine to ${PRESYNC_H}"

    # A dials B; on-connect getheaders PULL syncs A to PRESYNC_H despite suppression.
    rpc_call "${a_rpc}" "${data_a}" "addnode" "[\"127.0.0.1:${b_p2p}\",\"onetry\"]" >/dev/null
    wait_condition "[[ \$(rpc_call ${a_rpc} ${data_a} getconnectioncount '[]' | jq -r '.result // 0') -ge 1 ]]" \
        "[${name}] Node A never connected to node B"
    wait_condition "[[ \$(chain_field ${a_rpc} ${data_a} blocks) -eq ${PRESYNC_H} ]]" \
        "[${name}] Node A did not initial-sync to ${PRESYNC_H} via on-connect pull"
    pass "[${name}] A synced to ${PRESYNC_H} via pull; B connected (suppression does not block pulls)"

    # The stall: B mines EXTRA_N more and hides them. A's tip should freeze at H.
    rpc_call "${b_rpc}" "${data_b}" "generatetoaddress" "[${EXTRA_N},\"${mine_addr}\"]" >/dev/null
    wait_condition "[[ \$(chain_field ${b_rpc} ${data_b} blocks) -eq ${TARGET_H} ]]" \
        "[${name}] Node B did not mine to ${TARGET_H}"
    info "[${name}] B is at ${TARGET_H}; A is at $(chain_field ${a_rpc} ${data_a} blocks) (headers=$(chain_field ${a_rpc} ${data_a} headers))"

    if [[ "${mode}" == "recover" ]]; then
        # EXPERIMENT: recovery must fire, headers must catch up, blocks must follow.
        wait_condition "[[ \$(chain_field ${a_rpc} ${data_a} headers) -eq ${TARGET_H} ]]" \
            "[${name}] A header tip did not recover to ${TARGET_H} (the #214 fix did not advance headers)"
        grep -q "Stale tip" "${data_a}.log" \
            || fail "[${name}] A caught up but logged no 'Stale tip ... re-issued getheaders' — recovery was not the cause"
        pass "[${name}] A re-issued getheaders and recovered header tip to ${TARGET_H}"
        wait_condition "[[ \$(chain_field ${a_rpc} ${data_a} blocks) -eq ${TARGET_H} ]]" \
            "[${name}] A header tip recovered but block tip did not reach ${TARGET_H} (downstream block-download, not #214)"
        pass "[${name}] A caught up end-to-end to ${TARGET_H} (blocks) — recovered in-daemon, no watchdog"
    else
        # CONTROL: recovery off -> A must stay stuck at H for a multi-tick window.
        local stuck_window=18  # >= 3 scheduler ticks
        info "[${name}] holding ${stuck_window}s to confirm A stays stuck (recovery disabled)..."
        sleep "${stuck_window}"
        local a_blocks a_headers
        a_blocks="$(chain_field ${a_rpc} ${data_a} blocks)"
        a_headers="$(chain_field ${a_rpc} ${data_a} headers)"
        [[ "${a_blocks}" -eq "${PRESYNC_H}" ]] \
            || fail "[${name}] A advanced to ${a_blocks} with recovery OFF — suppression leaked or another path rescued A"
        [[ "${a_headers}" -eq "${PRESYNC_H}" ]] \
            || fail "[${name}] A header tip advanced to ${a_headers} with recovery OFF — suppression leaked"
        grep -q "Stale tip" "${data_a}.log" \
            && fail "[${name}] A logged a recovery despite the 1-day threshold — override not applied"
        pass "[${name}] A stayed stuck at ${PRESYNC_H} (blocks+headers), no recovery — stall is real, nothing else rescues A"
    fi

    # Per-scenario teardown so ports/datadirs are clean for the next one.
    kill "${pid_a}" "${pid_b}" 2>/dev/null || true
    sleep 1
    rm -rf "${data_a}" "${data_b}" "${data_a}.log" "${data_b}.log"
}

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

# Control first (proves the stall is real), then the experiment (proves the fix).
run_scenario "control"    35720 "${CONTROL_THRESHOLD_SECS}" stuck
run_scenario "experiment" 35740 "${EXP_THRESHOLD_SECS}"     recover

echo "P2P_STALENESS_RECOVERY=PASS"
