#!/usr/bin/env bash
# One-command local/CI runner: configure + build + run P0 tests + generate markdown + summary
set -euo pipefail

BUILD_DIR="${1:-build-test}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --- Sanitizer env (platform-aware defaults) ---
UNAME="$(uname || echo Unknown)"
if [[ "${UNAME}" == "Darwin" ]]; then
  export ASAN_OPTIONS="detect_leaks=0:strict_string_checks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1"
else
  export ASAN_OPTIONS="detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1"
fi
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

# --- Configure if needed ---
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
fi

# --- Build ---
cmake --build "${BUILD_DIR}" -j "$( (command -v sysctl >/dev/null && sysctl -n hw.ncpu) || (command -v nproc >/dev/null && nproc) || echo 3 )"

# --- Run P0 suite (7 tests) using labels ---
pushd "${BUILD_DIR}" >/dev/null

echo "▶️  Running P0 suite with CTest..."
set +e
ctest -L p0 --output-on-failure
CTEST_RC=$?
set -e

# --- Generate markdown report (always) ---
if [[ -x "${PROJECT_ROOT}/scripts/gen_p0_report.sh" ]]; then
  "${PROJECT_ROOT}/scripts/gen_p0_report.sh" "P0_CRYPTO_COMPLETE.md"
else
  # Minimal inline report if generator is missing
  cat <<'EOF' > P0_CRYPTO_COMPLETE.md
## 🎯 P0 Crypto Test Suite - Results

(Inline report: scripts/gen_p0_report.sh not found)
EOF
fi

# Append live summary (pass/fail) to report
{
  echo
  echo "### Live run on $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  pass=0; total=0
  for t in test_crypto_vectors test_bip39_seed_kat test_slip132_prefix test_descriptor_roundtrip test_bip84_bech32_roundtrip test_bip32_fingerprint test_p2wpkh_script; do
    total=$((total+1))
    if "./bin/$t" >/dev/null 2>&1; then
      echo "✅ $t"
      pass=$((pass+1))
    else
      echo "❌ $t (see ./bin/$t output)"
    fi
  done
  echo
  echo "### Summary: $pass/$total tests passed"
  echo "_Generated on $(date -u '+%Y-%m-%d %H:%M:%S UTC')_"
} >> P0_CRYPTO_COMPLETE.md

# Print concise console summary
if [[ $CTEST_RC -eq 0 ]]; then
  echo "✅ P0 suite PASSED (see ${BUILD_DIR}/P0_CRYPTO_COMPLETE.md)"
else
  echo "❌ P0 suite FAILED (see logs and ${BUILD_DIR}/P0_CRYPTO_COMPLETE.md)"
fi

popd >/dev/null

exit "${CTEST_RC}"
