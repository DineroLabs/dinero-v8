#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# DineroCoin Hermetic Build Script
# ═══════════════════════════════════════════════════════════════════════════
# Purpose: Sealed environment for reproducible builds
# Usage:   ./scripts/hermetic-build.sh [target]
#
# This script eliminates environmental degrees of freedom by:
#   1. Resetting PATH to system-only binaries
#   2. Unsetting all compiler/linker environment variables
#   3. Setting locale/timezone to deterministic values
#   4. Logging all build inputs for auditability
#   5. Enforcing hermetic toolchain file usage
#
# Design philosophy: "If it's not explicit, it doesn't exist"
# ═══════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Color Output (for logging)
# ─────────────────────────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
  echo -e "${BLUE}🔒 [HERMETIC]${NC} $*"
}

log_success() {
  echo -e "${GREEN}✅ [HERMETIC]${NC} $*"
}

log_warn() {
  echo -e "${YELLOW}⚠️  [HERMETIC]${NC} $*"
}

log_error() {
  echo -e "${RED}❌ [HERMETIC]${NC} $*" >&2
}

# ─────────────────────────────────────────────────────────────────────────────
# Script Configuration
# ─────────────────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build_hermetic"
TARGET="${1:-all}"

log_info "Starting hermetic build"
log_info "Project root: ${PROJECT_ROOT}"
log_info "Build directory: ${BUILD_DIR}"
log_info "Target: ${TARGET}"

# ─────────────────────────────────────────────────────────────────────────────
# Build Tools: Explicit Pinning (NOT Ambient)
# ─────────────────────────────────────────────────────────────────────────────
# CMake and Make are build inputs, not system utilities
# Pin them explicitly to avoid ambient PATH pollution

# Pin CMake (explicit, platform-aware)
if [[ -x "/Applications/CMake.app/Contents/bin/cmake" ]]; then
  # macOS official installer
  CMAKE_BIN="/Applications/CMake.app/Contents/bin/cmake"
elif [[ -x "/opt/homebrew/bin/cmake" ]]; then
  # macOS Homebrew (Apple Silicon)
  CMAKE_BIN="/opt/homebrew/bin/cmake"
elif [[ -x "/usr/local/bin/cmake" ]]; then
  # macOS Homebrew (Intel) or custom installs
  CMAKE_BIN="/usr/local/bin/cmake"
elif [[ -x "/usr/bin/cmake" ]]; then
  # Linux (system package manager)
  CMAKE_BIN="/usr/bin/cmake"
else
  log_error "CMake not found. Expected at one of:
    - /Applications/CMake.app/Contents/bin/cmake
    - /opt/homebrew/bin/cmake
    - /usr/local/bin/cmake
    - /usr/bin/cmake"
  exit 1
fi

# Verify CMake works
if ! "$CMAKE_BIN" --version >/dev/null 2>&1; then
  log_error "CMake at ${CMAKE_BIN} is broken"
  exit 1
fi

log_info "CMake pinned: ${CMAKE_BIN}"

# Pin Make
MAKE_BIN="/usr/bin/make"
if [[ ! -x "${MAKE_BIN}" ]]; then
  log_error "Make not found at ${MAKE_BIN}"
  exit 1
fi

log_info "Make pinned: ${MAKE_BIN}"

# ─────────────────────────────────────────────────────────────────────────────
# Tool Version Validation: Pinned → Proven
# ─────────────────────────────────────────────────────────────────────────────
# Validate that tools match BUILD_INVARIANTS.md (not just pinned, but verified)

log_info "Validating tool versions..."

# Expected versions (from BUILD_INVARIANTS.md)
EXPECTED_CMAKE_VERSION="3.31.6"
EXPECTED_CLANG_VERSION="17.0.0"

