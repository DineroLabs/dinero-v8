#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# DineroCoin Determinism Test Script
# ═══════════════════════════════════════════════════════════════════════════
# Purpose: Verify that hermetic builds produce byte-identical artifacts
# Usage:   ./scripts/test-determinism.sh [target]
#
# Process:
#   1. Build A: Clean build with hermetic environment
#   2. Build B: Second clean build with same inputs
#   3. Hash comparison: Verify byte-for-byte identity
#   4. Report: Log differences if found
#
# This is the single-machine version of multi-builder reproducibility testing.
# Even on one Mac, this catches:
#   - Timestamp embedding
#   - Non-deterministic codegen
#   - Build order dependencies
#   - Environmental leaks
#
# Design philosophy: "Identical inputs must produce identical outputs"
# ═══════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# Color Output
# ─────────────────────────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
NC='\033[0m'

log_info() {
  echo -e "${BLUE}🔬 [DETERMINISM]${NC} $*"
}

log_success() {
  echo -e "${GREEN}✅ [DETERMINISM]${NC} $*"
}

log_warn() {
  echo -e "${YELLOW}⚠️  [DETERMINISM]${NC} $*"
}

log_error() {
  echo -e "${RED}❌ [DETERMINISM]${NC} $*" >&2
}

log_diff() {
  echo -e "${MAGENTA}🔍 [DETERMINISM]${NC} $*"
}

# ─────────────────────────────────────────────────────────────────────────────
# Configuration
# ─────────────────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

TARGET="${1:-test_consensus_ring2_validity}"
TEMP_DIR="/tmp/dinero_determinism_test"
BUILD_A="${TEMP_DIR}/build_a"
BUILD_B="${TEMP_DIR}/build_b"
ARTIFACTS_A="${TEMP_DIR}/artifacts_a"
ARTIFACTS_B="${TEMP_DIR}/artifacts_b"

log_info "Testing determinism of target: ${TARGET}"
log_info "Working directory: ${TEMP_DIR}"

# ─────────────────────────────────────────────────────────────────────────────
# Clean Previous Test Runs
# ─────────────────────────────────────────────────────────────────────────────

log_info "Cleaning previous test artifacts..."
rm -rf "${TEMP_DIR}"
mkdir -p "${ARTIFACTS_A}" "${ARTIFACTS_B}"

# ─────────────────────────────────────────────────────────────────────────────
# Build A: First Hermetic Build
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "Build A: First hermetic build"
echo "═══════════════════════════════════════════════════════════"

log_info "Running hermetic build A..."

cd "${PROJECT_ROOT}"
"${SCRIPT_DIR}/hermetic-build.sh" "${TARGET}" 2>&1 | tee "${TEMP_DIR}/build_a.log"

log_info "Copying artifacts from build A..."
cp -a "${PROJECT_ROOT}/build_hermetic/${TARGET}" "${ARTIFACTS_A}/" 2>/dev/null || \
  log_warn "Target '${TARGET}' not found, testing full build instead"

# If specific target not found, test all binaries
if [[ ! -f "${ARTIFACTS_A}/${TARGET}" ]]; then
  find "${PROJECT_ROOT}/build_hermetic" -type f \( -name "*.a" -o -name "*.dylib" -o -name "*.so" -o -executable \) \
    -exec cp {} "${ARTIFACTS_A}/" \; 2>/dev/null || true
fi

log_success "Build A complete"

# ─────────────────────────────────────────────────────────────────────────────
# Clean Between Builds
# ─────────────────────────────────────────────────────────────────────────────

log_info "Cleaning build directory..."
rm -rf "${PROJECT_ROOT}/build_hermetic"

# Small delay to ensure timestamp differences would show up (if present)
sleep 2

# ─────────────────────────────────────────────────────────────────────────────
# Build B: Second Hermetic Build
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "Build B: Second hermetic build (should be identical)"
echo "═══════════════════════════════════════════════════════════"

log_info "Running hermetic build B..."

cd "${PROJECT_ROOT}"
"${SCRIPT_DIR}/hermetic-build.sh" "${TARGET}" 2>&1 | tee "${TEMP_DIR}/build_b.log"

log_info "Copying artifacts from build B..."
cp -a "${PROJECT_ROOT}/build_hermetic/${TARGET}" "${ARTIFACTS_B}/" 2>/dev/null || true

if [[ ! -f "${ARTIFACTS_B}/${TARGET}" ]]; then
  find "${PROJECT_ROOT}/build_hermetic" -type f \( -name "*.a" -o -name "*.dylib" -o -name "*.so" -o -executable \) \
    -exec cp {} "${ARTIFACTS_B}/" \; 2>/dev/null || true
fi

log_success "Build B complete"

# ─────────────────────────────────────────────────────────────────────────────
# Hash Comparison
# ─────────────────────────────────────────────────────────────────────────────

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "Determinism Analysis"
echo "═══════════════════════════════════════════════════════════"

log_info "Computing artifact hashes..."

# Generate hashes for all artifacts
(cd "${ARTIFACTS_A}" && find . -type f -exec shasum -a 256 {} \; | sort > "${TEMP_DIR}/hashes_a.txt")
(cd "${ARTIFACTS_B}" && find . -type f -exec shasum -a 256 {} \; | sort > "${TEMP_DIR}/hashes_b.txt")

