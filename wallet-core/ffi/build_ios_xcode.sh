#!/bin/bash
# wallet-core/ffi/build_ios_simple.sh
# Build DineroCoin Wallet FFI library for iOS (simplified, without RocksDB)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${DINERO_PROJECT_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
IOS_BUILD_DIR="${DINERO_IOS_BUILD_DIR:-${PROJECT_DIR}/build-ios-simple}"
IOS_DEPLOYMENT_TARGET="13.0"

echo "🍎 Building DineroCoin Wallet FFI for iOS (Simplified)"
echo "======================================================"
echo ""

cd "${PROJECT_DIR}"

if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
    echo "❌ Error: PROJECT_DIR does not look like repo root: ${PROJECT_DIR}"
    echo "   Set DINERO_PROJECT_DIR to your Dinero repo root."
    exit 1
fi

# Use Xcode generator for better iOS support
echo "📋 Generating Xcode project for iOS..."
echo "   Deployment Target: iOS ${IOS_DEPLOYMENT_TARGET}"
echo "   Architecture: arm64"
echo ""

# Generate Xcode project
cmake -S . -B "${IOS_BUILD_DIR}" \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DCURL_FOUND=OFF \
  2>&1 | grep -E "(Configuring|Generating|Found|Error)" | tail -20

echo ""
echo "✅ Xcode project generated!"
echo ""
echo "📋 Next Steps:"
echo "   1. Open Xcode project:"
echo "      open ${IOS_BUILD_DIR}/DineroCoin.xcodeproj"
echo ""
echo "   2. Select 'dinero_wallet_ffi' scheme"
echo ""
echo "   3. Select Generic iOS Device or specific device"
echo ""
echo "   4. Product → Build (⌘B)"
echo ""
echo "   5. Find library at:"
echo "      ${IOS_BUILD_DIR}/lib/libdinero_wallet_ffi.a"
echo ""
echo "   OR build from command line:"
echo "   xcodebuild -project ${IOS_BUILD_DIR}/DineroCoin.xcodeproj \\"
echo "              -scheme dinero_wallet_ffi \\"
echo "              -sdk iphoneos \\"
echo "              -configuration Release \\"
echo "              -arch arm64"
