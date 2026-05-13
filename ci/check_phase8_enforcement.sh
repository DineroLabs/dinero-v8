#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.6: Master Enforcement Script
# ═══════════════════════════════════════════════════════════════════════════
# Runs all four enforcement layers for L1/L2 separation
#
# Enforcement layers:
#   1. Header Boundary Guard    (check_lightning_includes.sh)
#   2. Symbol Boundary Guard    (check_lightning_symbols.sh)
#   3. Runtime Determinism Guard (check_lightning_runtime.sh)
#   4. Hermetic Build Verification (check_hermetic_build.sh)
#
# Exit code:
# - 0: All checks passed (CI SUCCESS)
# - 1: At least one check failed (CI FAIL)
#
# Usage:
#   ./ci/check_phase8_enforcement.sh          # Run all checks
#   ./ci/check_phase8_enforcement.sh --fast   # Skip hermetic build (faster, less thorough)
# ═══════════════════════════════════════════════════════════════════════════

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_ROOT"

echo "════════════════════════════════════════════════════════════════════════════"
echo "PHASE 8.6: L1/L2 SEPARATION ENFORCEMENT"
echo "════════════════════════════════════════════════════════════════════════════"
echo ""
echo "Project: $PROJECT_ROOT"
echo "Enforcement: All architectural invariants from Phases 8.1-8.5"
echo ""
echo "Layers:"
echo "  [1] Header Boundary Guard     - Lightning cannot include L1 headers"
echo "  [2] Symbol Boundary Guard     - Lightning cannot link L1 symbols"
echo "  [3] Runtime Determinism Guard - Lightning has no threads/clocks"
echo "  [4] Hermetic Build Verification - Lightning builds without dinerod"
echo ""
echo "════════════════════════════════════════════════════════════════════════════"
echo ""

FAILED_CHECKS=()
FAST_MODE=false

# Parse arguments
if [ "$1" == "--fast" ]; then
  FAST_MODE=true
  echo "⚡ Fast mode: Skipping hermetic build verification"
  echo ""
fi

# ═════════════════════════════════════════════════════════════════════════════
# Layer 1: Header Boundary Guard
# ═════════════════════════════════════════════════════════════════════════════

echo ""
if "$SCRIPT_DIR/check_lightning_includes.sh"; then
  echo "✅ Layer 1: PASSED"
else
  echo "❌ Layer 1: FAILED"
  FAILED_CHECKS+=("Header Boundary Guard")
fi

# ═════════════════════════════════════════════════════════════════════════════
# Layer 2: Symbol Boundary Guard
# ═════════════════════════════════════════════════════════════════════════════

echo ""
if "$SCRIPT_DIR/check_lightning_symbols.sh"; then
  echo "✅ Layer 2: PASSED"
else
  echo "❌ Layer 2: FAILED"
  FAILED_CHECKS+=("Symbol Boundary Guard")
fi

# ═════════════════════════════════════════════════════════════════════════════
# Layer 3: Runtime Determinism Guard
# ═════════════════════════════════════════════════════════════════════════════

echo ""
if "$SCRIPT_DIR/check_lightning_runtime.sh"; then
  echo "✅ Layer 3: PASSED"
else
  echo "❌ Layer 3: FAILED"
  FAILED_CHECKS+=("Runtime Determinism Guard")
fi

# ═════════════════════════════════════════════════════════════════════════════
# Layer 4: Hermetic Build Verification
# ═════════════════════════════════════════════════════════════════════════════

if [ "$FAST_MODE" = false ]; then
  echo ""
  if "$SCRIPT_DIR/check_hermetic_build.sh"; then
    echo "✅ Layer 4: PASSED"
  else
    echo "❌ Layer 4: FAILED"
    FAILED_CHECKS+=("Hermetic Build Verification")
  fi
else
  echo ""
  echo "⏭️  Layer 4: SKIPPED (--fast mode)"
fi

# ═════════════════════════════════════════════════════════════════════════════
# Final Report
# ═════════════════════════════════════════════════════════════════════════════

echo ""
echo "════════════════════════════════════════════════════════════════════════════"

if [ ${#FAILED_CHECKS[@]} -eq 0 ]; then
  echo "✅ PHASE 8.6: ALL ENFORCEMENT LAYERS PASSED"
  echo ""
  echo "Architectural guarantees VERIFIED:"
  echo "  ✅ Lightning cannot include L1 headers"
  echo "  ✅ Lightning cannot link L1 symbols"
  echo "  ✅ Lightning has no threads or clocks (deterministic)"
  if [ "$FAST_MODE" = false ]; then
    echo "  ✅ Lightning builds hermetically (no dinerod)"
  fi
  echo ""
  echo "L1/L2 separation: MECHANICALLY ENFORCED ✅"
  echo "Phase 8: COMPLETE ✅"
  echo "════════════════════════════════════════════════════════════════════════════"
  exit 0
else
  echo "❌ PHASE 8.6: ${#FAILED_CHECKS[@]} ENFORCEMENT LAYER(S) FAILED"
  echo ""
  echo "Failed checks:"
  for check in "${FAILED_CHECKS[@]}"; do
    echo "  ❌ $check"
  done
  echo ""
  echo "L1/L2 separation: VIOLATED ❌"
  echo "CI: MUST BLOCK MERGE ❌"
  echo "════════════════════════════════════════════════════════════════════════════"
  exit 1
fi
