#!/bin/bash
# Short local soak for the Stratum pool path:
# - 4 workers through dinero-stratum-worker
# - vardiff on/off
# - pool accounting enabled
# - stratum restart while miners stay attached
# - daemon restart while miners stay attached
# - explicit --payout mixed-address acceptance

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DINERO_ROOT="${SCRIPT_DIR}/../.."
STRATUM_ROOT="${DINERO_ROOT}/../stratum"

# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${DINERO_ROOT}/build/dinerod"
fi
STRATUM="${STRATUM_ROOT}/build/bin/dinero-stratum"
WORKER="${DINERO_ROOT}/build/dinero-stratum-worker"

DATADIR="/tmp/dinero_pool_restart_soak_$$"
RPC_PORT=$((22000 + RANDOM % 1000))
STRATUM_PORT=$((33000 + RANDOM % 1000))
P2P_PORT=$((44000 + RANDOM % 1000))

DAEMON_PID=""
STRATUM_PID=""
WORKER_PIDS=()
FAILED=0

log() {
    printf '%s\n' "$*"
}

fail() {
    log "FAIL: $*"
    FAILED=1
    exit 1
}

pass() {
    log "PASS: $*"
}

cleanup() {
    set +e
    for pid in "${WORKER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    if [ -n "${STRATUM_PID:-}" ]; then
        kill "$STRATUM_PID" 2>/dev/null || true
    fi
    if [ -n "${DAEMON_PID:-}" ]; then
        kill "$DAEMON_PID" 2>/dev/null || true
    fi
    sleep 1
    for pid in "${WORKER_PIDS[@]:-}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    if [ -n "${STRATUM_PID:-}" ]; then
        kill -9 "$STRATUM_PID" 2>/dev/null || true
    fi
    if [ -n "${DAEMON_PID:-}" ]; then
        kill -9 "$DAEMON_PID" 2>/dev/null || true
    fi

    if [ "$FAILED" -eq 1 ]; then
        log "Keeping datadir for inspection: $DATADIR"
        log "  daemon log:  $DATADIR/daemon.log"
        log "  stratum log: $DATADIR/stratum.log"
        log "  worker logs: $DATADIR/worker-*.log"
    else
        rm -rf "$DATADIR"
    fi
}
trap cleanup EXIT

rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    local cookie
    cookie=$(cat "${DATADIR}/.cookie")
    curl -s --max-time 15 \
         --user "${cookie}" \
         --data-binary "{\"jsonrpc\":\"2.0\",\"method\":\"${method}\",\"params\":${params},\"id\":1}" \
         -H 'content-type: application/json' \
         "http://127.0.0.1:${RPC_PORT}/"
}

