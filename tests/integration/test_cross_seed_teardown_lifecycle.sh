#!/usr/bin/env bash
# Regression for #511: a cleanup helper must not return while a tracked daemon
# or an untracked datadir writer is still alive.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"

TEST_ROOT="$(mktemp -d /tmp/dinero_cross_seed_teardown.XXXXXX)"
TRACKED_DIR="${TEST_ROOT}/tracked"
STRAY_DIR="${TEST_ROOT}/stray"
TRACKED_PID=""
STRAY_PID=""
LAST_WRITER_PID=""

emergency_cleanup() {
    local rc=$?
    trap - EXIT
    set +e
    [[ -n "${TRACKED_PID}" ]] && kill -9 "${TRACKED_PID}" 2>/dev/null
    [[ -n "${STRAY_PID}" ]] && kill -9 "${STRAY_PID}" 2>/dev/null
    [[ -n "${TRACKED_PID}" ]] && wait "${TRACKED_PID}" 2>/dev/null
    [[ -n "${STRAY_PID}" ]] && wait "${STRAY_PID}" 2>/dev/null
    rm -rf "${TEST_ROOT}"
    exit "${rc}"
}
trap emergency_cleanup EXIT

start_delayed_writer() {
    local datadir="$1"
    local argv0="$2"

    mkdir -p "${datadir}"
    DATADIR="${datadir}" bash -c '
        trap "sleep 0.5; exit 0" TERM
        i=0
        : >"${DATADIR}/ready"
        while :; do
            i=$((i + 1))
            : >"${DATADIR}/write-${i}"
            sleep 0.01
        done
    ' "${argv0}" &
    LAST_WRITER_PID="$!"
}

wait_for_ready() {
    local datadir="$1"
    local deadline=$((SECONDS + 5))
    until [[ -f "${datadir}/ready" ]]; do
        (( SECONDS < deadline )) || {
            printf '[FAIL] writer did not become ready: %s\n' "${datadir}" >&2
            return 1
        }
        sleep 0.05
    done
}

# Cleanup must preserve a substantive failure, but it must also turn a clean
# test into a failure when teardown itself cannot establish quiescence.
dinero_cleanup_result 0 0
set +e
dinero_cleanup_result 0 9
cleanup_only_rc=$?
dinero_cleanup_result 7 9
test_and_cleanup_rc=$?
set -e
[[ "${cleanup_only_rc}" -eq 1 ]] || {
    printf '[FAIL] cleanup-only failure did not produce rc=1 (got %s)\n' "${cleanup_only_rc}" >&2
    exit 1
}
[[ "${test_and_cleanup_rc}" -eq 7 ]] || {
    printf '[FAIL] cleanup replaced the substantive test rc (got %s)\n' "${test_and_cleanup_rc}" >&2
    exit 1
}

read -r PORT_A PORT_B PORT_C < <(dinero_allocate_port_triplet)
[[ "${PORT_A}" =~ ^[0-9]+$ && "${PORT_B}" =~ ^[0-9]+$ && "${PORT_C}" =~ ^[0-9]+$ ]] || {
    printf '[FAIL] port allocator returned malformed values\n' >&2
    exit 1
}
[[ "${PORT_A}" != "${PORT_B}" && "${PORT_A}" != "${PORT_C}" && "${PORT_B}" != "${PORT_C}" ]] || {
    printf '[FAIL] port allocator returned duplicate values\n' >&2
    exit 1
}

start_delayed_writer "${TRACKED_DIR}" "writer-tracked"
TRACKED_PID="${LAST_WRITER_PID}"
wait_for_ready "${TRACKED_DIR}"

# Establish the original race deterministically: immediately after SIGTERM the
# writer is still alive and still owns the datadir.
kill "${TRACKED_PID}"
sleep 0.05
dinero_process_is_running "${TRACKED_PID}" || {
    printf '[FAIL] teardown race precondition was not established\n' >&2
    exit 1
}

dinero_stop_process "${TRACKED_PID}" "tracked teardown probe" 3 2
! dinero_process_is_running "${TRACKED_PID}" || {
    printf '[FAIL] tracked writer survived cleanup\n' >&2
    exit 1
}
TRACKED_PID=""
rm -rf "${TRACKED_DIR}"
[[ ! -e "${TRACKED_DIR}" ]] || {
    printf '[FAIL] tracked datadir remained after cleanup\n' >&2
    exit 1
}

# The argv string intentionally matches the production fallback search:
# dinerod.*<datadir>.  This process is not passed to the helper by PID.
start_delayed_writer "${STRAY_DIR}" "dinerod --datadir=${STRAY_DIR}"
STRAY_PID="${LAST_WRITER_PID}"
wait_for_ready "${STRAY_DIR}"
dinero_stop_datadir_processes "${STRAY_DIR}"
! dinero_process_is_running "${STRAY_PID}" || {
    printf '[FAIL] untracked datadir writer survived cleanup\n' >&2
    exit 1
}
wait "${STRAY_PID}" 2>/dev/null || true
STRAY_PID=""
rm -rf "${STRAY_DIR}"
[[ ! -e "${STRAY_DIR}" ]] || {
    printf '[FAIL] stray-writer datadir remained after cleanup\n' >&2
    exit 1
}

printf '[PASS] teardown waits for tracked and untracked datadir writers\n'
