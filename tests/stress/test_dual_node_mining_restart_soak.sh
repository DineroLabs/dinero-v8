#!/usr/bin/env bash
#
# Duration-based dual-node mining restart soak.
#
# Repeatedly runs the dual-node mining stress test for a target wall-clock
# duration. Each cycle starts from a fresh pair of nodes, which exercises
# restart resilience, fork convergence, and mining safety under repeated churn.
#
# Usage:
#   ./test_dual_node_mining_restart_soak.sh
#   SOAK_SECONDS=900 MIN_CYCLES=2 ./test_dual_node_mining_restart_soak.sh
#   SOAK_HOURS=6 MINERS_LOW=4 MINERS_HIGH=10 TARGET_HEIGHT_BASE=180 ./test_dual_node_mining_restart_soak.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
STRESS_SCRIPT="${SCRIPT_DIR}/test_dual_node_mining_stress.sh"

if [[ -n "${DINEROD:-}" && -x "${DINEROD}" ]]; then
    DINEROD="${DINEROD}"
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    echo "dinerod not found"
    exit 1
fi

SOAK_HOURS=${SOAK_HOURS:-4}
SOAK_SECONDS=${SOAK_SECONDS:-}
MIN_CYCLES=${MIN_CYCLES:-2}
MAX_CYCLES=${MAX_CYCLES:-0}
COOLDOWN_SECONDS=${COOLDOWN_SECONDS:-2}
TARGET_HEIGHT_BASE=${TARGET_HEIGHT_BASE:-120}
TARGET_HEIGHT_STEP=${TARGET_HEIGHT_STEP:-20}
MINERS_LOW=${MINERS_LOW:-4}
MINERS_HIGH=${MINERS_HIGH:-8}
CONVERGENCE_TIMEOUT=${CONVERGENCE_TIMEOUT:-120}
SETTLE_TIME=${SETTLE_TIME:-8}
STARTUP_WAIT=${STARTUP_WAIT:-8}
RATE_LIMIT_MS=${RATE_LIMIT_MS:-100}
RPC_TIMEOUT=${RPC_TIMEOUT:-5}

if [[ -n "${SOAK_SECONDS}" ]]; then
    TARGET_SECONDS="${SOAK_SECONDS}"
else
    TARGET_SECONDS=$(awk -v h="${SOAK_HOURS}" 'BEGIN { printf("%d", h * 3600) }')
fi

if [[ "${TARGET_SECONDS}" -lt 1 ]]; then
    echo "Invalid target duration: TARGET_SECONDS=${TARGET_SECONDS}"
    exit 1
fi
if [[ "${MIN_CYCLES}" -lt 1 ]]; then
    echo "MIN_CYCLES must be >= 1"
    exit 1
fi
[[ -x "${STRESS_SCRIPT}" ]] || {
    echo "Missing executable stress script: ${STRESS_SCRIPT}"
    exit 1
}

if [[ -z "${ARTIFACT_ROOT:-}" ]]; then
    ARTIFACT_ROOT=$(mktemp -d -t dinero_dual_mining_soak_XXXXXX)
else
    mkdir -p "${ARTIFACT_ROOT}"
fi

SUMMARY_TSV="${ARTIFACT_ROOT}/summary.tsv"
RUN_LOG="${ARTIFACT_ROOT}/run.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

fail() {
    echo -e "${RED}FAILED:${NC} $1" | tee -a "${RUN_LOG}"
    exit 1
}

info() {
    echo -e "${CYAN}$1${NC}" | tee -a "${RUN_LOG}"
}

pass() {
    echo -e "${GREEN}$1${NC}" | tee -a "${RUN_LOG}"
}

choose_profile() {
    local cycle="$1"
    case $(( cycle % 3 )) in
        1)
            PROFILE_NAME="balanced"
            PROFILE_MINERS_A="${MINERS_HIGH}"
            PROFILE_MINERS_B="${MINERS_HIGH}"
            ;;
        2)
            PROFILE_NAME="a_heavy"
            PROFILE_MINERS_A="${MINERS_HIGH}"
            PROFILE_MINERS_B="${MINERS_LOW}"
            ;;
        *)
            PROFILE_NAME="b_heavy"
            PROFILE_MINERS_A="${MINERS_LOW}"
            PROFILE_MINERS_B="${MINERS_HIGH}"
            ;;
    esac
}

echo -e "cycle\tstatus\tseconds\tprofile\tminers_a\tminers_b\ttarget_height\tfinal_height\tlog_path" > "${SUMMARY_TSV}"

START_TS=$(date +%s)
CYCLE=0
FAILURES=0

echo "" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "  DUAL-NODE MINING RESTART SOAK" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "  target_seconds:      ${TARGET_SECONDS}" | tee -a "${RUN_LOG}"
echo "  min_cycles:          ${MIN_CYCLES}" | tee -a "${RUN_LOG}"
echo "  max_cycles:          ${MAX_CYCLES}" | tee -a "${RUN_LOG}"
echo "  cooldown_seconds:    ${COOLDOWN_SECONDS}" | tee -a "${RUN_LOG}"
echo "  artifact_root:       ${ARTIFACT_ROOT}" | tee -a "${RUN_LOG}"
echo "-----------------------------------------------------------------" | tee -a "${RUN_LOG}"
echo "  target_height_base:  ${TARGET_HEIGHT_BASE}" | tee -a "${RUN_LOG}"
echo "  target_height_step:  ${TARGET_HEIGHT_STEP}" | tee -a "${RUN_LOG}"
echo "  miners_low/high:     ${MINERS_LOW}/${MINERS_HIGH}" | tee -a "${RUN_LOG}"
echo "  convergence_timeout: ${CONVERGENCE_TIMEOUT}" | tee -a "${RUN_LOG}"
echo "  settle_time:         ${SETTLE_TIME}" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "" | tee -a "${RUN_LOG}"

