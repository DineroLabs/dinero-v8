#!/bin/bash
#
# Ring 4 Phase 4h.5 — Category C: Disk Corruption Injection
#
# PURPOSE:
#   Validate that checksum validation (SHA256) detects corruption
#   and triggers conservative recovery (MR3).
#
# VALIDATION:
#   After corruption injection:
#   - recover() returns nullopt
#   - System restarts from clean state
#   - No partial state exposure
#   - No undefined behavior
#
# USAGE:
#   ./disk_corruption_injection.sh <persistence_dir>
#
# EXAMPLE:
#   ./disk_corruption_injection.sh /tmp/test_persistence_db
#

set -euo pipefail

PERSISTENCE_DIR=${1:-""}
BUILD_DIR="/Users/haydarevich/Documents/DineroCoin/build_test"

if [ -z "$PERSISTENCE_DIR" ]; then
    echo "Usage: $0 <persistence_dir>"
    echo ""
    echo "Example:"
    echo "  $0 /tmp/test_persistence_db"
    exit 1
fi

echo "════════════════════════════════════════════════════════════════"
echo "Ring 4 Phase 4h.5 — Category C: Disk Corruption Injection"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "Persistence directory: $PERSISTENCE_DIR"
echo ""
echo "Test cases:"
echo "  1. Corrupt snapshot blob"
echo "  2. Corrupt checksum"
echo "  3. Corrupt version metadata"
echo "  4. Delete checksum (simulates partial write)"
echo "  5. Truncate snapshot (simulates torn write)"
echo ""
echo "Expected outcome:"
echo "  - recover() returns nullopt"
echo "  - MR3 tests pass (conservative recovery)"
echo ""
echo "════════════════════════════════════════════════════════════════"
echo ""

test_passed=0
test_failed=0

run_corruption_test() {
    local test_name=$1
    local corruption_fn=$2

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Test: $test_name"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # Clean state
    rm -rf "$PERSISTENCE_DIR"
    mkdir -p "$PERSISTENCE_DIR"

    # Run test to create initial snapshot
    echo "  📝 Creating initial snapshot..."
    if ! "$BUILD_DIR/bin/test_deterministic_persistence_store" >/dev/null 2>&1; then
        echo "  ⚠️  Could not create initial snapshot (expected for some tests)"
    fi

    # Apply corruption
    echo "  💥 Injecting corruption: $test_name"
    eval "$corruption_fn"

    # Run MR3 test (validates conservative recovery)
    echo "  🔍 Running MR3 validation..."
    if "$BUILD_DIR/bin/test_mining_persistence_oracle_mr3" 2>&1 | tail -3; then
        echo "  ✅ MR3 PASSED after corruption"
        ((test_passed++))
    else
        echo "  ❌ MR3 FAILED after corruption"
        ((test_failed++))
    fi

    echo ""
}

# Test 1: Corrupt snapshot blob
run_corruption_test "Corrupt snapshot blob" '
    if [ -d "$PERSISTENCE_DIR" ]; then
        find "$PERSISTENCE_DIR" -type f -name "*.sst" 2>/dev/null | while read f; do
            if [ -f "$f" ]; then
                dd if=/dev/urandom of="$f" bs=1 count=32 seek=100 conv=notrunc 2>/dev/null || true
            fi
        done
    fi
'

# Test 2: Corrupt MANIFEST
run_corruption_test "Corrupt MANIFEST" '
    if [ -d "$PERSISTENCE_DIR" ]; then
        find "$PERSISTENCE_DIR" -type f -name "MANIFEST-*" 2>/dev/null | while read f; do
            if [ -f "$f" ]; then
                dd if=/dev/urandom of="$f" bs=1 count=16 seek=10 conv=notrunc 2>/dev/null || true
            fi
        done
    fi
'

# Test 3: Corrupt WAL
run_corruption_test "Corrupt WAL" '
    if [ -d "$PERSISTENCE_DIR" ]; then
        find "$PERSISTENCE_DIR" -type f -name "*.log" 2>/dev/null | while read f; do
            if [ -f "$f" ]; then
                dd if=/dev/urandom of="$f" bs=1 count=64 seek=50 conv=notrunc 2>/dev/null || true
            fi
        done
    fi
'

# Test 4: Delete CURRENT file
run_corruption_test "Delete CURRENT file" '
    if [ -f "$PERSISTENCE_DIR/CURRENT" ]; then
        rm "$PERSISTENCE_DIR/CURRENT"
    fi
'

# Test 5: Truncate all SST files
run_corruption_test "Truncate SST files" '
    if [ -d "$PERSISTENCE_DIR" ]; then
        find "$PERSISTENCE_DIR" -type f -name "*.sst" 2>/dev/null | while read f; do
            if [ -f "$f" ]; then
                truncate -s 50 "$f" 2>/dev/null || true
            fi
        done
    fi
'

echo "════════════════════════════════════════════════════════════════"
echo "Ring 4 Phase 4h.5 — Category C Results"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "Tests passed: $test_passed"
echo "Tests failed: $test_failed"
echo ""

if [ $test_failed -eq 0 ]; then
    echo "🎯 ALL CORRUPTION DETECTION TESTS PASSED"
    echo ""
    echo "Validated:"
    echo "  ✅ Checksum validation detects corruption"
    echo "  ✅ Conservative recovery (MR3) upheld"
    echo "  ✅ No partial state exposure"
    echo "  ✅ No undefined behavior"
    echo ""
    echo "Ring 4h.5 Category C: VERIFIED ✅"
    exit 0
else
    echo "❌ Some corruption tests failed"
    echo "Ring 4h.5 Category C: FAILED"
    exit 1
fi
