#!/usr/bin/env bash
# Shared failure-diagnostic capture for daemon-spawning integration tests.
#
# A harness that dumps `tail -N daemon.log` on failure destroys its own
# evidence whenever the failure is detected by a POLL LOOP: the loop logs
# while it waits, so by the time the timeout fires, the N-line window holds
# only the polling and the action under test has scrolled off.
#
# Observed case (issue #538): a bridge reorg failure produced a 123-line dump
# of which 115 lines were `[RPC DEBUG] getblockcount`, with zero matches for
# REORG|invalidate|Disconnect|rewind.  The diagnostic failed under exactly the
# condition it exists to diagnose.
#
# Two independent defects, so two independent guards:
#   1. WINDOW  — anchor the capture at the action (dinero_log_mark) instead of
#                at the end of the file.
#   2. VOLUME  — filter known chatter, and grep the whole file for signal, so
#                a noisy wait cannot evict the evidence even inside a window.
#
# Sourcing this file has no side effects.

# Lines that carry no diagnostic value but are emitted per poll iteration.
# Callers may extend this before sourcing, or override it afterwards.
: "${DINERO_LOG_NOISE_RE:=^\[RPC DEBUG\]}"

# Lines worth surfacing regardless of where they landed in the file.
: "${DINERO_REORG_SIGNAL_RE:=REORG|reorg|invalidate|Disconnect|disconnect|rewind|ActivateBestChain|ABC-|EARLY RETURN|FATAL|safe mode}"

# Byte offset to start a later failure dump from.  Record this BEFORE the
# action under test, so the dump cannot be pushed out by whatever the
# detection loop logs while it waits.  Prints 0 when the log does not exist
# yet, which makes an unmarked caller degrade to a whole-file dump rather
# than to an empty one.
dinero_log_mark() {
    local logfile="$1"

    if [[ ! -f "${logfile}" ]]; then
        printf '0\n'
        return 0
    fi
    wc -c < "${logfile}" 2>/dev/null | tr -d '[:space:]' || printf '0\n'
}

# Dump the log from a recorded mark forward, with chatter removed.
#
# Takes the HEAD of that slice, not the tail: the action under test sits at
# the START of the post-mark region, so tailing here would reintroduce the
# exact eviction this helper exists to prevent.
dinero_dump_log_from_mark() {
    local logfile="$1"
    local mark="${2:-0}"
    local label="${3:-daemon}"
    local max_lines="${4:-200}"
    local size slice

    printf '\n=== %s log, from the marked action onward (max %s lines, poll chatter filtered) ===\n' \
        "${label}" "${max_lines}"

    if [[ ! -f "${logfile}" ]]; then
        printf '(no log file at %s)\n' "${logfile}"
        return 0
    fi

    # A rotated or truncated log invalidates the offset.  Falling back to the
    # whole file is noisy; silently printing nothing would look like "the
    # daemon logged nothing after the action", which is a different and much
    # more misleading claim.
    size="$(dinero_log_mark "${logfile}")"
    if [[ -n "${size}" ]] && (( size < mark )); then
        printf '(log shrank from %s to %s bytes — rotated or truncated; dumping from the start)\n' \
            "${mark}" "${size}"
        mark=0
    fi

    slice="$(tail -c "+$((mark + 1))" "${logfile}" 2>/dev/null | grep -Ev "${DINERO_LOG_NOISE_RE}" || true)"

    if [[ -z "${slice}" ]]; then
        printf '(nothing after the mark survived the chatter filter)\n'
        return 0
    fi

    printf '%s\n' "${slice}" | head -n "${max_lines}"
}

# Whole-file grep for diagnostically interesting lines.
#
# Backstop for the case where the mark is missing, wrong, or the evidence was
# written before it.  Matches are rare by construction, so tailing them is
# safe here — unlike tailing the raw log.
dinero_dump_log_matches() {
    local logfile="$1"
    local pattern="${2:-${DINERO_REORG_SIGNAL_RE}}"
    local label="${3:-daemon}"
    local max="${4:-40}"
    local matches

    printf '\n=== %s log, signal lines anywhere in the file (last %s) ===\n' "${label}" "${max}"

    if [[ ! -f "${logfile}" ]]; then
        printf '(no log file at %s)\n' "${logfile}"
        return 0
    fi

    matches="$(grep -nE "${pattern}" "${logfile}" 2>/dev/null | tail -n "${max}" || true)"
    if [[ -z "${matches}" ]]; then
        # An affirmative negative.  "No matches" and "never grepped" produce
        # the same empty output otherwise, and they mean different things.
        printf '(no lines matched %s — the daemon never reported one)\n' "${pattern}"
        return 0
    fi
    printf '%s\n' "${matches}"
}

# Convenience wrapper: both guards for one daemon log.
dinero_dump_failure_log() {
    local logfile="$1"
    local mark="${2:-0}"
    local label="${3:-daemon}"
    local max_lines="${4:-200}"

    dinero_dump_log_from_mark "${logfile}" "${mark}" "${label}" "${max_lines}"
    dinero_dump_log_matches "${logfile}" "${DINERO_REORG_SIGNAL_RE}" "${label}"
}
