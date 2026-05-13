#!/bin/bash
#
# Ring 4 Phase 4h.5 — Category A: Kill-9 During Persist
# Ring 4 Phase 4h.5 — Category D: Crash Storm
#
# PURPOSE:
#   Validate that RocksDB WriteBatch atomicity + WAL durability
#   hold under SIGKILL at arbitrary instruction windows.
#
# VALIDATION:
#   After each crash, run MR1-MR5 tests unchanged.
#   Expected: Either old snapshot OR new snapshot loads cleanly.
#   Never partial, never corrupted, never duplicated.
#
# USAGE:
#   ./crash_kill9_loop.sh <iterations> <test_executable>
#
# EXAMPLE:
#   ./crash_kill9_loop.sh 100 /path/to/test_mining_persistence_oracle_mr3
#

set -euo pipefail

ITERATIONS=${1:-50}
TEST_EXECUTABLE=${2:-""}
BUILD_DIR="/Users/haydarevich/Documents/DineroCoin/build_test"

if [ -z "$TEST_EXECUTABLE" ]; then
    echo "Usage: $0 <iterations> <test_executable>"
    echo ""
    echo "Example:"
    echo "  $0 100 $BUILD_DIR/bin/test_mining_persistence_oracle_mr3"
    exit 1
fi

if [ ! -f "$TEST_EXECUTABLE" ]; then
    echo "Error: Test executable not found: $TEST_EXECUTABLE"
    exit 1
fi

echo "════════════════════════════════════════════════════════════════"
echo "Ring 4 Phase 4h.5 — OS-Level Crash Testing"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "Test executable: $TEST_EXECUTABLE"
echo "Iterations:      $ITERATIONS"
echo "Strategy:        Random SIGKILL during persist operations"
echo ""
echo "Validation:"
echo "  - MR3: Partial persistence recovers safely"
echo "  - MR4: Restart converges to valid state"
echo "  - No partial state, no corruption, no duplication"
echo ""
echo "════════════════════════════════════════════════════════════════"
echo ""

total_crashes=0
total_recoveries=0
failed_tests=0

for i in $(seq 1 $ITERATIONS); do
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Iteration $i/$ITERATIONS"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Run test in background
    "$TEST_EXECUTABLE" &
    TEST_PID=$!

    # Random sleep (0-500ms) to catch different execution windows
    SLEEP_MS=$((RANDOM % 500))
    sleep 0.$(printf "%03d" $SLEEP_MS)

    # Send SIGKILL if process still running
    if kill -0 $TEST_PID 2>/dev/null; then
        echo "  💥 Sending SIGKILL to PID $TEST_PID after ${SLEEP_MS}ms"
        kill -9 $TEST_PID 2>/dev/null || true
        wait $TEST_PID 2>/dev/null || true
        ((total_crashes++))
    else
        echo "  ✅ Test completed before SIGKILL"
        wait $TEST_PID
        if [ $? -eq 0 ]; then
            ((total_recoveries++))
        else
            echo "  ❌ Test failed naturally (not crash-related)"
            ((failed_tests++))
        fi
        continue
    fi

    # Brief pause to let OS flush
    sleep 0.1

    # Now run the SAME test again to validate recovery
    echo "  🔄 Running recovery validation..."
    if "$TEST_EXECUTABLE" 2>&1 | tail -3; then
        echo "  ✅ Recovery validation PASSED"
        ((total_recoveries++))
    else
        echo "  ❌ Recovery validation FAILED"
        ((failed_tests++))
        echo ""
        echo "FATAL: Recovery failed after crash in iteration $i"
        echo "This violates MR3/MR4 guarantees."
        exit 1
    fi

    echo ""
done

echo "════════════════════════════════════════════════════════════════"
echo "Ring 4 Phase 4h.5 — Category A Results"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "Total iterations:     $ITERATIONS"
echo "Crashes injected:     $total_crashes"
echo "Successful recoveries: $total_recoveries"
echo "Failed tests:         $failed_tests"
echo ""

if [ $failed_tests -eq 0 ]; then
    echo "🎯 ALL CRASH RECOVERY TESTS PASSED"
    echo ""
    echo "Validated:"
    echo "  ✅ RocksDB WriteBatch atomicity"
    echo "  ✅ WAL durability under SIGKILL"
    echo "  ✅ Conservative recovery (MR3)"
    echo "  ✅ State convergence (MR4)"
    echo ""
    echo "Ring 4h.5 Category A: VERIFIED ✅"
    exit 0
else
    echo "❌ Some recovery tests failed"
    echo "Ring 4h.5 Category A: FAILED"
    exit 1
fi