# Validate CMake version
CMAKE_VERSION=$("$CMAKE_BIN" --version | head -1 | awk '{print $3}')
if [[ "$CMAKE_VERSION" != "$EXPECTED_CMAKE_VERSION" ]]; then
  log_error "CMake version mismatch"
  log_error "  Expected: ${EXPECTED_CMAKE_VERSION}"
  log_error "  Found:    ${CMAKE_VERSION}"
  log_error "  Update BUILD_INVARIANTS.md or install expected version"
  exit 1
fi
log_info "  CMake ${CMAKE_VERSION} ✓"

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Selection (Explicit, Platform-Aware)
# ─────────────────────────────────────────────────────────────────────────────

if [[ -x "/usr/bin/clang" ]]; then
  CC_BIN="/usr/bin/clang"
  CXX_BIN="/usr/bin/clang++"
  COMPILER_NAME="clang"
elif [[ -x "/usr/bin/gcc" ]]; then
  CC_BIN="/usr/bin/gcc"
  CXX_BIN="/usr/bin/g++"
  COMPILER_NAME="gcc"
else
  log_error "No supported compiler found. Expected one of:
    - /usr/bin/clang
    - /usr/bin/gcc"
  exit 1
fi

log_info "Compiler selected: ${COMPILER_NAME}"
log_info "  CC:  ${CC_BIN}"
log_info "  CXX: ${CXX_BIN}"

