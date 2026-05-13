#!/bin/bash

set -e

# ==============================================================================
# DineroCoin - iOS Wallet FFI Build Script
# Builds libdinero_wallet_ffi.a for iOS (arm64) without RocksDB, daemon, or miner
# ==============================================================================

# Configuration
PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-ios-simple"
IOS_DEPLOY_TARGET="13.0"
IOS_ARCH="arm64"
IOS_TARGET_COPY="/Users/haydarevich/Documents/DineroiOS/Dinero/Dinero/FFI/libdinero_wallet_ffi.a"

echo "📦 Building Dinero Wallet FFI for iOS"
echo "Root: ${PROJECT_ROOT}"
echo "Build dir: ${BUILD_DIR}"
echo "Deployment target: iOS ${IOS_DEPLOY_TARGET}"
echo "Architecture: ${IOS_ARCH}"
echo ""

# ------------------------------------------------------------------------------
# 1. Prepare clean build directory
# ------------------------------------------------------------------------------
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# ------------------------------------------------------------------------------
# 2. Configure CMake for iOS (FFI only)
# ------------------------------------------------------------------------------
# Note: CMakeLists.txt is at project root, not in wallet-core/ffi
cmake .. \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=${IOS_ARCH} \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=${IOS_DEPLOY_TARGET} \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_FFI_ONLY=ON \
  -DENABLE_SANITIZERS=OFF \
  -G Xcode

# ------------------------------------------------------------------------------
# 3. Build static library
# ------------------------------------------------------------------------------
cmake --build . --target dinero_wallet_ffi --config Release

# ------------------------------------------------------------------------------
# 4. Find the built library (Xcode puts it in Release-iphoneos/)
# ------------------------------------------------------------------------------
FFI_OUTPUT_PATH=$(find "${BUILD_DIR}" -name "libdinero_wallet_ffi.a" -type f | head -1)

if [ -z "${FFI_OUTPUT_PATH}" ]; then
  echo "❌ Build failed: libdinero_wallet_ffi.a not found!"
  echo "   Searched in: ${BUILD_DIR}"
  exit 1
fi

SIZE=$(stat -f%z "${FFI_OUTPUT_PATH}" 2>/dev/null || echo "0")

if [ "$SIZE" -lt 50000 ]; then
  echo "⚠️  Warning: Library size is small (${SIZE} bytes) — check build output."
else
  echo "✅ Library built successfully: ${FFI_OUTPUT_PATH} (${SIZE} bytes)"
fi

# ------------------------------------------------------------------------------
# 5. Find dependencies (jsoncpp, dinero_wallet, dinero_crypto)
# ------------------------------------------------------------------------------
JSONCPP_LIB=$(find "${BUILD_DIR}" -name "libjsoncpp*.a" -type f | head -1)
WALLET_LIB=$(find "${BUILD_DIR}" -name "libdinero_wallet.a" -type f | head -1)
CRYPTO_LIB=$(find "${BUILD_DIR}" -name "libdinero_crypto.a" -type f | head -1)

echo ""
echo "📚 Dependencies found:"
if [ -n "${JSONCPP_LIB}" ]; then
  echo "   ✅ jsoncpp: ${JSONCPP_LIB}"
else
  echo "   ⚠️  jsoncpp: Not found"
fi

if [ -n "${WALLET_LIB}" ]; then
  echo "   ✅ dinero_wallet: ${WALLET_LIB}"
else
  echo "   ⚠️  dinero_wallet: Not found"
fi

if [ -n "${CRYPTO_LIB}" ]; then
  echo "   ✅ dinero_crypto: ${CRYPTO_LIB}"
else
  echo "   ⚠️  dinero_crypto: Not found"
fi

# ------------------------------------------------------------------------------
# 6. Create fat library with dependencies (or copy dependencies separately)
# ------------------------------------------------------------------------------
# For iOS, we'll copy dependencies to FFI directory for Xcode linking
FFI_DIR="$(dirname "${IOS_TARGET_COPY}")"
mkdir -p "${FFI_DIR}"

# Copy main library
cp "${FFI_OUTPUT_PATH}" "${IOS_TARGET_COPY}"

# Copy dependencies to FFI directory
if [ -n "${JSONCPP_LIB}" ]; then
  cp "${JSONCPP_LIB}" "${FFI_DIR}/libjsoncpp.a"
  echo "   ✅ Copied jsoncpp to FFI directory"
fi

if [ -n "${WALLET_LIB}" ]; then
  cp "${WALLET_LIB}" "${FFI_DIR}/libdinero_wallet.a"
  echo "   ✅ Copied dinero_wallet to FFI directory"
fi

if [ -n "${CRYPTO_LIB}" ]; then
  cp "${CRYPTO_LIB}" "${FFI_DIR}/libdinero_crypto.a"
  echo "   ✅ Copied dinero_crypto to FFI directory"
fi

# ------------------------------------------------------------------------------
# 7. Verify exported symbols
# ------------------------------------------------------------------------------
echo ""
echo "🔍 Exported FFI symbols:"
nm -gU "${FFI_OUTPUT_PATH}" 2>/dev/null | grep "dinero_wallet" | head -10 || echo "⚠️  No symbols found (check nm output)."

# ------------------------------------------------------------------------------
# 8. Summary
# ------------------------------------------------------------------------------
echo ""
echo "📁 Copied to iOS project:"
ls -lh "${IOS_TARGET_COPY}"

echo ""
echo "🎯 Dinero Wallet FFI build complete!"
echo "   • Output: ${FFI_OUTPUT_PATH}"
echo "   • Copied: ${IOS_TARGET_COPY}"
echo "   • Dependencies: ${FFI_DIR}/libjsoncpp.a, ${FFI_DIR}/libdinero_wallet.a, ${FFI_DIR}/libdinero_crypto.a"
echo ""
echo "📋 Xcode Linking Instructions:"
echo "   1. Link Binary With Libraries:"
echo "      • libdinero_wallet_ffi.a"
echo "      • libjsoncpp.a"
echo "      • libdinero_wallet.a"
echo "      • libdinero_crypto.a"
echo "      • libc++.tbd (C++ standard library)"
echo "      • libz.tbd (compression)"
echo "      • libsqlite3.tbd (SQLite)"
echo ""
echo "   2. Other Linker Flags:"
echo "      • -force_load \$(SRCROOT)/Dinero/Dinero/FFI/libdinero_wallet_ffi.a"
echo "      • -force_load \$(SRCROOT)/Dinero/Dinero/FFI/libjsoncpp.a"
echo ""
echo "Next: Open Xcode → select scheme 'Dinero' → Build & Run on iPhone!"
