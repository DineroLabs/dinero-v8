#!/usr/bin/env bash
# Shared process-lifecycle helpers for daemon-spawning integration tests.
#
# Sending a signal is not the same as stopping a process.  Callers must not
# remove a daemon's datadir until these helpers confirm that every writer has
# exited (and reap it when it is a child of the current shell).

dinero_process_is_running() {
    local pid="$1"
    local state

    kill -0 "${pid}" 2>/dev/null || return 1
    state="$(ps -o stat= -p "${pid}" 2>/dev/null || true)"
    # If `ps` races or cannot report a state, remain conservative: kill -0
    # still says the PID exists, so do not declare the writer gone.
    [[ -z "${state}" || "${state}" != *Z* ]]
}

dinero_wait_for_process_exit() {
    local pid="$1"
    local timeout_seconds="${2:-10}"
    local deadline=$((SECONDS + timeout_seconds))

    while dinero_process_is_running "${pid}"; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done

    # Reap the process when it belongs to this shell.  `wait` returns 127 for
    # non-child processes; either result is fine once the PID is no longer
    # running.
    wait "${pid}" 2>/dev/null || true
    return 0
}

dinero_report_process() {
    local pid="$1"
    local label="$2"

    printf '[CLEANUP] surviving %s process: ' "${label}" >&2
    ps -p "${pid}" -o pid=,ppid=,stat=,command= >&2 2>/dev/null || \
        printf 'pid=%s (process details unavailable)\n' "${pid}" >&2
}

dinero_stop_process() {
    local pid="$1"
    local label="${2:-daemon}"
    local term_timeout="${3:-30}"
    local kill_timeout="${4:-5}"

    [[ -n "${pid}" ]] || return 0
    if ! dinero_process_is_running "${pid}"; then
        wait "${pid}" 2>/dev/null || true
        return 0
    fi

    kill "${pid}" 2>/dev/null || true
    if dinero_wait_for_process_exit "${pid}" "${term_timeout}"; then
        return 0
    fi

    printf '[CLEANUP] %s pid %s ignored SIGTERM; escalating to SIGKILL\n' \
        "${label}" "${pid}" >&2
    kill -9 "${pid}" 2>/dev/null || true
    if dinero_wait_for_process_exit "${pid}" "${kill_timeout}"; then
        return 0
    fi

    dinero_report_process "${pid}" "${label}"
    return 1
}

dinero_find_datadir_processes() {
    local datadir="$1"
    local pid
    local command

    while IFS= read -r pid; do
        [[ -n "${pid}" ]] || continue
        [[ "${pid}" = "$$" || "${pid}" = "${BASHPID:-$$}" ]] && continue
        command="$(ps -p "${pid}" -o command= 2>/dev/null || true)"
        [[ "${command}" = *"${datadir}"* ]] || continue
        printf '%s\n' "${pid}"
    done < <(pgrep -f '[d]inerod' 2>/dev/null || true)
}

dinero_stop_datadir_processes() {
    local datadir="$1"
    local rc=0
    local pid
    local pids=""

    while IFS= read -r pid; do
        [[ -n "${pid}" ]] && pids+="${pid} "
    done < <(dinero_find_datadir_processes "${datadir}")

    for pid in ${pids}; do
        dinero_stop_process "${pid}" "dinerod for ${datadir}" || rc=1
    done

    # A helper could have forked while its parent was shutting down.  Refuse
    # to claim quiescence if any process still names the datadir.
    pids=""
    while IFS= read -r pid; do
        [[ -n "${pid}" ]] && pids+="${pid} "
    done < <(dinero_find_datadir_processes "${datadir}")
    if [[ -n "${pids}" ]]; then
        rc=1
        for pid in ${pids}; do
            dinero_report_process "${pid}" "dinerod for ${datadir}"
        done
    fi

    return "${rc}"
}

dinero_cleanup_result() {
    local test_rc="$1"
    local cleanup_rc="$2"

    # Preserve an existing test failure.  A cleanup failure only supplies the
    # verdict when the substantive test itself passed.
    if (( test_rc != 0 )); then
        return "${test_rc}"
    fi
    if (( cleanup_rc != 0 )); then
        return 1
    fi
    return 0
}
