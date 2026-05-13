#!/bin/bash
set -euo pipefail

# Wallet release gates: must stay green before CI/release can proceed.
# Usage: ./scripts/wallet_release_gates.sh [build_dir]
# Local-only: this gate is not allowed to run in GitHub Actions.

BUILD_DIR="${1:-build}"

if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then
  echo "❌ Wallet release gates are local-only and must not run in GitHub Actions."
  exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "❌ Wallet gates failed: build directory not found: $BUILD_DIR"
  echo "   Build first, e.g.: cmake -S . -B $BUILD_DIR && cmake --build $BUILD_DIR"
  exit 1
fi

if ! command -v ctest >/dev/null 2>&1; then
  echo "❌ Wallet gates failed: ctest not found in PATH"
  exit 1
fi

WALLET_GATE_TESTS=(
  "BIP39"
  "WalletPathHygiene"
  "WalletFFIConcurrency"
  "WalletDeterminism"
  "WalletSidecarMigration"
  "WalletDescriptorActiveContext"
  "WalletPsbtPolicyContext"
)

echo "🧪 Wallet release gates"
echo "======================="
echo "Build dir: $BUILD_DIR"

echo ""
echo "🔎 Verifying gate tests are registered..."
for test_name in "${WALLET_GATE_TESTS[@]}"; do
  if ! ctest --test-dir "$BUILD_DIR" -N -R "^${test_name}$" 2>/dev/null | grep -q "Test #"; then
    echo "❌ Required wallet gate test not found: ${test_name}"
    exit 1
  fi
  echo "  ✅ ${test_name}"
done

echo ""
echo "🏃 Running wallet gate tests..."
for test_name in "${WALLET_GATE_TESTS[@]}"; do
  echo "  • ${test_name}"
  ctest --test-dir "$BUILD_DIR" -R "^${test_name}$" --output-on-failure
done

echo ""
echo "✅ Wallet release gates passed"