# Validate compiler version
if [[ "$COMPILER_NAME" == "clang" ]]; then
  COMPILER_VERSION=$("$CC_BIN" --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  if [[ "$COMPILER_VERSION" != "$EXPECTED_CLANG_VERSION" ]]; then
    log_error "Clang version mismatch"
    log_error "  Expected: ${EXPECTED_CLANG_VERSION}"
    log_error "  Found:    ${COMPILER_VERSION}"
    exit 1
  fi
  log_info "  Clang ${COMPILER_VERSION} ✓"
elif [[ "$COMPILER_NAME" == "gcc" ]]; then
  COMPILER_VERSION=$("$CC_BIN" --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  EXPECTED_GCC_VERSION="15.2.0"
  # GCC version check - allow minor version differences
  GCC_MAJOR=$(echo "$COMPILER_VERSION" | cut -d. -f1)
  EXPECTED_GCC_MAJOR=$(echo "$EXPECTED_GCC_VERSION" | cut -d. -f1)
  if [[ "$GCC_MAJOR" != "$EXPECTED_GCC_MAJOR" ]]; then
    log_error "GCC major version mismatch"
    log_error "  Expected: ${EXPECTED_GCC_VERSION} (major: ${EXPECTED_GCC_MAJOR})"
    log_error "  Found:    ${COMPILER_VERSION} (major: ${GCC_MAJOR})"
    exit 1
  fi
  log_info "  GCC ${COMPILER_VERSION} ✓"
fi

# Export compiler paths explicitly
export CC="${CC_BIN}"
export CXX="${CXX_BIN}"

# Validate SDK version (macOS only, content-addressed)
if command -v xcrun &>/dev/null; then
  SDK_VERSION=$(xcrun --show-sdk-version 2>/dev/null || echo "unknown")
  SDK_PATH=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)
  log_info "  SDK ${SDK_VERSION} at ${SDK_PATH}"

  # Content-address SDK: hash a known stable header
  if [[ -f "${SDK_PATH}/usr/include/stdlib.h" ]]; then
    SDK_STDLIB_HASH=$(shasum -a 256 "${SDK_PATH}/usr/include/stdlib.h" | awk '{print $1}')
    log_info "  SDK stdlib.h: ${SDK_STDLIB_HASH:0:16}... (content-addressed)"
  else
    log_warn "SDK stdlib.h not found - cannot content-address SDK"
  fi
else
  log_info "  SDK validation skipped (Linux)"
fi

log_success "Tool versions validated"

# ─────────────────────────────────────────────────────────────────────────────
# Environment Reset: Hard Quarantine
# ─────────────────────────────────────────────────────────────────────────────

log_info "Sealing environment..."

# PATH: Only system binaries (build tools pinned explicitly above)
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"
log_info "PATH reset to: ${PATH}"
log_info "  (All build tools explicitly pinned, not discovered via PATH)"

# Unset all compiler/linker environment variables
# These can leak into the build and cause non-determinism
unset CPATH
unset C_INCLUDE_PATH
unset CPLUS_INCLUDE_PATH
unset LIBRARY_PATH
unset LD_LIBRARY_PATH
unset DYLD_LIBRARY_PATH
unset DYLD_FALLBACK_LIBRARY_PATH
unset DYLD_FRAMEWORK_PATH
unset DYLD_FALLBACK_FRAMEWORK_PATH

# Unset SDK/compiler overrides
unset SDKROOT
unset MACOSX_DEPLOYMENT_TARGET
# CC and CXX are explicitly set above and must be preserved for CMake
unset CFLAGS
unset CXXFLAGS
unset LDFLAGS

# Unset CMake environment variables (force CMakeLists.txt + toolchain only)
unset CMAKE_PREFIX_PATH
unset CMAKE_MODULE_PATH
unset CMAKE_INCLUDE_PATH
unset CMAKE_LIBRARY_PATH
unset CMAKE_FRAMEWORK_PATH

# Unset package manager variables
unset HOMEBREW_PREFIX
unset HOMEBREW_CELLAR
unset PKG_CONFIG_PATH

log_success "Environment sealed (all ambient variables cleared)"

# ─────────────────────────────────────────────────────────────────────────────
# Compiler Flags: Explicit Lock (Prevent Drift)
# ─────────────────────────────────────────────────────────────────────────────
# Even with pinned compiler, defaults can drift between versions
# Lock flags explicitly to prevent:
#   - Embedded compiler invocation strings
#   - Flag-order drift
#   - Non-deterministic codegen

# Optimization: -O2 (balanced, deterministic)
# Security: -fstack-protector-strong, -D_FORTIFY_SOURCE=2
# Determinism: -fno-record-gcc-switches (don't embed compiler command)
export CFLAGS="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-record-gcc-switches"
export CXXFLAGS="${CFLAGS}"

# LDFLAGS: Platform-specific (Linux vs macOS)
if [[ "$(uname)" == "Linux" ]]; then
  export LDFLAGS="-Wl,-z,relro,-z,now"  # Full RELRO (GOT protection)
  log_info "Compiler flags locked:"
  log_info "  CFLAGS: ${CFLAGS}"
  log_info "  LDFLAGS: ${LDFLAGS} (Linux hardening)"
else
  # macOS: Don't override LDFLAGS - let toolchain use platform defaults
  # (macOS already has ASLR, code signing, and other hardening by default)
  log_info "Compiler flags locked:"
  log_info "  CFLAGS: ${CFLAGS}"
  log_info "  LDFLAGS: (platform defaults - macOS has built-in hardening)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Locale/Timezone Determinism
# ─────────────────────────────────────────────────────────────────────────────

export LANG=C
export LC_ALL=C
export TZ=UTC

log_info "Locale: ${LANG}"
log_info "Timezone: ${TZ}"

# ─────────────────────────────────────────────────────────────────────────────
# Reproducible Build Timestamp (MANDATORY)
# ─────────────────────────────────────────────────────────────────────────────
# SOURCE_DATE_EPOCH: Unix timestamp used for reproducible builds
# Set to git commit timestamp for deterministic builds
# This is REQUIRED for hermetic builds (not optional)

cd "${PROJECT_ROOT}"
if git rev-parse HEAD >/dev/null 2>&1; then
  # Use git commit timestamp
  export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
  log_info "SOURCE_DATE_EPOCH: ${SOURCE_DATE_EPOCH} (git commit timestamp)"
  log_info "  Commit: $(git rev-parse --short HEAD)"
  log_info "  Date: $(date -u -r ${SOURCE_DATE_EPOCH} '+%Y-%m-%d %H:%M:%S UTC')"
else
  # Not a git repo - fail hard (hermetic builds require git)
  log_error "Not a git repository. Hermetic builds require git for SOURCE_DATE_EPOCH."
  exit 1
fi

# Hard gate: SOURCE_DATE_EPOCH must be set
: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH must be set for hermetic builds}"

