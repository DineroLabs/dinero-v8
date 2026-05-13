#!/usr/bin/env bash
#
# Bridge proof-serving hardening harness
#
# Runs the production-risk suite around proof serving:
#  1) reorg churn with live CSN sync
#  2) restart resilience via repeated churn runs
#  3) adversarial proof validation suite
#
# Usage:
#   ./test_bridge_proof_plane_hardening.sh
#   CYCLES=3 PRELOAD_BLOCKS=200 CHURN_ROUNDS=6 ./test_bridge_proof_plane_hardening.sh
#   RUN_LONG_SOAK=1 SOAK_HOURS=4 SOAK_MIN_CYCLES=2 ./test_bridge_proof_plane_hardening.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CHURN_SCRIPT="${SCRIPT_DIR}/test_csn_reorg_churn.sh"
LONG_SOAK_SCRIPT="${SCRIPT_DIR}/test_csn_reorg_churn_restart_soak.sh"
GETPROOF_ABUSE_SCRIPT="${SCRIPT_DIR}/test_getproof_abuse_disconnect.sh"
PROOFDATA_ABUSE_SCRIPT="${SCRIPT_DIR}/test_proofdata_adversarial.sh"
ADVERSARIAL_SCRIPT="${PROJECT_ROOT}/tests/adversarial/utreexo/run_all.sh"

CYCLES=${CYCLES:-2}
PRELOAD_BLOCKS=${PRELOAD_BLOCKS:-140}
CHURN_ROUNDS=${CHURN_ROUNDS:-5}
ROUND_ADVANCE_BLOCKS=${ROUND_ADVANCE_BLOCKS:-14}
ROUND_REBUILD_BLOCKS=${ROUND_REBUILD_BLOCKS:-10}
TIMEOUT=${TIMEOUT:-240}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}
RUN_ADVERSARIAL=${RUN_ADVERSARIAL:-1}
RUN_GETPROOF_ABUSE=${RUN_GETPROOF_ABUSE:-1}
RUN_PROOFDATA_ABUSE=${RUN_PROOFDATA_ABUSE:-1}
RUN_LONG_SOAK=${RUN_LONG_SOAK:-0}
SOAK_HOURS=${SOAK_HOURS:-4}
SOAK_SECONDS=${SOAK_SECONDS:-}
SOAK_MIN_CYCLES=${SOAK_MIN_CYCLES:-2}

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

fail() {
    echo -e "${RED}FAILED:${NC} $1"
    exit 1
}

info() {
    echo -e "${CYAN}$1${NC}"
}

pass() {
    echo -e "${GREEN}$1${NC}"
}

[[ -x "$CHURN_SCRIPT" ]] || fail "Missing executable churn script: $CHURN_SCRIPT"
[[ -x "${PROJECT_ROOT}/build/dinerod" || -x "${PROJECT_ROOT}/dinerod" ]] || \
    fail "dinerod binary not found (build first)"
if [[ "$RUN_GETPROOF_ABUSE" == "1" ]]; then
    [[ -x "$GETPROOF_ABUSE_SCRIPT" ]] || fail "Missing executable abuse script: $GETPROOF_ABUSE_SCRIPT"
fi
if [[ "$RUN_PROOFDATA_ABUSE" == "1" ]]; then
    [[ -x "$PROOFDATA_ABUSE_SCRIPT" ]] || fail "Missing executable abuse script: $PROOFDATA_ABUSE_SCRIPT"
fi
if [[ "$RUN_LONG_SOAK" == "1" ]]; then
    [[ -x "$LONG_SOAK_SCRIPT" ]] || fail "Missing executable long soak script: $LONG_SOAK_SCRIPT"
fi

echo ""
echo "================================================================="
echo "  BRIDGE PROOF-SERVING HARDENING HARNESS"
echo "================================================================="
echo "  cycles:               $CYCLES"
echo "  preload blocks:       $PRELOAD_BLOCKS"
echo "  churn rounds/cycle:   $CHURN_ROUNDS"
echo "  round advance blocks: $ROUND_ADVANCE_BLOCKS"
echo "  round rebuild blocks: $ROUND_REBUILD_BLOCKS"
echo "  timeout:              $TIMEOUT s"
echo "  run getproof abuse:   $RUN_GETPROOF_ABUSE"
echo "  run proofdata abuse:  $RUN_PROOFDATA_ABUSE"
echo "  run long soak:        $RUN_LONG_SOAK"
echo "  soak hours:           $SOAK_HOURS"
echo "  soak seconds:         ${SOAK_SECONDS:-<unset>}"
echo "  soak min cycles:      $SOAK_MIN_CYCLES"
echo "  run adversarial:      $RUN_ADVERSARIAL"
echo "================================================================="
echo ""

for ((cycle=1; cycle<=CYCLES; cycle++)); do
    info "[Cycle ${cycle}/${CYCLES}] Running CSN reorg churn"
    PRELOAD_BLOCKS="$PRELOAD_BLOCKS" \
    CHURN_ROUNDS="$CHURN_ROUNDS" \
    ROUND_ADVANCE_BLOCKS="$ROUND_ADVANCE_BLOCKS" \
    ROUND_REBUILD_BLOCKS="$ROUND_REBUILD_BLOCKS" \
    TIMEOUT="$TIMEOUT" \
    KEEP_TMP_ON_FAIL="$KEEP_TMP_ON_FAIL" \
    "$CHURN_SCRIPT"

    pass "Cycle ${cycle} completed"
done

if [[ "$RUN_LONG_SOAK" == "1" ]]; then
    info "[LongSoak] Running duration-based churn/restart soak"
    SOAK_HOURS="$SOAK_HOURS" \
    SOAK_SECONDS="$SOAK_SECONDS" \
    MIN_CYCLES="$SOAK_MIN_CYCLES" \
    PRELOAD_BLOCKS="$PRELOAD_BLOCKS" \
    CHURN_ROUNDS="$CHURN_ROUNDS" \
    ROUND_ADVANCE_BLOCKS="$ROUND_ADVANCE_BLOCKS" \
    ROUND_REBUILD_BLOCKS="$ROUND_REBUILD_BLOCKS" \
    TIMEOUT="$TIMEOUT" \
    KEEP_TMP_ON_FAIL="$KEEP_TMP_ON_FAIL" \
    "$LONG_SOAK_SCRIPT"
    pass "Long soak completed"
fi

if [[ "$RUN_GETPROOF_ABUSE" == "1" ]]; then
    info "[Final] Running getproof abuse disconnect test"
    "$GETPROOF_ABUSE_SCRIPT"
    pass "Getproof abuse test completed"
fi

if [[ "$RUN_PROOFDATA_ABUSE" == "1" ]]; then
    info "[Final] Running proofdata adversarial abuse test"
    "$PROOFDATA_ABUSE_SCRIPT"
    pass "Proofdata abuse test completed"
fi

if [[ "$RUN_ADVERSARIAL" == "1" ]]; then
    [[ -x "$ADVERSARIAL_SCRIPT" ]] || fail "Missing executable adversarial suite: $ADVERSARIAL_SCRIPT"
    info "[Final] Running adversarial Utreexo proof suite"
    "$ADVERSARIAL_SCRIPT"
    pass "Adversarial proof suite completed"
fi

echo ""
pass "Proof-serving hardening harness passed"
echo ""
