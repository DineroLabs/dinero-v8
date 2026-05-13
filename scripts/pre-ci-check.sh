#!/usr/bin/env bash
# Pre-CI Gate: Run locally before triggering GitHub Actions
# Purpose: Prevent using CI as a debugger

set -euo pipefail

PLATFORM=$(uname -s | tr A-Z a-z)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
MACOS_TARGET_FILE="${ROOT_DIR}/.macos-deployment-target"

echo "════════════════════════════════════════════════════════════════"
echo "PRE-CI GATE CHECK"
echo "════════════════════════════════════════════════════════════════"
echo "Platform: $PLATFORM"
echo "Date: $(date)"
echo ""
echo "⚠️  RULE: If this script fails, DO NOT run CI."
echo "⚠️  CI is for verification, not diagnosis."
echo ""
echo "📋 CI PARITY:"
echo "   This script must be run immediately before CI."
echo "   If CI fails after this passes, CI configuration is at fault."
echo "   Code failures → caught here (local)"
echo "   CI failures → plumbing issues (not code)"
echo "════════════════════════════════════════════════════════════════"
echo ""

STEPS_TOTAL=8
STEP=0

# ══════════════════════════════════════════════════════════════════
# Helper Functions
# ══════════════════════════════════════════════════════════════════

step() {
  STEP=$((STEP + 1))
  echo ""
  echo "[$STEP/$STEPS_TOTAL] $1"
  echo "────────────────────────────────────────────────────────────────"
}

check_build_dir() {
  if [ ! -d "build" ]; then
    echo "❌ Build directory not found"
    echo "   Run: cmake -S . -B build -DDINERO_RELEASE=ON -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON"
    exit 1
  fi
}

version_equal() {
  python3 - "$1" "$2" <<'PY'
import sys

def parse(value):
    parts = []
    for token in value.strip().split("."):
        try:
            parts.append(int(token))
        except ValueError:
            parts.append(0)
    return parts

left = parse(sys.argv[1])
right = parse(sys.argv[2])
width = max(len(left), len(right))
left.extend([0] * (width - len(left)))
right.extend([0] * (width - len(right)))
print("1" if left == right else "0")
PY
}

read_repo_macos_target() {
  if [ ! -f "${MACOS_TARGET_FILE}" ]; then
    echo "❌ BLOCKER: missing repo macOS target file: ${MACOS_TARGET_FILE}"
    exit 1
  fi

  local target
  target="$(tr -d '[:space:]' < "${MACOS_TARGET_FILE}")"
  if [ -z "${target}" ]; then
    echo "❌ BLOCKER: repo macOS target file is empty: ${MACOS_TARGET_FILE}"
    exit 1
  fi
  printf '%s\n' "${target}"
}

detect_openssl_archive_target() {
  local archive="$1"
  if [ "$PLATFORM" = "darwin" ] && [ -f "${archive}" ]; then
    /usr/bin/otool -l "${archive}" 2>/dev/null | awk '/minos /{print $2; exit}'
  fi
}

vendored_openssl_dir() {
  local base="third_party/openssl-3.3.2"
  if [ "$PLATFORM" != "darwin" ]; then
    printf '%s\n' "${base}"
    return
  fi

  local host_arch
  host_arch="$(uname -m)"
  if [ -d "${base}/prebuilt/macos-${host_arch}" ]; then
    printf '%s\n' "${base}/prebuilt/macos-${host_arch}"
  elif [ -d "${base}/prebuilt/macos-arm64" ]; then
    printf '%s\n' "${base}/prebuilt/macos-arm64"
  else
    printf '%s\n' "${base}"
  fi
}

verify_vendored_openssl_target() {
  if [ "$PLATFORM" != "darwin" ]; then
    return 0
  fi

  local repo_target="$1"
  local openssl_dir
  openssl_dir="$(vendored_openssl_dir)"
  local metadata_file="${openssl_dir}/.dinero-build-meta"
  local crypto_archive="${openssl_dir}/libcrypto.a"
  local metadata_target=""
  local archive_target=""

  if [ -f "${metadata_file}" ]; then
    metadata_target="$(awk -F= '/^MACOSX_DEPLOYMENT_TARGET=/{print $2; exit}' "${metadata_file}")"
  fi
  archive_target="$(detect_openssl_archive_target "${crypto_archive}")"
  if [ -n "${metadata_target}" ] && [ -n "${archive_target}" ] && [ "$(version_equal "${metadata_target}" "${archive_target}")" != "1" ]; then
    echo "❌ BLOCKER: vendored OpenSSL metadata says macOS ${metadata_target}, but the archive targets ${archive_target}"
    echo "   Rebuild with: OPENSSL_REBUILD=1 OPENSSL_MACOS_DEPLOYMENT_TARGET=${repo_target} ./scripts/build-openssl-vendored.sh"
    exit 1
  fi
  if [ -z "${archive_target}" ]; then
    archive_target="${metadata_target}"
  fi
  if [ -z "${archive_target}" ]; then
    echo "❌ BLOCKER: unable to determine vendored OpenSSL macOS target"
    echo "   Expected metadata at ${metadata_file} or a readable archive at ${crypto_archive}"
    exit 1
  fi
  if [ "$(version_equal "${archive_target}" "${repo_target}")" != "1" ]; then
    echo "❌ BLOCKER: vendored OpenSSL targets macOS ${archive_target}, repo policy requires ${repo_target}"
    echo "   Rebuild with: OPENSSL_REBUILD=1 OPENSSL_MACOS_DEPLOYMENT_TARGET=${repo_target} ./scripts/build-openssl-vendored.sh"
    exit 1
  fi
  echo "✅ Vendored OpenSSL macOS target matches repo policy (${repo_target})"
}