# ─────────────────────────────────────────────────────────────────────────────
# Deterministic Archives (macOS/Linux)
# ─────────────────────────────────────────────────────────────────────────────
# Static archives (.a) are a classic source of non-determinism
# Force stable member ordering and timestamps

export ARFLAGS=rcsD
log_info "Archive flags: ARFLAGS=${ARFLAGS} (deterministic)"

# ─────────────────────────────────────────────────────────────────────────────
# Detect Platform
# ─────────────────────────────────────────────────────────────────────────────

PLATFORM="$(uname -s)"
case "${PLATFORM}" in
  Darwin)
    TOOLCHAIN_FILE="toolchains/macos-hermetic.cmake"
    ;;
  Linux)
    TOOLCHAIN_FILE="toolchains/linux-hermetic.cmake"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    TOOLCHAIN_FILE="toolchains/windows-msvc-hermetic.cmake"
    ;;
  *)
    log_error "Unsupported platform: ${PLATFORM}"
    exit 1
    ;;
esac

log_info "Platform: ${PLATFORM}"
log_info "Toolchain: ${TOOLCHAIN_FILE}"

# ─────────────────────────────────────────────────────────────────────────────
# Verify Toolchain File Exists
# ─────────────────────────────────────────────────────────────────────────────

if [[ ! -f "${PROJECT_ROOT}/${TOOLCHAIN_FILE}" ]]; then
  log_error "Toolchain file not found: ${TOOLCHAIN_FILE}"
  exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# Log Build Inputs (Auditability)
# ─────────────────────────────────────────────────────────────────────────────

log_info "Recording build inputs..."

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "Build Input Audit Log"
echo "═══════════════════════════════════════════════════════════"
echo "Timestamp:     $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "Platform:      ${PLATFORM}"
echo "Arch:          $(uname -m)"
echo "Toolchain:     ${TOOLCHAIN_FILE}"
echo ""
echo "Compiler:"
"$CC_BIN" --version | head -1
echo ""
echo "CMake:"
"$CMAKE_BIN" --version | head -1
echo ""
echo "Git Commit:"
git rev-parse HEAD 2>/dev/null || echo "Not a git repository"
echo ""
echo "Environment:"
echo "  PATH=${PATH}"
echo "  LANG=${LANG}"
echo "  TZ=${TZ}"
echo "═══════════════════════════════════════════════════════════"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# CMake Configure
# ─────────────────────────────────────────────────────────────────────────────

log_info "Configuring CMake (hermetic mode)..."

cd "${PROJECT_ROOT}"

"$CMAKE_BIN" -S . -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release

log_success "CMake configuration complete"

# ─────────────────────────────────────────────────────────────────────────────
# Build
# ─────────────────────────────────────────────────────────────────────────────

log_info "Building target: ${TARGET}"

# Parallel jobs (use all CPU cores)
if [[ "${PLATFORM}" == "Darwin" ]]; then
  JOBS=$(sysctl -n hw.ncpu)
elif [[ "${PLATFORM}" == "Linux" ]]; then
  JOBS=$(nproc)
else
  JOBS=4
fi

# Unset ARFLAGS to prevent conflict with Rust cc-rs crate
# (cc-rs adds its own ar flags, causing "ar rcsD cq" - two operation modes)
unset ARFLAGS
"$CMAKE_BIN" --build "${BUILD_DIR}" --target "${TARGET}" -j"${JOBS}"

log_success "Build complete"

# ─────────────────────────────────────────────────────────────────────────────
# Timestamp Leakage Detection (Automatic Non-Determinism Check)
# ─────────────────────────────────────────────────────────────────────────────

