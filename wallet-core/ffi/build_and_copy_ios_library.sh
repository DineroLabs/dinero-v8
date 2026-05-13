#!/bin/bash
# build_and_copy_ios_library.sh
# Build iOS FFI library and copy to iOS project when complete

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${DINERO_PROJECT_DIR:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
IOS_PROJECT="${DINERO_IOS_FFI_OUTPUT_DIR:-${PROJECT_DIR}/build-ios-artifacts/FFI}"
BUILD_DIR="${DINERO_IOS_BUILD_DIR:-${PROJECT_DIR}/build-ios-real}"

echo "🔨 Building Real iOS Library"
echo "============================"
echo ""

cd "${PROJECT_DIR}"

if [ ! -f "${PROJECT_DIR}/CMakeLists.txt" ]; then
    echo "❌ Error: PROJECT_DIR does not look like repo root: ${PROJECT_DIR}"
    echo "   Set DINERO_PROJECT_DIR to your Dinero repo root."
    exit 1
fi

# Ensure Xcode project exists
if [ ! -f "${BUILD_DIR}/Dinero.xcodeproj/project.pbxproj" ]; then
    echo "📋 Generating Xcode project..."
    cmake -S . -B "${BUILD_DIR}" \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_SANITIZERS=OFF \
      -DCURL_FOUND=OFF \
      -G Xcode \
      2>&1 | grep -E "(Configuring|Generating|Skipping)" | tail -5
fi

echo ""
echo "🔨 Building dinero_wallet_ffi library..."
echo "   This will take 5-10 minutes (RocksDB compilation)"
echo ""

# Build the library
xcodebuild -project "${BUILD_DIR}/Dinero.xcodeproj" \
  -scheme dinero_wallet_ffi \
  -sdk iphoneos \
  -configuration Release \
  -arch arm64 \
  ONLY_ACTIVE_ARCH=NO \
  CODE_SIGN_IDENTITY="" \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGNING_ALLOWED=NO \
  2>&1 | tee "${BUILD_DIR}/build.log" | grep -E "(Building|Linking|error|warning|Built target|BUILD)" | tail -20

echo ""
echo "📋 Searching for built library..."

# Find the library
LIB_PATH=$(find "${BUILD_DIR}" -name "libdinero_wallet_ffi.a" -type f 2>/dev/null | head -1)

if [ -z "$LIB_PATH" ]; then
    # Try alternative locations
    LIB_PATH=$(find "${BUILD_DIR}" -path "*/Release-iphoneos/*.a" -name "*wallet_ffi*" -type f 2>/dev/null | head -1)
fi

if [ -n "$LIB_PATH" ] && [ -f "$LIB_PATH" ]; then
    SIZE=$(stat -f%z "$LIB_PATH" 2>/dev/null || stat -c%s "$LIB_PATH" 2>/dev/null || echo "0")
    
    echo "✅ Found library: $LIB_PATH"
    echo "   Size: $SIZE bytes"
    
    if [ "$SIZE" -gt 10000 ]; then
        echo "   ✅ Real library (size > 10KB)"
        
        # Verify architecture
        lipo -info "$LIB_PATH" || echo "   (lipo info not available)"
        file "$LIB_PATH"
        
        echo ""
        echo "📦 Copying to output directory..."
        mkdir -p "${IOS_PROJECT}"
        cp "$LIB_PATH" "${IOS_PROJECT}/libdinero_wallet_ffi.a"
        
        echo "✅ Real library copied to: ${IOS_PROJECT}"
        echo ""
        echo "📋 Final Library:"
        ls -lh "${IOS_PROJECT}/libdinero_wallet_ffi.a"
        echo ""
        echo "🎉 Real iOS library ready!"
    else
        echo "   ⚠️  Library seems too small ($SIZE bytes)"
        echo "   May be incomplete - check build log for errors"
    fi
else
    echo "❌ Library not found after build"
    echo ""
    echo "📋 Build log summary:"
    tail -20 "${BUILD_DIR}/build.log" | grep -E "(error|warning|BUILD)" || echo "   (No errors found in tail)"
    echo ""
    echo "💡 Check full log: ${BUILD_DIR}/build.log"
fi
