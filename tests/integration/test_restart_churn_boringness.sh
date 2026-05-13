#!/usr/bin/env bash
#
# Restart/churn boringness gate and long-run harness.
#
# Profiles:
#   PROFILE=gate  -> short deterministic gate suitable for CTest/release checks
#   PROFILE=soak  -> longer manual soak using the same component scripts
#
# The harness intentionally composes three production-sensitive restart paths:
#   1) CSN reorg/restart churn
#   2) dual-node mining restart churn
#   3) mempool persistence across daemon restarts
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CSN_SOAK_SCRIPT="${SCRIPT_DIR}/test_csn_reorg_churn_restart_soak.sh"
MINING_SOAK_SCRIPT="${PROJECT_ROOT}/tests/stress/test_dual_node_mining_restart_soak.sh"
MEMPOOL_RESTART_SCRIPT="${SCRIPT_DIR}/test_mempool_restart.sh"

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

PROFILE="${PROFILE:-gate}"
KEEP_TMP_ON_FAIL="${KEEP_TMP_ON_FAIL:-1}"

case "${PROFILE}" in
    gate)
        RUN_MEMPOOL_RESTART="${RUN_MEMPOOL_RESTART:-1}"
        CSN_SOAK_SECONDS="${CSN_SOAK_SECONDS:-150}"
        CSN_MIN_CYCLES="${CSN_MIN_CYCLES:-1}"
        CSN_PRELOAD_BLOCKS="${CSN_PRELOAD_BLOCKS:-90}"
        CSN_CHURN_ROUNDS="${CSN_CHURN_ROUNDS:-3}"
        CSN_ROUND_ADVANCE_BLOCKS="${CSN_ROUND_ADVANCE_BLOCKS:-10}"
        CSN_ROUND_REBUILD_BLOCKS="${CSN_ROUND_REBUILD_BLOCKS:-6}"
        CSN_TIMEOUT="${CSN_TIMEOUT:-210}"

        MINING_SOAK_SECONDS="${MINING_SOAK_SECONDS:-150}"
        MINING_MIN_CYCLES="${MINING_MIN_CYCLES:-1}"
        MINING_TARGET_HEIGHT_BASE="${MINING_TARGET_HEIGHT_BASE:-80}"
        MINING_TARGET_HEIGHT_STEP="${MINING_TARGET_HEIGHT_STEP:-15}"
        MINING_MINERS_LOW="${MINING_MINERS_LOW:-3}"
        MINING_MINERS_HIGH="${MINING_MINERS_HIGH:-6}"
        MINING_CONVERGENCE_TIMEOUT="${MINING_CONVERGENCE_TIMEOUT:-120}"
        MINING_SETTLE_TIME="${MINING_SETTLE_TIME:-6}"
        ;;
    soak)
        RUN_MEMPOOL_RESTART="${RUN_MEMPOOL_RESTART:-1}"
        CSN_SOAK_SECONDS="${CSN_SOAK_SECONDS:-}"
        CSN_SOAK_HOURS="${CSN_SOAK_HOURS:-4}"
        CSN_MIN_CYCLES="${CSN_MIN_CYCLES:-2}"
        CSN_PRELOAD_BLOCKS="${CSN_PRELOAD_BLOCKS:-140}"
        CSN_CHURN_ROUNDS="${CSN_CHURN_ROUNDS:-5}"
        CSN_ROUND_ADVANCE_BLOCKS="${CSN_ROUND_ADVANCE_BLOCKS:-14}"
        CSN_ROUND_REBUILD_BLOCKS="${CSN_ROUND_REBUILD_BLOCKS:-10}"
        CSN_TIMEOUT="${CSN_TIMEOUT:-480}"

        MINING_SOAK_SECONDS="${MINING_SOAK_SECONDS:-}"
        MINING_SOAK_HOURS="${MINING_SOAK_HOURS:-4}"
        MINING_MIN_CYCLES="${MINING_MIN_CYCLES:-2}"
        MINING_TARGET_HEIGHT_BASE="${MINING_TARGET_HEIGHT_BASE:-140}"
        MINING_TARGET_HEIGHT_STEP="${MINING_TARGET_HEIGHT_STEP:-20}"
        MINING_MINERS_LOW="${MINING_MINERS_LOW:-4}"
        MINING_MINERS_HIGH="${MINING_MINERS_HIGH:-8}"
        MINING_CONVERGENCE_TIMEOUT="${MINING_CONVERGENCE_TIMEOUT:-150}"
        MINING_SETTLE_TIME="${MINING_SETTLE_TIME:-8}"
        ;;
    *)
        echo "Unsupported PROFILE=${PROFILE}; expected gate or soak"
        exit 1
        ;;
esac

[[ -x "${CSN_SOAK_SCRIPT}" ]] || { echo "Missing executable script: ${CSN_SOAK_SCRIPT}"; exit 1; }
[[ -x "${MINING_SOAK_SCRIPT}" ]] || { echo "Missing executable script: ${MINING_SOAK_SCRIPT}"; exit 1; }
if [[ "${RUN_MEMPOOL_RESTART}" == "1" ]]; then
    [[ -x "${MEMPOOL_RESTART_SCRIPT}" ]] || { echo "Missing executable script: ${MEMPOOL_RESTART_SCRIPT}"; exit 1; }