log_info "Scanning for timestamp leakage..."

TIMESTAMP_LEAK_FOUND=false

# Scan all built binaries for embedded timestamp macros
for BINARY in $(find "${BUILD_DIR}" -type f \( -name "dinerod" -o -name "dinero-cli" -o -name "test_*" -o -name "*.a" \) 2>/dev/null); do
  if [[ -f "$BINARY" ]]; then
    # Check for __DATE__, __TIME__, __TIMESTAMP__ macros in strings
    if strings "$BINARY" | grep -qE "(__DATE__|__TIME__|__TIMESTAMP__|Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) (0?[1-9]|[12][0-9]|3[01]) [0-9]{4}"; then
      log_warn "Potential timestamp leak detected in: $(basename "$BINARY")"
      TIMESTAMP_LEAK_FOUND=true
    fi
  fi
done

if [[ "$TIMESTAMP_LEAK_FOUND" == "true" ]]; then
  log_warn "Non-determinism risk: Timestamp macros detected in binaries"
  log_warn "  Search source code for: __DATE__, __TIME__, __TIMESTAMP__"
  log_warn "  This will cause determinism tests to fail"
  # Don't fail build - just warn (timestamp embedding is common in version info)
else
  log_success "No obvious timestamp leakage detected"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Verify Output
# ─────────────────────────────────────────────────────────────────────────────

