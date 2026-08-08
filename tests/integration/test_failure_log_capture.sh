#!/usr/bin/env bash
#
# Regression guard for issue #538: a failure diagnostic must not be evicted by
# the poll chatter of the loop that detected the failure.
#
# Pure shell — spawns no daemon, binds no port, runs in well under a second.
# It is therefore deliberately NOT 'integration'-labeled in CMakeLists.txt, so
# it executes in the main ctest lane rather than needing an exact-name carve-out
# in the workflow.
#
# The first assertion is the PREMISE check: it proves the old `tail -N`
# behaviour really does lose the evidence under these inputs.  Without it, the
# rest of the test could pass against a window that was never actually too
# small, and the guard would have no teeth.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=helpers/failure_log_capture.sh
source "${SCRIPT_DIR}/helpers/failure_log_capture.sh"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

FAILURES=0
CHECKS=0

check() {
    local desc="$1"
    CHECKS=$((CHECKS + 1))
    if "${@:2}"; then
        echo -e "${GREEN}  ok  ${NC}${desc}"
    else
        echo -e "${RED}  FAIL${NC} ${desc}"
        FAILURES=$((FAILURES + 1))
    fi
}

contains()     { grep -q "$2" <<<"$1"; }
not_contains() { ! grep -q "$2" <<<"$1"; }

TMPDIR_TEST="$(mktemp -d)"
trap 'rm -rf "${TMPDIR_TEST}"' EXIT

LOG="${TMPDIR_TEST}/daemon.log"
EVIDENCE='[ABC] REORG detected: fork=41 disconnect=2 rewind to 0000dead'
CHATTER='[RPC DEBUG] Calling handler for method: getblockcount'

emit_chatter() {
    local n="$1" i
    for ((i = 0; i < n; i++)); do
        printf '%s\n' "${CHATTER}" >> "${LOG}"
        printf '[RPC DEBUG] handler_ptr address: 0x10f4c2a80\n' >> "${LOG}"
        printf '[RPC DEBUG] daemon_context_ address: 0x7fd2c1004e00\n' >> "${LOG}"
        printf '[RPC DEBUG] Handler completed successfully for: getblockcount\n' >> "${LOG}"
    done
}

echo "=== Failure-log capture regression (#538) ==="

# --- Scenario: the shape of the real failure -------------------------------
# Startup history, then the action under test, then a 60s poll loop's worth of
# debug output while the harness waits for a rewind that never comes.
emit_chatter 40
MARK="$(dinero_log_mark "${LOG}")"
printf '%s\n' "${EVIDENCE}" >> "${LOG}"
emit_chatter 150

echo
echo "-- premise: the old tail-window behaviour loses the evidence"
OLD_DUMP="$(tail -120 "${LOG}")"
check "tail -120 does NOT contain the reorg evidence (eviction is real)" \
    not_contains "${OLD_DUMP}" "REORG detected"

echo
echo "-- guard 1 (window): anchored dump keeps the evidence"
NEW_DUMP="$(dinero_dump_log_from_mark "${LOG}" "${MARK}" "Bridge")"
check "anchored dump contains the reorg evidence" \
    contains "${NEW_DUMP}" "REORG detected"
check "anchored dump filters the poll chatter" \
    not_contains "${NEW_DUMP}" "RPC DEBUG"

echo
echo "-- guard 1 teeth: anchoring must survive volume the chatter filter does NOT remove"
# The scenario above is satisfied by the noise filter alone, so it cannot tell
# head-of-slice from tail-of-slice.  This one can: the post-mark region is
# filled with lines the filter keeps, so only taking the HEAD of the slice
# preserves the evidence.  Swap the head for a tail and this check goes red.
VOL_LOG="${TMPDIR_TEST}/volume.log"
printf 'startup line\n' > "${VOL_LOG}"
VOL_MARK="$(dinero_log_mark "${VOL_LOG}")"
printf '%s\n' "${EVIDENCE}" >> "${VOL_LOG}"
for ((i = 0; i < 300; i++)); do
    printf '[Mempool] accepted tx %d into the pool\n' "${i}" >> "${VOL_LOG}"
done
VOL_DUMP="$(dinero_dump_log_from_mark "${VOL_LOG}" "${VOL_MARK}" "Volume" 50)"
check "anchored dump keeps evidence buried under 300 unfilterable lines" \
    contains "${VOL_DUMP}" "REORG detected"
