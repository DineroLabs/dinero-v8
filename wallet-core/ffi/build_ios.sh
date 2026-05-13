#!/bin/bash
# wallet-core/ffi/build_ios.sh
# Build DineroCoin Wallet FFI library for iOS

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${DINERO_PROJECT_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
IOS_BUILD_DIR="${DINERO_IOS_BUILD_DIR:-${PROJECT_DIR}/build-ios}"
IOS_FFI_OUTPUT_DIR="${DINERO_IOS_FFI_OUTPUT_DIR:-${PROJECT_DIR}/build-ios-artifacts/FFI}"
IOS_DEPLOYMENT_TARGET="13.0"

echo "🍎 Building DineroCoin Wallet FFI for iOS"
echo "=========================================="
echo ""

cd "${PROJECT_DIR}"

if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
    echo "❌ Error: PROJECT_DIR does not look like repo root: ${PROJECT_DIR}"
    echo "   Set DINERO_PROJECT_DIR to your Dinero repo root."
    exit 1
fi

# Check if CMake is available
if ! command -v cmake &> /dev/null; then
    echo "❌ Error: CMake not found"
    echo "   Install: brew install cmake"
    exit 1
fi

# Create iOS build directory
mkdir -p "${IOS_BUILD_DIR}"

echo "📋 Configuring CMake for iOS..."
echo "   Deployment Target: iOS ${IOS_DEPLOYMENT_TARGET}"
echo "   Architecture: arm64"
echo "   Note: CURL optional (only needed for OpenKYC)"
echo ""

# Configure for iOS (make CURL optional)
cmake -S . -B "${IOS_BUILD_DIR}" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
  -DCMAKE_XCODE_ATTRIBUTE_VALID_ARCHS="arm64" \
  -DCMAKE_XCODE_ATTRIBUTE_ARCHS="arm64" \
  -DCURL_FOUND=OFF \
  2>&1 | tail -30

echo ""
echo "🔨 Building FFI library..."
cmake --build "${IOS_BUILD_DIR}" --target dinero_wallet_ffi -j8 2>&1 | tail -30

# Check if library was built
LIB_PATH="${IOS_BUILD_DIR}/lib/libdinero_wallet_ffi.a"
if [ -f "${LIB_PATH}" ]; then
    echo ""
    echo "✅ FFI library built successfully!"
    echo "   Location: ${LIB_PATH}"
    echo ""
    
    # Show library info
    echo "📦 Library Information:"
    file "${LIB_PATH}"
    echo ""
    lipo -info "${LIB_PATH}" 2>/dev/null || echo "   (lipo info not available)"
    
    # Copy to iOS project
    echo ""
    echo "📦 Copying to output directory..."
    mkdir -p "${IOS_FFI_OUTPUT_DIR}"
    cp "${LIB_PATH}" "${IOS_FFI_OUTPUT_DIR}/"
    echo "✅ Library copied to: ${IOS_FFI_OUTPUT_DIR}"
    
    echo ""
    echo "📋 Next Steps:"
    echo "   1. Open Dinero.xcodeproj in Xcode"
    echo "   2. Add FFI/ folder to project"
    echo "   3. Configure linker flags (see INTEGRATION_COMPLETE.md)"
    echo "   4. Build and run!"
else
    echo ""
    echo "❌ Error: Library not found at ${LIB_PATH}"
    echo "   Check build output above for errors"
    exit 1
fi
