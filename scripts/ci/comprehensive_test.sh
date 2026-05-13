#!/usr/bin/env bash
set -euo pipefail

# CI Comprehensive Test Script
# Runs both Release and ASAN builds with smoke tests
# Exits with non-zero if any test fails

echo "🚀 === DineroCoin CI Comprehensive Test Suite ==="
echo "Testing both Release and ASAN builds..."
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# Test results tracking
TESTS_PASSED=0
TESTS_TOTAL=0
FAILED_TESTS=()

# Helper function to run a test and track results
run_test() {
    local test_name="$1"
    local test_command="$2"
    
    echo "🧪 Running: $test_name"
    echo "   Command: $test_command"
    
    if eval "$test_command"; then
        echo "✅ $test_name: PASSED"
        ((TESTS_PASSED++))
    else
        echo "❌ $test_name: FAILED"
        FAILED_TESTS+=("$test_name")
    fi
    ((TESTS_TOTAL++))
    echo ""
}

# Clean up any running processes
echo "🧹 Cleaning up any running dinerod processes..."
pkill -f dinerod || true
sleep 1

# === BUILD PHASE ===
echo "🔨 === Build Phase ==="

# Build Release version
run_test "Release Build" "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDIN_ENABLE_ASAN=OFF && cmake --build build --target dinerod -j4"

# Build ASAN version
run_test "ASAN Build" "cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDIN_ENABLE_ASAN=ON && cmake --build build-asan --target dinerod -j4"

# === TEST PHASE ===
echo "🧪 === Test Phase ==="

# Test Release build with smoke test
run_test "Release Smoke Test" "BIN=./build/dinerod scripts/dev/smoke.sh ./ci_test_release 22000 22001"

# Test ASAN build with smoke test
run_test "ASAN Smoke Test" "BIN=./build-asan/dinerod scripts/dev/smoke.sh ./ci_test_asan 22002 22003"

# Test durability with Release build
run_test "Release Durability Test" "BIN=./build/dinerod scripts/dev/smoke.sh ./ci_test_durability 22004 22005 true"

# Test mining E2E with ASAN build
run_test "ASAN Mining E2E Test" "BIN=./build-asan/dinerod scripts/dev/mining_e2e.sh ./ci_test_mining_asan 22006 22007"

# Test path regression
run_test "Path Regression Test" "BIN=./build/dinerod scripts/dev/regression_paths.sh ./ci_test_paths"

# Test CIC mismatch detection
run_test "CIC Validation Test" "timeout 10s ./build/dinerod --datadir=./ci_test_cic_fail --regtest --printtoconsole 2>&1 | grep -q 'Chain Identity Check' || exit 1"

# === UNIT TESTS ===
echo "🔬 === Unit Test Phase ==="

# Test target roundtrip (if available)
if [ -f "./build/test_target_roundtrip" ]; then
    run_test "Target Roundtrip Unit Test" "./build/test_target_roundtrip"
fi

# === CLEANUP ===
echo "🧹 Cleaning up test data..."
pkill -f dinerod || true
rm -rf ./ci_test_* || true

# === RESULTS ===
echo "📊 === Test Results Summary ==="
echo "Tests passed: $TESTS_PASSED/$TESTS_TOTAL"
echo ""

if [ "$TESTS_PASSED" -eq "$TESTS_TOTAL" ]; then
    echo "🎉 ALL TESTS PASSED! CI SUCCESS"
    echo "✅ Release build: Working"
    echo "✅ ASAN build: Working" 
    echo "✅ Smoke tests: Passing"
    echo "✅ Durability: Verified"
    echo "✅ Mining E2E: Functional"
    echo "✅ Path hardening: Secure"
    echo "✅ CIC validation: Protected"
    exit 0
else
    echo "💥 SOME TESTS FAILED! CI FAILURE"
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
    done
    echo ""
    echo "Check the logs above for details."
    exit 1
fi
