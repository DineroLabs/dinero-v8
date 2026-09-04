#!/bin/bash

# Build and run ReorgGuard Atomicity test (Day 2.2)

set -e

PROJECT_ROOT="/Users/haydarevich/Documents/DineroCoin"
TEST_DIR="$PROJECT_ROOT/tests/integration"
BUILD_DIR="$TEST_DIR/build"

mkdir -p "$BUILD_DIR"

echo "========================================="
echo "Building ReorgGuard Atomicity Test (Day 2.2)"
echo "========================================="

CXX="g++"
CXXFLAGS="-std=c++20 -Wall -Wextra -I$PROJECT_ROOT/include -I$PROJECT_ROOT -I$TEST_DIR"
LDFLAGS="-lpthread"

echo ""
echo "[1/1] Compiling test_reorg_guard_atomicity..."

$CXX $CXXFLAGS \
    "$TEST_DIR/test_reorg_guard_atomicity.cpp" \
    -o "$BUILD_DIR/test_reorg_guard" \
    $LDFLAGS

echo "✅ Build complete!"
echo ""
echo "========================================="
echo "Running Tests"
echo "========================================="

"$BUILD_DIR/test_reorg_guard"

TEST_EXIT_CODE=$?

echo ""
echo "========================================="
if [ $TEST_EXIT_CODE -eq 0 ]; then
    echo "✅ All tests PASSED"
else
    echo "❌ Some tests FAILED"
fi
echo "========================================="

exit $TEST_EXIT_CODE
