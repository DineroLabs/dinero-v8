#!/bin/bash
#
# Run all regression and stress tests for Dinero Core
# Usage: ./run_regression_tests.sh [quick|full]
#

set -e

MODE="${1:-quick}"
BUILD_DIR="${BUILD_DIR:-build}"

echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Dinero Core v1.1 - Regression & Stress Test Suite       ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if build exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory not found: $BUILD_DIR${NC}"
    echo "Run: cmake -B $BUILD_DIR && cmake --build $BUILD_DIR"
    exit 1
fi

# Change to build directory
cd "$BUILD_DIR"

# Track results
TOTAL=0
PASSED=0
FAILED=0

run_test() {
    local test_name="$1"
    local test_path="$2"

    if [ ! -f "$test_path" ]; then
        echo -e "${YELLOW}⊘ SKIP${NC}: $test_name (not built)"
        return
    fi

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Running: $test_name"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    TOTAL=$((TOTAL + 1))

    if "$test_path"; then
        echo -e "${GREEN}✓ PASS${NC}: $test_name"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ FAIL${NC}: $test_name"
        FAILED=$((FAILED + 1))
    fi
}

# ============================================================================
# Regression Tests
# ============================================================================

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  PHASE 1: Regression Tests"
echo "═══════════════════════════════════════════════════════════"

run_test "Wallet Recovery Tests" "tests/regression/test_wallet_recovery"
run_test "Deep Reorg Tests" "tests/regression/test_deep_reorg"

# ============================================================================
# Stress Tests
# ============================================================================

if [ "$MODE" = "full" ]; then
    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "  PHASE 2: Stress Tests"
    echo "═══════════════════════════════════════════════════════════"

    run_test "Mempool Stress Tests" "tests/stress/test_mempool_stress"
else
    echo ""
    echo -e "${YELLOW}Skipping stress tests in quick mode${NC}"
    echo "Run with 'full' argument to include stress tests"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║  Test Results Summary                                      ║"
echo "╠════════════════════════════════════════════════════════════╣"
printf "║  Total:  %-3d                                              ║\n" "$TOTAL"
printf "║  ${GREEN}Passed: %-3d${NC}                                              ║\n" "$PASSED"
printf "║  ${RED}Failed: %-3d${NC}                                              ║\n" "$FAILED"
echo "╚════════════════════════════════════════════════════════════╝"

if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo -e "${RED}❌ Some tests failed!${NC}"
    echo "Review the output above for details."
    exit 1
else
    echo ""
    echo -e "${GREEN}✅ All tests passed!${NC}"
    exit 0
fi