echo ""
log_info "Build artifacts:"
find "${BUILD_DIR}" -type f \( -name "dinerod" -o -name "dinero-cli" -o -name "test_*" \) -exec ls -lh {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Build Fingerprint (Attestation Precursor)
# ─────────────────────────────────────────────────────────────────────────────

log_info "Generating build fingerprint..."

FINGERPRINT_FILE="${BUILD_DIR}/build-fingerprint.json"
GIT_COMMIT=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
GIT_COMMIT_SHORT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
COMPILER_VERSION=$(/usr/bin/clang --version 2>/dev/null | head -1 || echo "unknown")
CMAKE_VERSION=$("$CMAKE_BIN" --version 2>/dev/null | head -1 | awk '{print $3}' || echo "unknown")
SDK_VERSION=$(xcrun --show-sdk-version 2>/dev/null || echo "unknown")

# Generate JSON fingerprint
cat > "${FINGERPRINT_FILE}" <<EOF
{
  "build_system": "DineroCoin Hermetic Build",
  "build_timestamp": "$(date -u -r ${SOURCE_DATE_EPOCH} '+%Y-%m-%d %H:%M:%S UTC')",
  "git_commit": "${GIT_COMMIT}",
  "git_commit_short": "${GIT_COMMIT_SHORT}",
  "source_date_epoch": ${SOURCE_DATE_EPOCH},
  "platform": "${PLATFORM}",
  "architecture": "$(uname -m)",
  "os_version": "$(sw_vers -productVersion 2>/dev/null || uname -r)",
  "compiler": "/usr/bin/clang",
  "compiler_version": "${COMPILER_VERSION}",
  "cmake_version": "${CMAKE_VERSION}",
  "sdk_version": "${SDK_VERSION}",
  "toolchain_file": "${TOOLCHAIN_FILE}",
  "locale": "${LANG}",
  "timezone": "${TZ}",
  "build_target": "${TARGET}",
  "artifacts": {
EOF

# Add artifact hashes
FIRST=true
find "${BUILD_DIR}" -type f \( -name "dinerod" -o -name "dinero-cli" -o -name "test_*" -o -name "*.a" \) 2>/dev/null | while read artifact; do
  if [[ -f "$artifact" ]]; then
    ARTIFACT_NAME=$(basename "$artifact")
    ARTIFACT_HASH=$(shasum -a 256 "$artifact" | awk '{print $1}')
    if [[ "$FIRST" == "true" ]]; then
      echo "    \"${ARTIFACT_NAME}\": \"${ARTIFACT_HASH}\"" >> "${FINGERPRINT_FILE}"
      FIRST=false
    else
      echo "    ,\"${ARTIFACT_NAME}\": \"${ARTIFACT_HASH}\"" >> "${FINGERPRINT_FILE}"
    fi
  fi
done

# Close JSON
cat >> "${FINGERPRINT_FILE}" <<EOF
  }
}
EOF

log_success "Build fingerprint saved: ${FINGERPRINT_FILE}"

# Display fingerprint summary
if [[ -f "${FINGERPRINT_FILE}" ]]; then
  log_info "Fingerprint summary:"
  echo "  Commit:      ${GIT_COMMIT_SHORT}"
  echo "  Date:        $(date -u -r ${SOURCE_DATE_EPOCH} '+%Y-%m-%d %H:%M:%S UTC')"
  echo "  Platform:    ${PLATFORM}/$(uname -m)"
  echo "  Compiler:    ${COMPILER_VERSION}"
  echo "  Toolchain:   ${TOOLCHAIN_FILE}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# GPG Signature (Attestation)
# ─────────────────────────────────────────────────────────────────────────────
# Sign the build fingerprint to create a cryptographic attestation.
# This is optional for now (no hard requirement), but recommended for releases.
#
# Philosophy:
#   Fingerprint = "what was built"
#   Signature   = "who attests to it"
#
# Gitian-compatible: multiple builders can sign the same fingerprint independently.
# ─────────────────────────────────────────────────────────────────────────────

# Find GPG (may be outside hermetic PATH since signing happens post-build)
GPG_BIN=""
if [[ -x "/opt/homebrew/bin/gpg" ]]; then
  GPG_BIN="/opt/homebrew/bin/gpg"
elif [[ -x "/usr/local/bin/gpg" ]]; then
  GPG_BIN="/usr/local/bin/gpg"
elif command -v gpg >/dev/null 2>&1; then
  GPG_BIN="$(command -v gpg)"
fi

if [[ -n "${GPG_BIN}" ]]; then
  # Check if user has a GPG key configured (must have actual keys, not just empty keyring)
  KEY_COUNT=$("${GPG_BIN}" --list-secret-keys --with-colons 2>/dev/null | grep -c '^sec:' || true)
  if [[ "${KEY_COUNT}" -gt 0 ]]; then
    log_info "Signing build fingerprint with GPG..."

    # Clearsign: signature embedded in readable JSON
    # (alternative: detached sig with --detach-sign)
    if "${GPG_BIN}" --clearsign --armor --output "${FINGERPRINT_FILE}.asc" "${FINGERPRINT_FILE}" 2>/dev/null; then
      log_success "Build fingerprint signed: ${FINGERPRINT_FILE}.asc"

      # Also create detached signature for programmatic verification
      "${GPG_BIN}" --detach-sign --armor --output "${FINGERPRINT_FILE}.sig" "${FINGERPRINT_FILE}" 2>/dev/null
      log_success "Detached signature: ${FINGERPRINT_FILE}.sig"

      # Show signer identity
      SIGNER=$("${GPG_BIN}" --list-secret-keys --with-colons 2>/dev/null | grep '^uid' | head -1 | cut -d: -f10)
      log_info "Signed by: ${SIGNER}"
    else
      log_warn "GPG signing failed (no default key or passphrase issue)"
      log_info "Fingerprint available unsigned: ${FINGERPRINT_FILE}"
    fi
  else
    log_info "No GPG secret key found - fingerprint not signed"
    log_info "To enable signing: ${GPG_BIN} --gen-key"
  fi
else
  log_info "GPG not available - fingerprint not signed"
  log_info "To enable signing: brew install gnupg"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Final Status
# ─────────────────────────────────────────────────────────────────────────────

echo ""
log_success "Hermetic build successful"
log_info "Build directory: ${BUILD_DIR}"
log_info "To run tests: cd ${BUILD_DIR} && ctest"
echo ""
