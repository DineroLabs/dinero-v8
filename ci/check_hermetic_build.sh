#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Phase 8.6: Enforcement Layer 4 - Hermetic Build Verification
# ═══════════════════════════════════════════════════════════════════════════
# Ensures lightningd can build WITHOUT dinerod (Phase 8.4 guarantee)
#
# Build configuration:
#   cmake -DBUILD_DINEROD=OFF -DBUILD_LIGHTNINGD=ON ..
#   make lightningd
#
# Forbidden outcomes during build:
# - NO protobuf generation (dinerod.pb.cc)
# - NO gRPC compilation (grpc)
# - NO daemon objects built
# - NO wallet objects built
# - NO chainstate objects built
#
# Exit code:
# - 0: Hermetic build succeeded
# - 1: Build contamination detected (CI FAIL)
# - 2: Build failed (CI FAIL)
# ═══════════════════════════════════════════════════════════════════════════

set -e

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 8.6 - Enforcement Layer 4: Hermetic Build Verification"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

VIOLATIONS=0
BUILD_LOG="ci_hermetic_build.log"

echo "[1/5] Cleaning previous build artifacts..."
rm -rf build_hermetic
mkdir -p build_hermetic

echo ""
echo "[2/5] Configuring hermetic Lightning build..."
echo "  cmake -S . -B build_hermetic -DBUILD_DINEROD=OFF -DBUILD_LIGHTNINGD=ON -DCMAKE_BUILD_TYPE=Debug"
echo ""

if ! cmake -S . -B build_hermetic -DBUILD_DINEROD=OFF -DBUILD_LIGHTNINGD=ON -DCMAKE_BUILD_TYPE=Debug > "build_hermetic/$BUILD_LOG" 2>&1; then
  echo "❌ FAIL: CMake configuration failed"
  echo ""
  echo "Last 20 lines of build log:"
  tail -20 "build_hermetic/$BUILD_LOG"
  exit 2
fi

# Check configuration output for violations
echo "[3/5] Verifying configuration output..."
echo ""

CONFIG_VIOLATIONS=(
  "Generating protobuf"
  "Generating gRPC"
  "dinerod.pb.cc"
  "dinerod.grpc.pb.cc"
  "dinerod_proto"
)

for pattern in "${CONFIG_VIOLATIONS[@]}"; do
  if grep -q "$pattern" "build_hermetic/$BUILD_LOG"; then
    echo "  ❌ VIOLATION: L1 artifact detected in configuration: $pattern"
    grep "$pattern" "build_hermetic/$BUILD_LOG"
    VIOLATIONS=$((VIOLATIONS + 1))
  else
    echo "  ✅ $pattern (not generated)"
  fi
done

# Verify correct configuration message
if grep -q "lightningd binary enabled (Phase 8.4: Hermetic Build)" "build_hermetic/$BUILD_LOG"; then
  echo "  ✅ Phase 8.4 hermetic build message confirmed"
else
  echo "  ⚠️  WARNING: Expected Phase 8.4 hermetic build message not found"
fi

echo ""
echo "[4/5] Building lightningd..."
echo "  make -C build_hermetic lightningd"
echo ""

if ! make -C build_hermetic lightningd >> "build_hermetic/$BUILD_LOG" 2>&1; then
  echo "❌ FAIL: lightningd build failed"
  echo ""
  echo "Last 30 lines of build log:"
  tail -30 "build_hermetic/$BUILD_LOG"
  exit 2
fi

# Check build output for contamination
BUILD_VIOLATIONS=(
  "dinerod.pb.cc"
  "daemon_context"
  "chainstate"
  "wallet_manager"
  "mempool_service"
)

for pattern in "${BUILD_VIOLATIONS[@]}"; do
  if grep -q "$pattern" "build_hermetic/$BUILD_LOG"; then
    echo "  ❌ VIOLATION: L1 object detected in build: $pattern"
    VIOLATIONS=$((VIOLATIONS + 1))
  fi
done

echo "[5/5] Verifying lightningd binary..."
echo ""

if [ ! -f "build_hermetic/lightningd" ]; then
  echo "❌ FAIL: lightningd binary not produced"
  exit 2
fi

BINARY_SIZE=$(ls -lh build_hermetic/lightningd | awk '{print $5}')
echo "  ✅ lightningd binary created: $BINARY_SIZE"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ $VIOLATIONS -eq 0 ]; then
  echo "✅ PASS: Hermetic Lightning build verified"
  echo "   Configuration: BUILD_DINEROD=OFF, BUILD_LIGHTNINGD=ON"
  echo "   Result: lightningd built WITHOUT L1 dependencies"
  echo "   Binary: build_hermetic/lightningd ($BINARY_SIZE)"
  echo "   Phase 8.4 guarantee: ENFORCED"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 0
else
  echo "❌ FAIL: $VIOLATIONS violation(s) detected"
  echo "   Lightning build is NOT hermetic"
  echo "   L1 artifacts leaked into build process"
  echo "   Phase 8.6 Enforcement Layer 4: FAILED"
  echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
  exit 1
fi
