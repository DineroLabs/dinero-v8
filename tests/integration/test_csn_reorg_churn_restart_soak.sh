#!/usr/bin/env bash
#
# Duration-based CSN churn/restart soak harness.
#
# Repeatedly runs the CSN reorg churn test for a target wall-clock duration.
# Each cycle starts fresh bridge/CSN daemons, exercising restart resilience.
#
# Usage examples:
#   ./test_csn_reorg_churn_restart_soak.sh
#   SOAK_HOURS=6 MIN_CYCLES=3 ./test_csn_reorg_churn_restart_soak.sh
#   SOAK_SECONDS=1800 CHURN_ROUNDS=6 PRELOAD_BLOCKS=180 ./test_csn_reorg_churn_restart_soak.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHURN_SCRIPT="${SCRIPT_DIR}/test_csn_reorg_churn.sh"

SOAK_HOURS=${SOAK_HOURS:-4}
SOAK_SECONDS=${SOAK_SECONDS:-}
MIN_CYCLES=${MIN_CYCLES:-2}
MAX_CYCLES=${MAX_CYCLES:-0}
COOLDOWN_SECONDS=${COOLDOWN_SECONDS:-2}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

# Churn-cycle parameters passed through to the underlying script.
PRELOAD_BLOCKS=${PRELOAD_BLOCKS:-120}
CHURN_ROUNDS=${CHURN_ROUNDS:-4}
ROUND_ADVANCE_BLOCKS=${ROUND_ADVANCE_BLOCKS:-12}
ROUND_REBUILD_BLOCKS=${ROUND_REBUILD_BLOCKS:-8}
TIMEOUT=${TIMEOUT:-240}
INVALIDATION_TIMEOUT=${INVALIDATION_TIMEOUT:-60}

if [[ -n "$SOAK_SECONDS" ]]; then
    TARGET_SECONDS="$SOAK_SECONDS"
else
    TARGET_SECONDS=$(awk -v h="$SOAK_HOURS" 'BEGIN { printf("%d", h * 3600) }')
fi

if [[ "$TARGET_SECONDS" -lt 1 ]]; then
    echo "Invalid target duration: TARGET_SECONDS=${TARGET_SECONDS}"
    exit 1
fi
if [[ "$MIN_CYCLES" -lt 1 ]]; then
    echo "MIN_CYCLES must be >= 1"
    exit 1
fi

[[ -x "$CHURN_SCRIPT" ]] || {
    echo "Missing executable churn script: $CHURN_SCRIPT"
    exit 1
}

if [[ -z "${ARTIFACT_ROOT:-}" ]]; then
    ARTIFACT_ROOT=$(mktemp -d -t dinero_csn_churn_soak_XXXXXX)
else
    mkdir -p "$ARTIFACT_ROOT"
fi

SUMMARY_TSV="${ARTIFACT_ROOT}/summary.tsv"
RUN_LOG="${ARTIFACT_ROOT}/run.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

fail() {
    echo -e "${RED}FAILED:${NC} $1" | tee -a "$RUN_LOG"
    exit 1
}

info() {
    echo -e "${CYAN}$1${NC}" | tee -a "$RUN_LOG"
}

pass() {
    echo -e "${GREEN}$1${NC}" | tee -a "$RUN_LOG"
}

echo -e "cycle\tstatus\tseconds\treorg_markers\tcommitment_mismatch\tlast_bridge_height\tlast_csn_height\tlog_path" > "$SUMMARY_TSV"

START_TS=$(date +%s)
CYCLE=0
FAILURES=0

echo ""
echo "=================================================================" | tee -a "$RUN_LOG"
echo "  CSN REORG CHURN RESTART SOAK" | tee -a "$RUN_LOG"
echo "=================================================================" | tee -a "$RUN_LOG"
echo "  target_seconds:      ${TARGET_SECONDS}" | tee -a "$RUN_LOG"
echo "  min_cycles:          ${MIN_CYCLES}" | tee -a "$RUN_LOG"
echo "  max_cycles:          ${MAX_CYCLES}" | tee -a "$RUN_LOG"
echo "  cooldown_seconds:    ${COOLDOWN_SECONDS}" | tee -a "$RUN_LOG"
echo "  artifact_root:       ${ARTIFACT_ROOT}" | tee -a "$RUN_LOG"
echo "-----------------------------------------------------------------" | tee -a "$RUN_LOG"
echo "  preload blocks:      ${PRELOAD_BLOCKS}" | tee -a "$RUN_LOG"
echo "  churn rounds:        ${CHURN_ROUNDS}" | tee -a "$RUN_LOG"
echo "  round advance:       ${ROUND_ADVANCE_BLOCKS}" | tee -a "$RUN_LOG"
echo "  round rebuild:       ${ROUND_REBUILD_BLOCKS}" | tee -a "$RUN_LOG"
echo "  sync timeout:        ${TIMEOUT}" | tee -a "$RUN_LOG"
echo "  invalidate timeout:  ${INVALIDATION_TIMEOUT}" | tee -a "$RUN_LOG"
echo "=================================================================" | tee -a "$RUN_LOG"
echo "" | tee -a "$RUN_LOG"

