#!/bin/bash
# build_universal_ios.sh
# Build universal iOS library (arm64 device + arm64 simulator)
# This creates a fat binary that works on both device and simulator

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${DINERO_PROJECT_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
IOS_BUILD_DIR="${DINERO_IOS_BUILD_DIR:-${PROJECT_DIR}/build-ios-universal}"
IOS_FFI_OUTPUT_DIR="${DINERO_IOS_FFI_OUTPUT_DIR:-${PROJECT_DIR}/build-ios-artifacts/FFI}"
IOS_DEPLOYMENT_TARGET="13.0"

echo "🍎 Building Universal iOS Library (Device + Simulator)"
echo "======================================================"
echo ""

cd "${PROJECT_DIR}"

if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
    echo "❌ Error: PROJECT_DIR does not look like repo root: ${PROJECT_DIR}"
    echo "   Set DINERO_PROJECT_DIR to your Dinero repo root."
    exit 1
fi

# Clean previous builds
rm -rf "${IOS_BUILD_DIR}"
mkdir -p "${IOS_BUILD_DIR}"

echo "📋 Building for iOS Device (arm64)..."
DEVICE_BUILD="${IOS_BUILD_DIR}/device"
mkdir -p "${DEVICE_BUILD}"

cmake -S . -B "${DEVICE_BUILD}" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DCURL_FOUND=OFF \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  2>&1 | grep -E "(Configuring|Generating|Found|Error|CMake)" | tail -15

echo ""
echo "🔨 Building device library..."
cmake --build "${DEVICE_BUILD}" --target dinero_wallet_ffi -j8 --config Release 2>&1 | tail -20

DEVICE_LIB="${DEVICE_BUILD}/lib/libdinero_wallet_ffi.a"
if [ ! -f "${DEVICE_LIB}" ]; then
    echo ""
    echo "❌ Error: Device library not built"
    echo "   Expected: ${DEVICE_LIB}"
    exit 1
fi

echo ""
echo "📋 Building for iOS Simulator (arm64)..."
SIM_BUILD="${IOS_BUILD_DIR}/simulator"
mkdir -p "${SIM_BUILD}"

cmake -S . -B "${SIM_BUILD}" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DCURL_FOUND=OFF \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  2>&1 | grep -E "(Configuring|Generating|Found|Error|CMake)" | tail -15

echo ""
echo "🔨 Building simulator library..."
cmake --build "${SIM_BUILD}" --target dinero_wallet_ffi -j8 --config Release 2>&1 | tail -20

SIM_LIB="${SIM_BUILD}/lib/libdinero_wallet_ffi.a"
if [ ! -f "${SIM_LIB}" ]; then
    echo ""
    echo "❌ Error: Simulator library not built"
    echo "   Expected: ${SIM_LIB}"
    exit 1
fi

echo ""
echo "📦 Creating Universal Binary..."
UNIVERSAL_LIB="${IOS_BUILD_DIR}/lib/libdinero_wallet_ffi.a"
mkdir -p "$(dirname "${UNIVERSAL_LIB}")"

lipo -create "${DEVICE_LIB}" "${SIM_LIB}" -output "${UNIVERSAL_LIB}"

echo ""
echo "✅ Universal library created!"
echo "   Location: ${UNIVERSAL_LIB}"
echo ""

# Show library info
echo "📋 Library Information:"
lipo -info "${UNIVERSAL_LIB}"
file "${UNIVERSAL_LIB}"

echo ""
echo "📦 Copying to output directory..."
mkdir -p "${IOS_FFI_OUTPUT_DIR}"
cp "${UNIVERSAL_LIB}" "${IOS_FFI_OUTPUT_DIR}/"
echo "✅ Universal library copied to: ${IOS_FFI_OUTPUT_DIR}"

echo ""
echo "✅ Universal iOS library ready!"
echo "   - Works on: iOS Device (arm64) + iOS Simulator (arm64)"
echo "   - Deployment Target: iOS ${IOS_DEPLOYMENT_TARGET}+"
echo ""
echo "📋 Note: For Intel Mac simulators, you'll need x86_64 architecture"
echo "   Current build supports: Apple Silicon Macs (arm64) only"