check "anchored dump respects the line cap" \
    test "$(printf '%s\n' "${VOL_DUMP}" | grep -c 'accepted tx')" -le 50

echo
echo "-- guard 2 (volume): whole-file signal grep keeps the evidence"
MATCH_DUMP="$(dinero_dump_log_matches "${LOG}" "${DINERO_REORG_SIGNAL_RE}" "Bridge")"
check "signal grep finds the reorg evidence regardless of position" \
    contains "${MATCH_DUMP}" "REORG detected"

echo
echo "-- affirmative negative: a silent log says so, rather than printing nothing"
QUIET_LOG="${TMPDIR_TEST}/quiet.log"
emit_chatter 5 2>/dev/null || true
printf '%s\n' "${CHATTER}" > "${QUIET_LOG}"
QUIET_DUMP="$(dinero_dump_log_matches "${QUIET_LOG}" "${DINERO_REORG_SIGNAL_RE}" "Quiet")"
check "no-match case reports 'never reported one' instead of empty output" \
    contains "${QUIET_DUMP}" "the daemon never reported one"

echo
echo "-- robustness: a rotated log must not silently produce an empty dump"
ROTATED="${TMPDIR_TEST}/rotated.log"
emit_chatter 1
printf '%s\n' "${EVIDENCE}" > "${ROTATED}"
STALE_MARK=999999
ROTATED_DUMP="$(dinero_dump_log_from_mark "${ROTATED}" "${STALE_MARK}" "Rotated")"
check "stale mark falls back to a full dump" \
    contains "${ROTATED_DUMP}" "REORG detected"
check "stale mark says why it fell back" \
    contains "${ROTATED_DUMP}" "rotated or truncated"

echo
echo "-- missing log is reported, not silently skipped"
MISSING_DUMP="$(dinero_dump_log_from_mark "${TMPDIR_TEST}/nope.log" 0 "Absent")"
check "absent log file is named in the output" \
    contains "${MISSING_DUMP}" "no log file at"

echo
echo "-- source tripwire: no ungated per-RPC debug print may return to the daemon"
# This is a SOURCE-LEVEL scan, not a behavioural test: it cannot prove the gate
# works at runtime, only that nobody has re-added an ungated per-request print.
# Counts are echoed affirmatively for the same reason the #490 seed pin runs
# verbosely — "found nothing" and "never scanned" otherwise look identical.
RPC_SERVER="${SCRIPT_DIR}/../../src/daemon/http_rpc_server.cpp"
if [[ -f "${RPC_SERVER}" ]]; then
    DEBUG_PRINTS="$(grep -c 'std::cout << "\[RPC DEBUG\]' "${RPC_SERVER}" || true)"
    GATE_BLOCKS="$(grep -c 'if (rpc_debug_enabled)' "${RPC_SERVER}" || true)"
    GATED_DEBUG_PRINTS="$(awk '
        /if \(rpc_debug_enabled\)/ { in_gate = 1; depth = 0 }
        in_gate {
            line = $0
            opens = gsub(/\{/, "{", line)
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            if ($0 ~ /std::cout << "\[RPC DEBUG\]/) gated++
            if (depth == 0) in_gate = 0
        }
        END { print gated + 0 }
    ' "${RPC_SERVER}")"
    GATE_DECL="$(grep -c 'DINERO_RPC_DEBUG' "${RPC_SERVER}" || true)"
    echo "     [RPC DEBUG] cout statements: ${DEBUG_PRINTS}"
    echo "     gated debug cout statements: ${GATED_DEBUG_PRINTS}"
    echo "     rpc_debug_enabled gates:     ${GATE_BLOCKS}"
    echo "     DINERO_RPC_DEBUG references: ${GATE_DECL}"
    check "the per-RPC debug prints are gated behind DINERO_RPC_DEBUG" \
        test "${GATE_DECL}" -ge 1
    check "every [RPC DEBUG] print site sits inside a gate block" \
        test "${GATED_DEBUG_PRINTS}" -eq "${DEBUG_PRINTS}"
    check "no new ungated [RPC DEBUG] print was added (expected 4)" \
        test "${DEBUG_PRINTS}" -eq 4
else
    echo "     (skipped: ${RPC_SERVER} not present)"
fi

echo
if (( FAILURES != 0 )); then
    echo -e "${RED}FAILED: ${FAILURES}/${CHECKS} checks${NC}"
    exit 1
fi
echo -e "${GREEN}PASSED: ${CHECKS}/${CHECKS} checks${NC}"
