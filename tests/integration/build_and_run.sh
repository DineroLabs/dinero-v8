#!/bin/bash

# Build and run Phase 2 integration/reorg tests via CMake+CTest.
# Legacy standalone sources (utxo_set.cpp / activate_best_chain.cpp) were removed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build-release}"

echo "========================================="
echo "Building Phase 2 Integration Tests"
echo "========================================="
echo "Project root: $PROJECT_ROOT"
echo "Build dir:    $BUILD_DIR"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Configuring CMake build directory..."
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi

echo ""
echo "[1/2] Building reorg test binaries..."
cmake --build "$BUILD_DIR" --target \
    reorg_simulation_harness \
    test_premine_reorg \
    test_confidential_reorg \
    test_adversarial_ct_reorg_soak \
    -j8

echo ""
echo "[2/2] Running reorg integration tests..."
ctest --test-dir "$BUILD_DIR" --output-on-failure -R \
    "ReorgSimulation|PremineReorg|ConfidentialReorg|AdversarialReorg_"

echo ""
echo "========================================="
echo "✅ Phase 2 integration tests PASSED"
echo "========================================="
