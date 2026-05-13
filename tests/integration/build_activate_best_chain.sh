#!/bin/bash

# Build and run Phase 2 activation/reorg harness.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build-release}"

echo "========================================="
echo "Building Phase 2 ActivateBestChain Harness"
echo "========================================="
echo "Project root: $PROJECT_ROOT"
echo "Build dir:    $BUILD_DIR"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Configuring CMake build directory..."
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi

echo ""
echo "[1/2] Building reorg_simulation_harness..."
cmake --build "$BUILD_DIR" --target reorg_simulation_harness -j8

echo ""
echo "[2/2] Running ReorgSimulation..."
ctest --test-dir "$BUILD_DIR" --output-on-failure -R "ReorgSimulation"

echo ""
echo "========================================="
echo "✅ Phase 2 activation harness PASSED"
echo "========================================="