# ─────────────────────────────────────────────────────────────────────────────
# Results
# ─────────────────────────────────────────────────────────────────────────────

echo ""
log_info "Comparing hashes..."
echo ""

if diff -u "${TEMP_DIR}/hashes_a.txt" "${TEMP_DIR}/hashes_b.txt" > "${TEMP_DIR}/hash_diff.txt"; then
  # ═══════════════════════════════════════════════════════════
  # SUCCESS: Deterministic Build Verified
  # ═══════════════════════════════════════════════════════════

  echo "┌──────────────────────────────────────────────────────┐"
  echo "│                                                      │"
  log_success "  ✅ DETERMINISTIC BUILD VERIFIED                   │"
  echo "│                                                      │"
  echo "│  Build A and Build B are byte-for-byte identical    │"
  echo "│  SHA-256 hashes match perfectly                     │"
  echo "│                                                      │"
  echo "└──────────────────────────────────────────────────────┘"
  echo ""

  log_info "Verified artifacts:"
  cat "${TEMP_DIR}/hashes_a.txt" | while read hash file; do
    echo "  ${hash:0:16}... ${file}"
  done

  # Generate hash manifest (machine-readable)
  MANIFEST_FILE="${PROJECT_ROOT}/build_hermetic/determinism-manifest.txt"
  cp "${TEMP_DIR}/hashes_a.txt" "${MANIFEST_FILE}"
  log_success "Hash manifest saved: ${MANIFEST_FILE}"

  echo ""
  log_success "Hermetic build is FULLY DETERMINISTIC on this machine"
  log_info "Commit measured: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
  echo ""
  exit 0
else
  # ═══════════════════════════════════════════════════════════
  # FAILURE: Non-Determinism Detected (Hard Failure)
  # ═══════════════════════════════════════════════════════════

  echo "┌──────────────────────────────────────────────────────┐"
  echo "│                                                      │"
  log_error "  ❌ NON-DETERMINISM DETECTED                        │"
  echo "│                                                      │"
  echo "│  Build A and Build B produced different outputs     │"
  echo "│  THIS IS A CONTRACT VIOLATION                       │"
  echo "│                                                      │"
  echo "└──────────────────────────────────────────────────────┘"
  echo ""

  # Store detailed evidence in build directory
  EVIDENCE_DIR="${PROJECT_ROOT}/build_hermetic/determinism-failure"
  mkdir -p "${EVIDENCE_DIR}"

  cp "${TEMP_DIR}/hashes_a.txt" "${EVIDENCE_DIR}/"
  cp "${TEMP_DIR}/hashes_b.txt" "${EVIDENCE_DIR}/"
  cp "${TEMP_DIR}/hash_diff.txt" "${EVIDENCE_DIR}/"
  cp "${TEMP_DIR}/build_a.log" "${EVIDENCE_DIR}/" 2>/dev/null || true
  cp "${TEMP_DIR}/build_b.log" "${EVIDENCE_DIR}/" 2>/dev/null || true

  # Copy differing binaries for analysis
  while read hash file; do
    HASH_A=$(grep "${file}" "${TEMP_DIR}/hashes_a.txt" | awk '{print $1}')
    HASH_B=$(grep "${file}" "${TEMP_DIR}/hashes_b.txt" | awk '{print $1}')
    if [[ "${HASH_A}" != "${HASH_B}" ]]; then
      BASENAME=$(basename "${file}")
      cp "${ARTIFACTS_A}/${BASENAME}" "${EVIDENCE_DIR}/${BASENAME}.build_a" 2>/dev/null || true
      cp "${ARTIFACTS_B}/${BASENAME}" "${EVIDENCE_DIR}/${BASENAME}.build_b" 2>/dev/null || true
    fi
  done < "${TEMP_DIR}/hashes_a.txt"

  log_diff "Differences found:"
  cat "${TEMP_DIR}/hash_diff.txt"
  echo ""

  log_diff "Possible causes:"
  echo "  • Embedded timestamps (check build_version.cc, __DATE__, __TIME__)"
  echo "  • Non-deterministic codegen (rare with modern compilers)"
  echo "  • Build order dependencies (parallel build race conditions)"
  echo "  • Environment leakage (check hermetic-build.sh is being used)"
  echo "  • Archive member ordering (check ARFLAGS=rcsD is set)"
  echo ""

  log_error "Evidence archived to: ${EVIDENCE_DIR}/"
  echo "  Hash A:      ${EVIDENCE_DIR}/hashes_a.txt"
  echo "  Hash B:      ${EVIDENCE_DIR}/hashes_b.txt"
  echo "  Diff:        ${EVIDENCE_DIR}/hash_diff.txt"
  echo "  Build logs:  ${EVIDENCE_DIR}/build_{a,b}.log"
  echo "  Binaries:    ${EVIDENCE_DIR}/*.build_{a,b}"
  echo ""

  log_error "HARD FAILURE: Non-determinism is a contract violation"
  log_warn "Fix required before release builds"
  echo ""
  exit 1
fi