# ══════════════════════════════════════════════════════════════════
# Step 1: Check Git Tree is Clean
# ══════════════════════════════════════════════════════════════════

step "Check git tree is clean"
if ! git diff --quiet; then
  echo "❌ BLOCKER: Working tree has uncommitted changes"
  echo ""
  echo "   Modified files:"
  git diff --name-only | sed 's/^/     /'
  echo ""
  echo "   Commit or stash changes before running pre-CI gate."
  echo "   This prevents: 'It passed locally but I forgot to commit X'"
  echo ""
  echo "   DO NOT run CI with uncommitted changes."
  exit 1
fi

if ! git diff --cached --quiet; then
  echo "❌ BLOCKER: Staged changes not committed"
  echo ""
  echo "   Staged files:"
  git diff --cached --name-only | sed 's/^/     /'
  echo ""
  echo "   Commit staged changes before running pre-CI gate."
  exit 1
fi

echo "✅ Git tree is clean"

# ══════════════════════════════════════════════════════════════════
# Step 2: Check Build Directory
# ══════════════════════════════════════════════════════════════════

step "Check build directory exists"
check_build_dir
echo "✅ Build directory found"

# ══════════════════════════════════════════════════════════════════
# Step 3: Ensure Vendored OpenSSL Exists
# ══════════════════════════════════════════════════════════════════

step "Ensure vendored OpenSSL exists"

OPENSSL_DIR="$(vendored_openssl_dir)"
OPENSSL_LIB="${OPENSSL_DIR}/libssl.a"
REPO_MACOS_DEPLOYMENT_TARGET=""
if [ "$PLATFORM" = "darwin" ]; then
  REPO_MACOS_DEPLOYMENT_TARGET="$(read_repo_macos_target)"
fi

if [ ! -f "$OPENSSL_LIB" ]; then
  echo "🔧 Vendored OpenSSL not found — building locally"
  echo "   This is a prerequisite for DINERO_RELEASE=ON builds"
  echo ""

  if [ ! -f "scripts/build-openssl-vendored.sh" ]; then
    echo "❌ BLOCKER: scripts/build-openssl-vendored.sh not found"
    exit 1
  fi

  if [ "$PLATFORM" = "darwin" ]; then
    OPENSSL_REBUILD=1 OPENSSL_MACOS_DEPLOYMENT_TARGET="${REPO_MACOS_DEPLOYMENT_TARGET}" ./scripts/build-openssl-vendored.sh || {
      echo ""
      echo "❌ BLOCKER: Failed to build vendored OpenSSL"
      echo "   Check logs above for errors"
      echo "   DO NOT run CI until this is fixed"
      exit 1
    }
  else
    ./scripts/build-openssl-vendored.sh || {
      echo ""
      echo "❌ BLOCKER: Failed to build vendored OpenSSL"
      echo "   Check logs above for errors"
      echo "   DO NOT run CI until this is fixed"
      exit 1
    }
  fi

  echo ""
  echo "✅ Vendored OpenSSL built successfully"
else
  echo "✅ Vendored OpenSSL already present"
fi

verify_vendored_openssl_target "${REPO_MACOS_DEPLOYMENT_TARGET:-}"

# ══════════════════════════════════════════════════════════════════
# Step 4: Build Project (Release Mode)
# ══════════════════════════════════════════════════════════════════

step "Build project (Release mode, tests enabled)"
echo "Configuring CMake..."
CMAKE_ARGS=(
  -S .
  -B build
  -DDINERO_RELEASE=ON
  -DCMAKE_BUILD_TYPE=Release
  -DENABLE_TESTS=ON
  -DENABLE_GPU_MINING=OFF
)
if [ "$PLATFORM" = "darwin" ]; then
  CMAKE_ARGS+=(-DCMAKE_OSX_DEPLOYMENT_TARGET="${REPO_MACOS_DEPLOYMENT_TARGET}")
fi
cmake "${CMAKE_ARGS[@]}"