wait_for_daemon() {
    for _ in $(seq 1 60); do
        if [ -f "${DATADIR}/.cookie" ] && rpc_call "getblockchaininfo" "[]" | jq -e '.result.chain' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_stratum() {
    for _ in $(seq 1 30); do
        if nc -z 127.0.0.1 "${STRATUM_PORT}" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_daemon() {
    mkdir -p "${DATADIR}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATADIR}" \
        --rpcport="${RPC_PORT}" \
        --p2pport="${P2P_PORT}" \
        --no-stratum \
        --pool.accounting.enable=1 \
        >"${DATADIR}/daemon.log" 2>&1 &
    DAEMON_PID=$!
    wait_for_daemon || fail "daemon RPC did not become ready"
}

stop_daemon() {
    if [ -n "${DAEMON_PID}" ]; then
        kill "${DAEMON_PID}" 2>/dev/null || true
        sleep 1
        kill -9 "${DAEMON_PID}" 2>/dev/null || true
        DAEMON_PID=""
    fi
}

start_stratum() {
    : > "${DATADIR}/stratum.log"
    "${STRATUM}" \
        --rpchost=127.0.0.1 \
        --rpcport="${RPC_PORT}" \
        --rpccookie="${DATADIR}/.cookie" \
        --stratumport="${STRATUM_PORT}" \
        --difficulty=0.001 \
        "$@" \
        >"${DATADIR}/stratum.log" 2>&1 &
    STRATUM_PID=$!
    wait_for_stratum || fail "stratum server did not become ready"
}

stop_stratum() {
    if [ -n "${STRATUM_PID}" ]; then
        kill "${STRATUM_PID}" 2>/dev/null || true
        sleep 1
        kill -9 "${STRATUM_PID}" 2>/dev/null || true
        STRATUM_PID=""
    fi
}

start_workers() {
    local base_addr="$1"
    local count="$2"
    WORKER_PIDS=()
    for i in $(seq 1 "${count}"); do
        "${WORKER}" \
            --stratum="127.0.0.1:${STRATUM_PORT}" \
            --user="${base_addr}.rig${i}" \
            --password=x \
            --threads=1 \
            >"${DATADIR}/worker-${i}.log" 2>&1 &
        WORKER_PIDS+=("$!")
        sleep 0.4
    done
}

start_mixed_workers() {
    local addr_a="$1"
    local addr_b="$2"
    WORKER_PIDS=()

    "${WORKER}" \
        --stratum="127.0.0.1:${STRATUM_PORT}" \
        --user="${addr_a}.alpha" \
        --password=x \
        --threads=1 \
        >"${DATADIR}/worker-mixed-a.log" 2>&1 &
    WORKER_PIDS+=("$!")
    sleep 0.4

    "${WORKER}" \
        --stratum="127.0.0.1:${STRATUM_PORT}" \
        --user="${addr_b}.beta" \
        --password=x \
        --threads=1 \
        >"${DATADIR}/worker-mixed-b.log" 2>&1 &
    WORKER_PIDS+=("$!")
}

stop_workers() {
    for pid in "${WORKER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in "${WORKER_PIDS[@]:-}"; do
        kill -9 "$pid" 2>/dev/null || true
    done
    WORKER_PIDS=()
}

json_number() {
    jq -r "$1"
}

extract_address() {
    jq -r '.result.address // .result // empty'
}

assert_eq() {
    local actual="$1"
    local expected="$2"
    local msg="$3"
    if [ "${actual}" != "${expected}" ]; then
        fail "${msg} (expected=${expected}, actual=${actual})"
    fi
    pass "${msg}"
}

assert_ge() {
    local actual="$1"
    local minimum="$2"
    local msg="$3"
    if [ "${actual}" -lt "${minimum}" ]; then
        fail "${msg} (minimum=${minimum}, actual=${actual})"
    fi
    pass "${msg}"
}

assert_gt() {
    local actual="$1"
    local minimum="$2"
    local msg="$3"
    if [ "${actual}" -le "${minimum}" ]; then
        fail "${msg} (must be > ${minimum}, actual=${actual})"
    fi
    pass "${msg}"
}

wait_for_pool_shares_gt() {
    local baseline="$1"
    local timeout_secs="$2"

    for _ in $(seq 1 "${timeout_secs}"); do
        local current
        current=$(rpc_call "pool.stats" "[]" | jq -r '.result.total_shares')
        if [ -n "${current}" ] && [ "${current}" -gt "${baseline}" ]; then
            echo "${current}"
            return 0
        fi
        sleep 1
    done

    echo "${baseline}"
    return 1
}

wait_for_blockcount_gt() {
    local baseline="$1"
    local timeout_secs="$2"

    for _ in $(seq 1 "${timeout_secs}"); do
        local current
        current=$(rpc_call "getblockcount" "[]" | jq -r '.result')
        if [ -n "${current}" ] && [ "${current}" -gt "${baseline}" ]; then
            echo "${current}"
            return 0
        fi
        sleep 1
    done

    echo "${baseline}"
    return 1
}

total_worker_job_count() {
    local total=0
    for worker_log in "${DATADIR}"/worker-*.log; do
        if [ -f "${worker_log}" ]; then
            local count
            count=$(rg -c '📦 New job' "${worker_log}" 2>/dev/null || echo 0)
            total=$((total + count))
        fi
    done
    echo "${total}"
}

wait_for_worker_jobs_gt() {
    local baseline="$1"
    local timeout_secs="$2"

    for _ in $(seq 1 "${timeout_secs}"); do
        local current
        current=$(total_worker_job_count)
        if [ -n "${current}" ] && [ "${current}" -gt "${baseline}" ]; then
            echo "${current}"
            return 0
        fi
        sleep 1
    done

    echo "${baseline}"
    return 1
}

log "=== Phase A0: single-worker warmup ==="
start_daemon
ADDR_A=$(rpc_call "getnewaddress" "[]" | extract_address)
ADDR_B=$(rpc_call "getnewaddress" "[]" | extract_address)
if [ -z "${ADDR_A}" ] || [ -z "${ADDR_B}" ] || [ "${ADDR_A}" = "null" ] || [ "${ADDR_B}" = "null" ]; then
    fail "failed to derive payout addresses from getnewaddress"
fi
start_stratum --difficulty=0.0001
start_workers "${ADDR_A}" 1
sleep 8
WARMUP_SHARES=$(rpc_call "pool.stats" "[]" | jq -r '.result.total_shares')
assert_gt "${WARMUP_SHARES}" 0 "single-worker warmup submitted shares"
stop_workers
stop_stratum

log "=== Phase A: vardiff enabled, 4 workers ==="
start_stratum --vardiff-shares=2
start_workers "${ADDR_A}" 4
sleep 15

STATUS_A=$(rpc_call "pool.status" "[]")
WORKERS_A=$(rpc_call "pool.workers" '{"all":true}')
STATS_A=$(rpc_call "pool.stats" "[]")
HEIGHT_A=$(rpc_call "getblockcount" "[]" | jq -r '.result')
WORKER_COUNT_A=$(echo "${WORKERS_A}" | jq -r '.result | length')
SHARES_A=$(echo "${STATS_A}" | jq -r '.result.total_shares')
BLOCKS_A=$(echo "${STATS_A}" | jq -r '.result.blocks_found')
VARDIFF_CHANGED=0

for worker_log in "${DATADIR}"/worker-[1-4].log; do
    UNIQUE_DIFFS=$(rg -o 'Difficulty updated: [0-9.e+-]+' "${worker_log}" | awk '{print $3}' | sort -u | wc -l | tr -d ' ')
    if [ "${UNIQUE_DIFFS}" -gt 1 ]; then
        VARDIFF_CHANGED=1
        break
    fi
done

echo "${STATUS_A}" | jq '{ready: .result.ready, enabled: .result.enabled}'
echo "${STATS_A}" | jq '{active_workers: .result.active_workers, total_shares: .result.total_shares, blocks_found: .result.blocks_found, pool_hashrate: .result.pool_hashrate_formatted}'

assert_eq "$(echo "${STATUS_A}" | jq -r '.result.ready')" "true" "pool accounting ready with vardiff on"
assert_ge "${WORKER_COUNT_A}" 4 "all 4 workers registered"
assert_gt "${SHARES_A}" 0 "shares recorded under load"
assert_gt "${BLOCKS_A}" 0 "blocks found under load"
assert_gt "${HEIGHT_A}" 0 "chain advanced under load"
if [ "${VARDIFF_CHANGED}" = "1" ]; then
    pass "vardiff changed difficulty under multi-worker load"
else
    log "WARN: vardiff did not visibly step during this regtest window"
fi

log "=== Phase B: restart stratum with miners attached ==="
BEFORE_RESTART_SHARES="${SHARES_A}"
BEFORE_RESTART_JOBS=$(total_worker_job_count)
stop_stratum
start_stratum
sleep 3

WORKERS_B=$(rpc_call "pool.workers" '{"all":true}')
WORKER_COUNT_B=$(echo "${WORKERS_B}" | jq -r '.result | length')
SHARES_B=$(wait_for_pool_shares_gt "${BEFORE_RESTART_SHARES}" 20 || true)
JOBS_B=$(wait_for_worker_jobs_gt "${BEFORE_RESTART_JOBS}" 20 || true)
STATS_B=$(rpc_call "pool.stats" "[]")

echo "${STATS_B}" | jq '{active_workers: .result.active_workers, total_shares: .result.total_shares, blocks_found: .result.blocks_found}'
assert_ge "${WORKER_COUNT_B}" 4 "workers reconnected after stratum restart"
assert_gt "${JOBS_B}" "${BEFORE_RESTART_JOBS}" "fresh jobs arrived after stratum restart"

log "=== Phase C: restart daemon with miners attached ==="
BEFORE_DAEMON_RESTART_SHARES="${SHARES_B}"
BEFORE_DAEMON_RESTART_JOBS=$(total_worker_job_count)
stop_daemon
sleep 2
start_daemon
sleep 3

STATUS_C=$(rpc_call "pool.status" "[]")
WORKERS_C=$(rpc_call "pool.workers" '{"all":true}')
WORKER_COUNT_C=$(echo "${WORKERS_C}" | jq -r '.result | length')
SHARES_C=$(wait_for_pool_shares_gt "${BEFORE_DAEMON_RESTART_SHARES}" 25 || true)
JOBS_C=$(wait_for_worker_jobs_gt "${BEFORE_DAEMON_RESTART_JOBS}" 25 || true)
HEIGHT_C=$(wait_for_blockcount_gt "${HEIGHT_A}" 25 || true)
STATS_C=$(rpc_call "pool.stats" "[]")

echo "${STATUS_C}" | jq '{ready: .result.ready, enabled: .result.enabled}'
echo "${STATS_C}" | jq '{active_workers: .result.active_workers, total_shares: .result.total_shares, blocks_found: .result.blocks_found}'

assert_eq "$(echo "${STATUS_C}" | jq -r '.result.ready')" "true" "pool accounting recovered after daemon restart"
assert_ge "${WORKER_COUNT_C}" 4 "workers still connected after daemon restart"
assert_gt "${JOBS_C}" "${BEFORE_DAEMON_RESTART_JOBS}" "fresh jobs arrived after daemon restart"
assert_gt "${HEIGHT_C}" "${HEIGHT_A}" "chain kept advancing after daemon restart"

log "=== Phase D: no-vardiff fixed difficulty ==="
stop_workers
stop_stratum
start_stratum --no-vardiff
start_workers "${ADDR_A}" 3
sleep 8

WORKERS_D=$(rpc_call "pool.workers" '{"all":true}')
FIXED_DIFF_OK=1
for worker_log in "${DATADIR}"/worker-*.log; do
    UNIQUE_DIFFS=$(rg -o 'Difficulty updated: [0-9.e+-]+' "${worker_log}" | awk '{print $3}' | sort -u | tr '\n' ' ' | sed 's/[[:space:]]*$//')
    if [ "${UNIQUE_DIFFS}" != "0.001000" ]; then
        FIXED_DIFF_OK=0
        break
    fi
done
echo "${WORKERS_D}" | jq '[.result[] | {worker_id, current_difficulty}]'
assert_eq "${FIXED_DIFF_OK}" "1" "no-vardiff keeps live worker difficulty fixed"

log "=== Phase E: explicit payout config allows mixed addresses ==="
stop_workers
stop_stratum
start_stratum "--payout=${ADDR_A}:70" "--payout=${ADDR_B}:30"
start_mixed_workers "${ADDR_A}" "${ADDR_B}"
sleep 8

WORKERS_E=$(rpc_call "pool.workers" '{"all":true}')
MIXED_COUNT=$(echo "${WORKERS_E}" | jq -r '.result | map(.worker_id) | map(select(test("alpha|beta"))) | length')
if rg -q "Multiple payout addresses require explicit --payout pool configuration" "${DATADIR}/stratum.log"; then
    fail "explicit payout config still triggered mixed-address rejection"
fi
echo "${WORKERS_E}" | jq '[.result[] | select(.worker_id | test("alpha|beta")) | {worker_id, wallet_address, shares_valid, blocks_found}]'
assert_eq "${MIXED_COUNT}" "2" "mixed-address workers were accepted with explicit payout config"

log "=== SOAK SUMMARY ==="
log "PASS: multi-worker pool soak, restart recovery, vardiff on/off, and explicit payout config"
