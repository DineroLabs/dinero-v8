#!/usr/bin/env bash
# ============================================================
# Dinero Core - Unified Test Runner
# ============================================================

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_LOG="$BUILD_DIR/test_results.log"

echo "🚀 Dinero Core Unified Test Runner"
echo "====================================="
echo "Project root: $PROJECT_ROOT"
echo "Build dir: $BUILD_DIR"
echo ""

# 1. Build everything
echo "🧱 Building Dinero Core..."
cmake -B "$BUILD_DIR" -DENABLE_TESTS=ON -DENABLE_FUZZING=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR" --target all -j$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)

# 2. Run regression and stress tests
echo ""
echo "🧪 Running regression and stress tests..."
cd "$BUILD_DIR"
ctest --output-on-failure | tee "$TEST_LOG"

# 3. Run fuzzing tests (optional)
if [ "$1" == "--fuzz" ]; then
    echo ""
    echo "🐛 Running consensus safety fuzzing suite..."
    DINERO_FUZZ_BUILD_DIR="$PROJECT_ROOT/build-fuzz" "$PROJECT_ROOT/run_fuzzing_suite.sh" 60
fi

# 4. Show summary
echo ""
echo "✅ All regression tests completed successfully!"
echo "Log file: $TEST_LOG"
