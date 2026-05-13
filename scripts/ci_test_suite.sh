#!/bin/bash
# CI Test Configuration for Dinero v1.0.0
# Run this script in CI to validate economics lock-in
# Local-only: must not run in GitHub Actions.

set -euo pipefail

if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
    echo "❌ ci_test_suite.sh is local-only and must not run in GitHub Actions."
    exit 1
fi

echo "🧪 Dinero v1.0.0 CI Test Suite"
echo "================================"

# Build all targets
echo "🔨 Building targets..."
cmake --build build --target dinerod dinero-cli test_block_time_economics test_reward_schedule -j4

# Core economics tests
echo ""
echo "🧪 Running core economics tests..."
ctest --test-dir build -R "(block_time_economics|reward_schedule)" --output-on-failure

# Wallet release gates (strict)
echo ""
echo "🧪 Running wallet release gates..."
./scripts/wallet_release_gates.sh build

# Smoke tests
echo ""
echo "🧪 Running smoke tests..."
ctest --test-dir build -R "(smoke_daemon|smoke_rpc)" --output-on-failure

# CLI integration tests
echo ""
echo "🧪 Running CLI integration tests..."
ctest --test-dir build -R "(cli_integration|ConnectionResolver|NodeinfoValidator)" --output-on-failure

# Manual smoke test with custom ports
echo ""
echo "🧪 Running manual smoke test..."
NETWORK=regtest RPC_PORT=21090 ./scripts/smoke_daemon.sh

# Version verification
echo ""
echo "🧪 Verifying versions..."
./build/bin/dinerod --help | grep -q "Dinero daemon options" || (echo "❌ Daemon help failed" && exit 1)
./build/bin/dinero-cli --version | grep -q "v1.0.0" || (echo "❌ CLI version mismatch" && exit 1)

# HRP consistency check
echo ""
echo "🧪 Verifying HRP consistency..."
timeout 5s ./build/bin/dinerod --regtest --datadir=/tmp/ci-hrp-test --printtoconsole 2>&1 | grep -q "HRP: rdin" || (echo "❌ Regtest HRP incorrect" && exit 1)

echo ""
echo "🎉 All CI tests passed! Economics are locked-in and ready for release."
