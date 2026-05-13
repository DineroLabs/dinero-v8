#!/bin/bash
# build_ios_ffi_direct.sh
# Build iOS FFI library directly using clang (bypasses CMake issues)
# This builds only the FFI library and its minimal dependencies

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${DINERO_PROJECT_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
BUILD_DIR="${DINERO_IOS_BUILD_DIR:-${PROJECT_DIR}/build-ios-direct}"
IOS_SDK=$(xcrun --show-sdk-path --sdk iphoneos)
IOS_DEPLOYMENT_TARGET="13.0"

echo "🔨 Building iOS FFI Library (Direct Method)"
echo "============================================"
echo ""

cd "${PROJECT_DIR}"

if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
    echo "❌ Error: PROJECT_DIR does not look like repo root: ${PROJECT_DIR}"
    echo "   Set DINERO_PROJECT_DIR to your Dinero repo root."
    exit 1
fi
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}/lib"
mkdir -p "${BUILD_DIR}/obj"

echo "📋 iOS SDK: ${IOS_SDK}"
echo "📋 Deployment Target: iOS ${IOS_DEPLOYMENT_TARGET}"
echo ""

# Compiler flags
CFLAGS="-arch arm64 -isysroot ${IOS_SDK} -miphoneos-version-min=${IOS_DEPLOYMENT_TARGET} -O2 -fPIC"
CXXFLAGS="${CFLAGS} -std=c++17"

# Include directories
INCLUDES="-I${PROJECT_DIR}/include"
INCLUDES="${INCLUDES} -I${PROJECT_DIR}/src"
INCLUDES="${INCLUDES} -I${PROJECT_DIR}/wallet-core/ffi"
INCLUDES="${INCLUDES} -I${PROJECT_DIR}/third_party/jsoncpp/include"
INCLUDES="${INCLUDES} -I${PROJECT_DIR}/third_party/secp256k1/src"
INCLUDES="${INCLUDES} -I${PROJECT_DIR}/third_party/secp256k1/include"

echo "⚠️  Note: This is a simplified build approach"
echo "   Building FFI library requires many dependencies"
echo "   (OpenSSL, secp256k1, jsoncpp, wallet core, etc.)"
echo ""
echo "📋 Recommended: Build in Xcode after fixing CMake"
echo ""
echo "📚 Alternative: Use existing macOS build and convert"
echo "   (Not recommended - architecture mismatch)"

echo ""
echo "🔧 Best approach: Fix CMake iOS configuration"
echo "   Issue: Line 626 - target_link_libraries error"
echo ""
echo "📋 Checking CMake error details..."

cd "${PROJECT_DIR}"
cmake -S . -B build-ios-check \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DCURL_FOUND=OFF \
  2>&1 | grep -A 10 "CMake Error" | head -20

echo ""
echo "💡 Solution: Fix CMakeLists.txt line 626"
echo "   Likely issue: CURL::libcurl linking for dinero-miner"
echo "   Fix: Make CURL optional or skip dinero-miner for iOS"
