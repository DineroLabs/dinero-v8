#!/usr/bin/env bash
set -euo pipefail

echo "🔄 Running reorg smoke test..."
echo "Binary dir: ${PWD}"
echo "Work dir: ${PWD}"

# Set environment variables for the test
DIN_BIN="${DINERO_BIN:-${BIN:-${PWD}/bin/dinerod}}"
export DIN_BIN

# Run the Python reorg test with absolute path
cd "${CMAKE_SOURCE_DIR:-$(dirname "$0")/..}"
exec python3 tests/e2e/test_reorg.py

echo "✅ Reorg test completed successfully"
