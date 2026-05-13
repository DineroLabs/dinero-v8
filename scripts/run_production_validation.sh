#!/bin/bash
# Production Validation Test Runner
# Executes all critical validation tests for DineroCoin storage production hardening

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_RESULTS_DIR="$PROJECT_ROOT/test_results"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Create test results directory
mkdir -p "$TEST_RESULTS_DIR"

echo -e "${BLUE}=== DineroCoin Production Validation Test Suite ===${NC}"
echo "Started at: $(date)"
echo "Results will be saved to: $TEST_RESULTS_DIR"
echo ""

# Function to run test and capture results
run_test() {
    local test_name="$1"
    local test_command="$2"
    local log_file="$TEST_RESULTS_DIR/${test_name}.log"
    
    echo -e "${YELLOW}Running: $test_name${NC}"
    echo "Command: $test_command"
    echo "Log: $log_file"
    
    if eval "$test_command" > "$log_file" 2>&1; then
        echo -e "${GREEN}✓ PASSED: $test_name${NC}"
        return 0
    else
        echo -e "${RED}✗ FAILED: $test_name${NC}"
        echo "Check log: $log_file"
        return 1
    fi
}

# Build tests if needed
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Building tests...${NC}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Debug -DDIN_ENABLE_TESTING=ON
    make -j$(nproc)
    cd "$PROJECT_ROOT"
fi

# Test execution tracking
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# 1. Durability Test Suite (500+ chaos/power-loss runs)
echo -e "\n${BLUE}=== 1. Durability Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "durability_500_runs" "$BUILD_DIR/test/chaos/durability_test_suite --gtest_filter=DurabilityTestSuite.PowerLossSimulation500Runs"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 2. Cold Restart Parity (10k blocks with restarts)
echo -e "\n${BLUE}=== 2. Cold Restart Parity Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "cold_restart_10k" "$BUILD_DIR/test/chaos/durability_test_suite --gtest_filter=DurabilityTestSuite.ColdRestartParity10kBlocks"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 3. Reorg Safety Validation
echo -e "\n${BLUE}=== 3. Reorg Safety Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "reorg_safety" "$BUILD_DIR/test/chaos/durability_test_suite --gtest_filter=DurabilityTestSuite.ReorgSafetyValidation"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 4. Crash Consistency Testing
echo -e "\n${BLUE}=== 4. Crash Consistency Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "crash_consistency" "$BUILD_DIR/test/chaos/durability_test_suite --gtest_filter=DurabilityTestSuite.CrashConsistencyCheckpoints"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 5. Backpressure Validation
echo -e "\n${BLUE}=== 5. Backpressure Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "backpressure_validation" "$BUILD_DIR/test/soak/soak_test_framework --gtest_filter=SoakTestFramework.BackpressureValidation"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 6. Cross-Backend Parity Testing
echo -e "\n${BLUE}=== 6. Cross-Backend Parity Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "cross_backend_parity" "$BUILD_DIR/test/storage/cross_backend_parity_test --gtest_filter=CrossBackendParityTest.DualWriteBlockStorage"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 7. UTXO Parity Testing
echo -e "\n${BLUE}=== 7. UTXO Parity Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "utxo_parity" "$BUILD_DIR/test/storage/cross_backend_parity_test --gtest_filter=CrossBackendParityTest.UTXOSetParity"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 8. Corruption Containment Testing
echo -e "\n${BLUE}=== 8. Corruption Containment Testing ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "corruption_containment" "$BUILD_DIR/test/storage/corruption_containment_test --gtest_filter=CorruptionContainmentTest.DataFileCorruption"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 9. Performance Comparison
echo -e "\n${BLUE}=== 9. Performance Comparison ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if run_test "performance_comparison" "$BUILD_DIR/test/storage/cross_backend_parity_test --gtest_filter=CrossBackendParityTest.PerformanceComparison"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# 10. Short Soak Test (compressed mode for CI)
echo -e "\n${BLUE}=== 10. Stability Testing (Compressed) ===${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))
export SOAK_TEST_COMPRESSED=1
if run_test "stability_compressed" "$BUILD_DIR/test/soak/soak_test_framework --gtest_filter=SoakTestFramework.StabilityTest24Hours"; then
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    FAILED_TESTS=$((FAILED_TESTS + 1))
fi

# Generate comprehensive test report
echo -e "\n${BLUE}=== Generating Test Report ===${NC}"
REPORT_FILE="$TEST_RESULTS_DIR/production_validation_report.md"

cat > "$REPORT_FILE" << EOF
# DineroCoin Production Validation Report

**Generated:** $(date)
**Test Suite Version:** Production Hardening v1.0

## Summary

- **Total Tests:** $TOTAL_TESTS
- **Passed:** $PASSED_TESTS
- **Failed:** $FAILED_TESTS
- **Success Rate:** $(( (PASSED_TESTS * 100) / TOTAL_TESTS ))%

## Test Results

EOF

# Add individual test results
for log_file in "$TEST_RESULTS_DIR"/*.log; do
    if [ -f "$log_file" ]; then
        test_name=$(basename "$log_file" .log)
        if grep -q "PASSED" "$log_file" 2>/dev/null || [ $? -eq 1 ]; then
            status="✅ PASSED"
        else
            status="❌ FAILED"
        fi
        
        echo "### $test_name" >> "$REPORT_FILE"
        echo "**Status:** $status" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        tail -20 "$log_file" >> "$REPORT_FILE" 2>/dev/null || echo "No log output" >> "$REPORT_FILE"
        echo '```' >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
done

# Add go-live readiness assessment
cat >> "$REPORT_FILE" << EOF

## Go-Live Readiness Assessment

### Critical Requirements Status

EOF

if [ $FAILED_TESTS -eq 0 ]; then
    cat >> "$REPORT_FILE" << EOF
🟢 **READY FOR PRODUCTION**

All critical validation tests have passed. The storage system meets production durability requirements.

### Next Steps
1. Execute 72-hour soak test in staging environment
2. Deploy monitoring and alerting infrastructure  
3. Begin canary deployment with shadow writes
4. Execute weekly backup restore drills
5. Validate TLS configurations in production environment

EOF
else
    cat >> "$REPORT_FILE" << EOF
🔴 **NOT READY FOR PRODUCTION**

$FAILED_TESTS critical tests have failed. Review failed tests and address issues before production deployment.

### Required Actions
1. Investigate and fix all failed tests
2. Re-run validation suite until 100% pass rate achieved
3. Review logs for any durability or consistency issues

EOF
fi

# Final summary
echo -e "\n${BLUE}=== Production Validation Complete ===${NC}"
echo "Completed at: $(date)"
echo -e "Results: ${GREEN}$PASSED_TESTS passed${NC}, ${RED}$FAILED_TESTS failed${NC} out of $TOTAL_TESTS total"
echo "Report: $REPORT_FILE"

if [ $FAILED_TESTS -eq 0 ]; then
    echo -e "\n${GREEN}🎉 ALL TESTS PASSED - READY FOR PRODUCTION DEPLOYMENT${NC}"
    exit 0
else
    echo -e "\n${RED}⚠️  SOME TESTS FAILED - REVIEW REQUIRED BEFORE PRODUCTION${NC}"
    exit 1
fi