fi

if [[ -z "${ARTIFACT_ROOT:-}" ]]; then
    ARTIFACT_ROOT=$(mktemp -d -t dinero_restart_churn_gate_XXXXXX)
else
    mkdir -p "${ARTIFACT_ROOT}"
fi

RUN_LOG="${ARTIFACT_ROOT}/run.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

info() {
    echo -e "${CYAN}$1${NC}" | tee -a "${RUN_LOG}"
}

pass() {
    echo -e "${GREEN}$1${NC}" | tee -a "${RUN_LOG}"
}

fail() {
    echo -e "${RED}FAILED:${NC} $1" | tee -a "${RUN_LOG}"
    exit 1
}

echo "" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "  RESTART / CHURN BORINGNESS HARNESS" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "  profile:              ${PROFILE}" | tee -a "${RUN_LOG}"
echo "  dinerod:              ${DINEROD}" | tee -a "${RUN_LOG}"
echo "  artifact_root:        ${ARTIFACT_ROOT}" | tee -a "${RUN_LOG}"
echo "  run_mempool_restart:  ${RUN_MEMPOOL_RESTART}" | tee -a "${RUN_LOG}"
echo "=================================================================" | tee -a "${RUN_LOG}"
echo "" | tee -a "${RUN_LOG}"

info "[1/3] Running CSN restart churn soak"
DINEROD="${DINEROD}" \
ARTIFACT_ROOT="${ARTIFACT_ROOT}/csn" \
SOAK_SECONDS="${CSN_SOAK_SECONDS:-}" \
SOAK_HOURS="${CSN_SOAK_HOURS:-}" \
MIN_CYCLES="${CSN_MIN_CYCLES}" \
PRELOAD_BLOCKS="${CSN_PRELOAD_BLOCKS}" \
CHURN_ROUNDS="${CSN_CHURN_ROUNDS}" \
ROUND_ADVANCE_BLOCKS="${CSN_ROUND_ADVANCE_BLOCKS}" \
ROUND_REBUILD_BLOCKS="${CSN_ROUND_REBUILD_BLOCKS}" \
TIMEOUT="${CSN_TIMEOUT}" \
KEEP_TMP_ON_FAIL="${KEEP_TMP_ON_FAIL}" \
"${CSN_SOAK_SCRIPT}" > "${ARTIFACT_ROOT}/csn_soak.log" 2>&1 || {
    tail -40 "${ARTIFACT_ROOT}/csn_soak.log" || true
    fail "CSN restart churn soak failed"
}
pass "CSN restart churn soak passed"

info "[2/3] Running dual-node mining restart soak"
DINEROD="${DINEROD}" \
ARTIFACT_ROOT="${ARTIFACT_ROOT}/mining" \
SOAK_SECONDS="${MINING_SOAK_SECONDS:-}" \
SOAK_HOURS="${MINING_SOAK_HOURS:-}" \
MIN_CYCLES="${MINING_MIN_CYCLES}" \
TARGET_HEIGHT_BASE="${MINING_TARGET_HEIGHT_BASE}" \
TARGET_HEIGHT_STEP="${MINING_TARGET_HEIGHT_STEP}" \
MINERS_LOW="${MINING_MINERS_LOW}" \
MINERS_HIGH="${MINING_MINERS_HIGH}" \
CONVERGENCE_TIMEOUT="${MINING_CONVERGENCE_TIMEOUT}" \
SETTLE_TIME="${MINING_SETTLE_TIME}" \
"${MINING_SOAK_SCRIPT}" > "${ARTIFACT_ROOT}/mining_soak.log" 2>&1 || {
    tail -40 "${ARTIFACT_ROOT}/mining_soak.log" || true
    fail "Dual-node mining restart soak failed"
}
pass "Dual-node mining restart soak passed"

if [[ "${RUN_MEMPOOL_RESTART}" == "1" ]]; then
    info "[3/3] Running mempool restart persistence check"
    DINEROD="${DINEROD}" \
    KEEP_TMP_ON_FAIL="${KEEP_TMP_ON_FAIL}" \
    "${MEMPOOL_RESTART_SCRIPT}" > "${ARTIFACT_ROOT}/mempool_restart.log" 2>&1 || {
        tail -40 "${ARTIFACT_ROOT}/mempool_restart.log" || true
        fail "Mempool restart persistence check failed"
    }
    pass "Mempool restart persistence check passed"
else
    info "[3/3] Skipping mempool restart persistence check"
fi

echo "" | tee -a "${RUN_LOG}"
pass "Restart / churn boringness harness passed"
echo "  artifacts: ${ARTIFACT_ROOT}" | tee -a "${RUN_LOG}"
echo "" | tee -a "${RUN_LOG}"

exit 0
