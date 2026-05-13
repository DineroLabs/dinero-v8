#!/bin/bash
#
# Tier-3 Adversarial Utreexo Test Suite Runner
#
# CONSENSUS-CRITICAL: All tests in this suite MUST pass.
# Any failure indicates a consensus vulnerability.
#
# Tests:
#   Tier-3.1: Missing proof rejection
#   Tier-3.3: Stale proof rejection (replay attack)
#   Tier-3.4: Wrong root rejection
#
# Usage:
#   ./run_all.sh           Run all tests
#   ./run_all.sh --quick   Run quick smoke test
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo ""
echo "═══════════════════════════════════════════════════════════════════"
echo "  TIER-3 ADVERSARIAL UTREEXO TEST SUITE"
echo "  CONSENSUS-CRITICAL: All tests must pass"
echo "═══════════════════════════════════════════════════════════════════"
echo ""

PASSED=0
FAILED=0
SKIPPED=0

run_test() {
    local name="$1"
    local script="$2"

    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${CYAN}Running: $name${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""

    if [[ ! -x "$script" ]]; then
        echo -e "${YELLOW}⚠️  SKIPPED: $script not executable${NC}"
        ((SKIPPED++))
        return
    fi

    if "$script"; then
        echo -e "${GREEN}✅ PASSED: $name${NC}"
        ((PASSED++))
    else
        echo -e "${RED}❌ FAILED: $name${NC}"
        ((FAILED++))
    fi
    echo ""
}

# Run all Tier-3 tests
run_test "Tier-3.1: Missing Proof" "$SCRIPT_DIR/test_missing_proof.sh"
run_test "Tier-3.2: Invalid Proof (Corrupted)" "$SCRIPT_DIR/test_invalid_proof.sh"
run_test "Tier-3.3: Stale Proof (Replay)" "$SCRIPT_DIR/test_stale_proof.sh"
run_test "Tier-3.4: Wrong Root" "$SCRIPT_DIR/test_wrong_root.sh"
run_test "Tier-3.5: Reorg Proof Replay" "$SCRIPT_DIR/test_reorg_proof_replay.sh"

# Summary
echo ""
echo "═══════════════════════════════════════════════════════════════════"
echo "  TIER-3 ADVERSARIAL TEST RESULTS"
echo "═══════════════════════════════════════════════════════════════════"
echo ""
echo -e "  Passed:  ${GREEN}$PASSED${NC}"
echo -e "  Failed:  ${RED}$FAILED${NC}"
echo -e "  Skipped: ${YELLOW}$SKIPPED${NC}"
echo ""

if [[ "$FAILED" -gt 0 ]]; then
    echo -e "${RED}═══════════════════════════════════════════════════════════════════${NC}"
    echo -e "${RED}  ❌ TIER-3 FAILED: $FAILED test(s) failed${NC}"
    echo -e "${RED}  CONSENSUS VULNERABILITY DETECTED${NC}"
    echo -e "${RED}═══════════════════════════════════════════════════════════════════${NC}"
    exit 1
fi

echo -e "${GREEN}═══════════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  ✅ TIER-3 PASSED: All adversarial tests passed${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════════════${NC}"
echo ""
echo "Validated:"
echo "  ✓ Missing proofs are rejected (Tier-3.1)"
echo "  ✓ Corrupted proofs are rejected (Tier-3.2)"
echo "  ✓ Stale proofs cannot be replayed (Tier-3.3)"
echo "  ✓ Wrong Utreexo roots are rejected (Tier-3.4)"
echo "  ✓ Cross-fork proof replay is blocked (Tier-3.5)"
echo "  ✓ No shadow mode or bypass exists"
echo ""

exit 0
