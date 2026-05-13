#!/usr/bin/env bash
# Build + run wallet_sqlite_lifecycle under current env (DINERO_WALLET_SYNC, etc.)
set -euo pipefail

BUILD_DIR="${1:-build-test}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Sanitizers (platform-aware)
UNAME="$(uname || echo Unknown)"
if [[ "${UNAME}" == "Darwin" ]]; then
  export ASAN_OPTIONS="detect_leaks=0:strict_string_checks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1"
else
  export ASAN_OPTIONS="detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1:halt_on_error=1:abort_on_error=1"
fi
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

# Default runtime knobs (overridable by env)
export DINERO_WAL_CKPT="${DINERO_WAL_CKPT:-PASSIVE}"
export DINERO_SQL_TRACE="${DINERO_SQL_TRACE:-0}"
export DINERO_WALLET_SYNC="${DINERO_WALLET_SYNC:-NORMAL}"

# Configure if needed
if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
fi

# Build
cmake --build "${BUILD_DIR}" -j "$( (command -v sysctl >/dev/null && sysctl -n hw.ncpu) || (command -v nproc >/dev/null && nproc) || echo 3 )"

pushd "${BUILD_DIR}" >/dev/null

# Run test (repeat a couple times to smoke flakes)
echo "▶️  Running wallet_sqlite_lifecycle (DINERO_WALLET_SYNC=${DINERO_WALLET_SYNC}, DINERO_WAL_CKPT=${DINERO_WAL_CKPT})"
set +e
ctest -R wallet_sqlite_lifecycle --output-on-failure --repeat until-fail:3
RC=$?
set -e

# Generate markdown report
if [[ -x "${PROJECT_ROOT}/scripts/gen_sqlite_report.sh" ]]; then
  "${PROJECT_ROOT}/scripts/gen_sqlite_report.sh" "P0_SQLITE_COMPLETE.md" "${RC}"
else
  cat <<EOF > P0_SQLITE_COMPLETE.md
## 🗄️ SQLite Wallet Lifecycle — Results

- DINERO_WALLET_SYNC=${DINERO_WALLET_SYNC}
- DINERO_WAL_CKPT=${DINERO_WAL_CKPT}

**CTests exit code:** ${RC}
EOF
fi

if [[ $RC -eq 0 ]]; then
  echo "✅ SQLite lifecycle PASSED (see ${BUILD_DIR}/P0_SQLITE_COMPLETE.md)"
else
  echo "❌ SQLite lifecycle FAILED (see logs and ${BUILD_DIR}/P0_SQLITE_COMPLETE.md)"
fi

popd >/dev/null
exit "${RC}"