while true; do
    CYCLE=$((CYCLE + 1))
    CYCLE_START=$(date +%s)
    CYCLE_LOG="${ARTIFACT_ROOT}/cycle_${CYCLE}.log"

    info "[Cycle ${CYCLE}] Starting churn/restart cycle"

    set +e
    PRELOAD_BLOCKS="$PRELOAD_BLOCKS" \
    CHURN_ROUNDS="$CHURN_ROUNDS" \
    ROUND_ADVANCE_BLOCKS="$ROUND_ADVANCE_BLOCKS" \
    ROUND_REBUILD_BLOCKS="$ROUND_REBUILD_BLOCKS" \
    TIMEOUT="$TIMEOUT" \
    INVALIDATION_TIMEOUT="$INVALIDATION_TIMEOUT" \
    KEEP_TMP_ON_FAIL="$KEEP_TMP_ON_FAIL" \
    "$CHURN_SCRIPT" >"$CYCLE_LOG" 2>&1
    RC=$?
    set -e

    CYCLE_END=$(date +%s)
    CYCLE_SECONDS=$((CYCLE_END - CYCLE_START))

    REORG_MARKERS=$(sed -n 's/.*Reorg markers in CSN logs:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$CYCLE_LOG" | tail -1)
    if [[ -z "$REORG_MARKERS" ]]; then
        REORG_MARKERS=$(grep -cE "STATELESS reorg|RewindToCheckpoint|REORG DETECTED" "$CYCLE_LOG" 2>/dev/null || true)
        REORG_MARKERS=${REORG_MARKERS:-0}
    fi
    COMMITMENT_MISMATCH=$(grep -c "COMMITMENT MISMATCH" "$CYCLE_LOG" 2>/dev/null || true)
    COMMITMENT_MISMATCH=${COMMITMENT_MISMATCH:-0}

    LAST_HEIGHT_LINE=$(grep "Heights after churn: bridge=" "$CYCLE_LOG" 2>/dev/null | tail -1 || true)
    LAST_BRIDGE_HEIGHT=$(echo "$LAST_HEIGHT_LINE" | sed -n 's/.*bridge=\([0-9][0-9]*\).*/\1/p')
    LAST_CSN_HEIGHT=$(echo "$LAST_HEIGHT_LINE" | sed -n 's/.*csn=\([0-9][0-9]*\).*/\1/p')
    [[ -z "$LAST_BRIDGE_HEIGHT" ]] && LAST_BRIDGE_HEIGHT=0
    [[ -z "$LAST_CSN_HEIGHT" ]] && LAST_CSN_HEIGHT=0

    STATUS="PASS"
    if [[ "$RC" -ne 0 ]]; then
        STATUS="FAIL"
        FAILURES=$((FAILURES + 1))
    fi

    echo -e "${CYCLE}\t${STATUS}\t${CYCLE_SECONDS}\t${REORG_MARKERS}\t${COMMITMENT_MISMATCH}\t${LAST_BRIDGE_HEIGHT}\t${LAST_CSN_HEIGHT}\t${CYCLE_LOG}" >> "$SUMMARY_TSV"

    if [[ "$STATUS" == "PASS" ]]; then
        pass "[Cycle ${CYCLE}] PASS (${CYCLE_SECONDS}s, reorg_markers=${REORG_MARKERS})"
    else
        echo -e "${RED}[Cycle ${CYCLE}] FAIL (${CYCLE_SECONDS}s)${NC}" | tee -a "$RUN_LOG"
        echo "  log: ${CYCLE_LOG}" | tee -a "$RUN_LOG"
        tail -40 "$CYCLE_LOG" | sed 's/^/  /' | tee -a "$RUN_LOG"
        fail "Cycle ${CYCLE} failed"
    fi

    ELAPSED=$((CYCLE_END - START_TS))
    REMAINING=$((TARGET_SECONDS - ELAPSED))
    if [[ "$REMAINING" -lt 0 ]]; then
        REMAINING=0
    fi
    info "[Cycle ${CYCLE}] elapsed=${ELAPSED}s remaining=${REMAINING}s"

    if [[ "$MAX_CYCLES" -gt 0 && "$CYCLE" -ge "$MAX_CYCLES" ]]; then
        info "Reached MAX_CYCLES=${MAX_CYCLES}; stopping soak."
        break
    fi

    if [[ "$ELAPSED" -ge "$TARGET_SECONDS" && "$CYCLE" -ge "$MIN_CYCLES" ]]; then
        info "Reached duration target and min cycles; stopping soak."
        break
    fi

    sleep "$COOLDOWN_SECONDS"
done

TOTAL_SECONDS=$(( $(date +%s) - START_TS ))
PASS_COUNT=$((CYCLE - FAILURES))

echo "" | tee -a "$RUN_LOG"
echo "=================================================================" | tee -a "$RUN_LOG"
echo -e "${GREEN}  CSN REORG CHURN RESTART SOAK PASSED${NC}" | tee -a "$RUN_LOG"
echo "=================================================================" | tee -a "$RUN_LOG"
echo "  cycles_run:          ${CYCLE}" | tee -a "$RUN_LOG"
echo "  cycles_passed:       ${PASS_COUNT}" | tee -a "$RUN_LOG"
echo "  cycles_failed:       ${FAILURES}" | tee -a "$RUN_LOG"
echo "  total_seconds:       ${TOTAL_SECONDS}" | tee -a "$RUN_LOG"
echo "  summary_tsv:         ${SUMMARY_TSV}" | tee -a "$RUN_LOG"
echo "  run_log:             ${RUN_LOG}" | tee -a "$RUN_LOG"
echo "=================================================================" | tee -a "$RUN_LOG"
echo "" | tee -a "$RUN_LOG"

exit 0