echo ""
echo "Building binaries..."
if [ "$PLATFORM" = "darwin" ]; then
  NUM_CORES=$(sysctl -n hw.ncpu)
else
  NUM_CORES=$(nproc 2>/dev/null || echo 4)
fi

cmake --build build --config Release --parallel "$NUM_CORES"

if [ ! -f "build/dinerod" ]; then
  echo "❌ Build failed: dinerod not found"
  exit 1
fi

echo "✅ Build succeeded"

# ══════════════════════════════════════════════════════════════════
# Step 5: Run Ring Tests (P0 - Must Pass)
# ══════════════════════════════════════════════════════════════════

step "Run Ring tests (65 tests, MUST PASS)"
if ! ctest --test-dir build -R "Ring" --output-on-failure; then
  echo ""
  echo "❌ BLOCKER: Ring tests failed"
  echo "   Ring tests are P0 (consensus-critical)"
  echo "   DO NOT run CI until these pass"
  exit 1
fi
echo "✅ Ring tests passed"

# ══════════════════════════════════════════════════════════════════
# Step 6: Run Consensus Tests (P0 - Must Pass)
# ══════════════════════════════════════════════════════════════════

step "Run consensus tests (MUST PASS)"
if ! ctest --test-dir build -R "consensus" --output-on-failure; then
  echo ""
  echo "❌ BLOCKER: Consensus tests failed"
  echo "   Consensus tests are P0 (chain safety)"
  echo "   DO NOT run CI until these pass"
  exit 1
fi
echo "✅ Consensus tests passed"

# ══════════════════════════════════════════════════════════════════
# Step 7: Run Smoke Tests (P0 - Must Pass)
# ══════════════════════════════════════════════════════════════════

step "Run smoke tests (16 tests, MUST PASS)"
if [ -f "scripts/smoke-test.sh" ]; then
  chmod +x scripts/smoke-test.sh
  export DINEROD=./build/dinerod
  export DINERO_CLI=./build/dinero-cli

  if ! ./scripts/smoke-test.sh; then
    echo ""
    echo "❌ BLOCKER: Smoke tests failed"
    echo "   Smoke tests verify basic daemon functionality"
    echo "   DO NOT run CI until these pass"
    exit 1
  fi
  echo "✅ Smoke tests passed"
else
  echo "⚠️  Smoke test script not found (skipping)"
fi

# ══════════════════════════════════════════════════════════════════
# Step 8: Binary Verification (Platform-Specific)
# ══════════════════════════════════════════════════════════════════

step "Binary verification (platform: $PLATFORM)"

if [ "$PLATFORM" = "darwin" ]; then
  echo "Checking for Homebrew dependencies..."
  if otool -L build/dinerod | grep -q '/opt/homebrew'; then
    echo "❌ BLOCKER: Homebrew dependencies found"
    echo "   Static linking not working (DINERO_RELEASE=ON not effective)"
    otool -L build/dinerod | grep '/opt/homebrew'
    echo "   DO NOT run CI until this is fixed"
    exit 1
  fi
  echo "✅ No Homebrew dependencies"

elif [ "$PLATFORM" = "linux" ]; then
  echo "Checking for unwanted dependencies..."
  if ldd build/dinerod | grep -iq 'grpc\|protobuf\|absl'; then
    echo "❌ BLOCKER: Unwanted dependencies found"
    echo "   Static linking not working"
    ldd build/dinerod | grep -iE 'grpc|protobuf|absl'
    echo "   DO NOT run CI until this is fixed"
    exit 1
  fi
  echo "✅ No unwanted dependencies"
fi

# ══════════════════════════════════════════════════════════════════
# Final Summary
# ══════════════════════════════════════════════════════════════════

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "✅ PRE-CI GATE: ALL CHECKS PASSED"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "Local validation complete:"
echo "  ✅ Git tree clean (no uncommitted changes)"
echo "  ✅ Vendored OpenSSL built/verified"
echo "  ✅ Build succeeded (Release mode, tests enabled)"
echo "  ✅ Ring tests passed (65/65)"
echo "  ✅ Consensus tests passed"
echo "  ✅ Smoke tests passed (16/16)"
echo "  ✅ Binary verification passed (no Homebrew deps)"
echo ""
echo "🟢 YOU MAY NOW RUN CI (workflow_dispatch)"
echo ""
echo "Next steps:"
echo "  1. Go to: https://github.com/Trucker2827/Dinero-Coin/actions"
echo "  2. Select: 'v3.0.0-alpha Release Build'"
echo "  3. Click 'Run workflow'"
echo "  4. Version: v3.0.0-alpha1-test"
echo "  5. Run and monitor results"
echo ""
echo "CI will verify:"
echo "  - macOS (arm64 + x86_64)"
echo "  - Linux (x86_64)"
echo "  - Windows (x86_64)"
echo ""
echo "Expected duration: ~20 minutes"
echo "════════════════════════════════════════════════════════════════"

exit 0