while true; do
    CYCLE=$((CYCLE + 1))
    CYCLE_START=$(date +%s)
    CYCLE_LOG="${ARTIFACT_ROOT}/cycle_${CYCLE}.log"
    TARGET_HEIGHT=$((TARGET_HEIGHT_BASE + ((CYCLE - 1) % 3) * TARGET_HEIGHT_STEP))
    choose_profile "${CYCLE}"

    info "[Cycle ${CYCLE}] profile=${PROFILE_NAME} miners=${PROFILE_MINERS_A}/${PROFILE_MINERS_B} target_height=${TARGET_HEIGHT}"

    set +e
    DINEROD="${DINEROD}" \
    TARGET_HEIGHT="${TARGET_HEIGHT}" \
    CONVERGENCE_TIMEOUT="${CONVERGENCE_TIMEOUT}" \
    SETTLE_TIME="${SETTLE_TIME}" \
    MINERS_A="${PROFILE_MINERS_A}" \
    MINERS_B="${PROFILE_MINERS_B}" \
    STARTUP_WAIT="${STARTUP_WAIT}" \
    RATE_LIMIT_MS="${RATE_LIMIT_MS}" \
    RPC_TIMEOUT="${RPC_TIMEOUT}" \
    "${STRESS_SCRIPT}" >"${CYCLE_LOG}" 2>&1
    RC=$?
    set -e

    CYCLE_END=$(date +%s)
    CYCLE_SECONDS=$((CYCLE_END - CYCLE_START))
    FINAL_HEIGHT=$(sed -n 's/.*Final height:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "${CYCLE_LOG}" | tail -1)
    [[ -z "${FINAL_HEIGHT}" ]] && FINAL_HEIGHT=0

    STATUS="PASS"
    if [[ "${RC}" -ne 0 ]]; then
        STATUS="FAIL"
        FAILURES=$((FAILURES + 1))
    fi

    echo -e "${CYCLE}\t${STATUS}\t${CYCLE_SECONDS}\t${PROFILE_NAME}\t${PROFILE_MINERS_A}\t${PROFILE_MINERS_B}\t${TARGET_HEIGHT}\t${FINAL_HEIGHT}\t${CYCLE_LOG}" >> "${SUMMARY_TSV}"

    if [[ "${STATUS}" == "PASS" ]]; then
        pass "[Cycle ${CYCLE}] PASS (${CYCLE_SECONDS}s, final_height=${FINAL_HEIGHT})"
    else
        echo -e "${RED}[Cycle ${CYCLE}] FAIL (${CYCLE_SECONDS}s)${NC}" | tee -a "${RUN_LOG}"
        echo "  log: ${CYCLE_LOG}" | tee -a "${RUN_LOG}"
        tail -40 "${CYCLE_LOG}" | sed 's/^/  /' | tee -a "${RUN_LOG}"
        fail "Cycle ${CYCLE} failed"
    fi

    ELAPSED=$((CYCLE_END - START_TS))
    REMAINING=$((TARGET_SECONDS - ELAPSED))
    if [[ "${REMAINING}" -lt 0 ]]; then
        REMAINING=0
    fi
    info "[Cycle ${CYCLE}] elapsed=${ELAPSED}s remaining=${REMAINING}s"

    if [[ "${MAX_CYCLES}" -gt 0 && "${CYCLE}" -ge "${MAX_CYCLES}" ]]; then
        info "Reached MAX_CYCLES=${MAX_CYCLES}; stopping soak."
        break
    fi

    if [[ "${ELAPSED}" -ge "${TARGET_SECONDS}" && "${CYCLE}" -ge "${MIN_CYCLES}" ]]; then
        info "Reached duration target and min cycles; stopping soak."
        break
    fi

    sleep "${COOLDOWN_SECONDS}"
done

TOTAL_SECONDS=$(( $(date +%s) - START_TS ))
PASS_COUNT=$((CYCLE - FAILURES))

echo "" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo -e "${GREEN}  DUAL-NODE MINING RESTART SOAK PASSED${NC}" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "  cycles_run:          ${CYCLE}" | tee -a "${RUN_LOG}"
echo "  cycles_passed:       ${PASS_COUNT}" | tee -a "${RUN_LOG}"
echo "  cycles_failed:       ${FAILURES}" | tee -a "${RUN_LOG}"
echo "  total_seconds:       ${TOTAL_SECONDS}" | tee -a "${RUN_LOG}"
echo "  summary_tsv:         ${SUMMARY_TSV}" | tee -a "${RUN_LOG}"
echo "  run_log:             ${RUN_LOG}" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "" | tee -a "${RUN_LOG}"

exit 0
