#!/bin/bash
#
# Build secp256k1-zkp for Apple platforms
#
# Produces static libraries with rangeproof module for:
# - iOS device (arm64)
# - iOS simulator (arm64 + x86_64)
# - macOS (arm64)
#
# Output: third_party/secp256k1-zkp/build-ios/
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SECP_DIR="$PROJECT_ROOT/third_party/secp256k1-zkp"
BUILD_DIR="$SECP_DIR/build-ios"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "========================================"
echo "Building secp256k1-zkp for iOS"
echo "========================================"
echo ""

# Check for secp256k1-zkp source
if [ ! -d "$SECP_DIR" ]; then
    echo -e "${RED}Error: secp256k1-zkp not found at $SECP_DIR${NC}"
    exit 1
fi

# Check for Xcode
if ! command -v xcrun &> /dev/null; then
    echo -e "${RED}Error: Xcode command line tools not found${NC}"
    exit 1
fi

# Get SDK paths
IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
SIM_SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
MACOS_SDK=$(xcrun --sdk macosx --show-sdk-path)

echo "iOS SDK: $IOS_SDK"
echo "Simulator SDK: $SIM_SDK"
echo "macOS SDK: $MACOS_SDK"
echo ""

# Clean previous build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# CMake common options
CMAKE_COMMON_OPTIONS=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    -DSECP256K1_DISABLE_SHARED=ON
    -DSECP256K1_BUILD_BENCHMARK=OFF
    -DSECP256K1_BUILD_TESTS=OFF
    -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF
    -DSECP256K1_BUILD_CTIME_TESTS=OFF
    -DSECP256K1_BUILD_EXAMPLES=OFF
    # Enable modules for DineroCoin
    -DSECP256K1_ENABLE_MODULE_ECDH=ON
    -DSECP256K1_ENABLE_MODULE_RECOVERY=ON
    -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON
    -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON
    -DSECP256K1_ENABLE_MODULE_GENERATOR=ON
    -DSECP256K1_ENABLE_MODULE_RANGEPROOF=ON
    -DSECP256K1_ENABLE_MODULE_SURJECTIONPROOF=OFF
    -DSECP256K1_ENABLE_MODULE_WHITELIST=OFF
    -DSECP256K1_ENABLE_MODULE_MUSIG=ON
    -DSECP256K1_ENABLE_MODULE_ECDSA_ADAPTOR=OFF
    -DSECP256K1_ENABLE_MODULE_ECDSA_S2C=OFF
    -DSECP256K1_ENABLE_MODULE_BPPP=OFF
    # Disable assembly for cross-compile
    -DSECP256K1_ASM=OFF
)

# ============================================
# Build for iOS device (arm64)
# ============================================
echo -e "${GREEN}[1/4] Building for iOS device (arm64)...${NC}"

IOS_BUILD_DIR="$BUILD_DIR/ios-arm64"
mkdir -p "$IOS_BUILD_DIR"

cmake -S "$SECP_DIR" -B "$IOS_BUILD_DIR" \
    "${CMAKE_COMMON_OPTIONS[@]}" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_OSX_SYSROOT="$IOS_SDK" \
    -DCMAKE_C_FLAGS="-fembed-bitcode -fno-stack-check"

cmake --build "$IOS_BUILD_DIR" --config Release -j$(sysctl -n hw.ncpu)

if [ -f "$IOS_BUILD_DIR/src/libsecp256k1.a" ]; then
    echo -e "${GREEN}✓ iOS device build complete${NC}"
else
    echo -e "${RED}✗ iOS device build failed${NC}"
    exit 1
fi

# ============================================
# Build for iOS simulator (arm64 - Apple Silicon)
# ============================================
echo ""
echo -e "${GREEN}[2/4] Building for iOS simulator arm64...${NC}"

SIM_ARM64_BUILD_DIR="$BUILD_DIR/ios-sim-arm64"
mkdir -p "$SIM_ARM64_BUILD_DIR"

cmake -S "$SECP_DIR" -B "$SIM_ARM64_BUILD_DIR" \
    "${CMAKE_COMMON_OPTIONS[@]}" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_OSX_SYSROOT="$SIM_SDK" \
    -DCMAKE_C_FLAGS="-target arm64-apple-ios14.0-simulator -fno-stack-check"

cmake --build "$SIM_ARM64_BUILD_DIR" --config Release -j$(sysctl -n hw.ncpu)

if [ -f "$SIM_ARM64_BUILD_DIR/src/libsecp256k1.a" ]; then
    echo -e "${GREEN}✓ iOS simulator arm64 build complete${NC}"
else
    echo -e "${RED}✗ iOS simulator arm64 build failed${NC}"
    exit 1
fi

# ============================================
# Build for iOS simulator (x86_64 - Intel)
# ============================================
echo ""
echo -e "${GREEN}[3/4] Building for iOS simulator x86_64...${NC}"

SIM_X64_BUILD_DIR="$BUILD_DIR/ios-sim-x64"
mkdir -p "$SIM_X64_BUILD_DIR"

cmake -S "$SECP_DIR" -B "$SIM_X64_BUILD_DIR" \
    "${CMAKE_COMMON_OPTIONS[@]}" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_OSX_SYSROOT="$SIM_SDK" \
    -DCMAKE_C_FLAGS="-target x86_64-apple-ios14.0-simulator -fno-stack-check"

cmake --build "$SIM_X64_BUILD_DIR" --config Release -j$(sysctl -n hw.ncpu)

if [ -f "$SIM_X64_BUILD_DIR/src/libsecp256k1.a" ]; then
    echo -e "${GREEN}✓ iOS simulator x86_64 build complete${NC}"
else
    echo -e "${YELLOW}⚠ iOS simulator x86_64 build failed (optional for Apple Silicon)${NC}"
fi

# ============================================
# Build for macOS (arm64)
# ============================================
echo ""
echo -e "${GREEN}[4/4] Building for macOS arm64...${NC}"

MAC_ARM64_BUILD_DIR="$BUILD_DIR/macos-arm64"
mkdir -p "$MAC_ARM64_BUILD_DIR"

cmake -S "$SECP_DIR" -B "$MAC_ARM64_BUILD_DIR" \
    "${CMAKE_COMMON_OPTIONS[@]}" \
    -DCMAKE_SYSTEM_NAME=Darwin \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_OSX_SYSROOT="$MACOS_SDK" \
    -DCMAKE_C_FLAGS="-fno-stack-check"

cmake --build "$MAC_ARM64_BUILD_DIR" --config Release -j$(sysctl -n hw.ncpu)

if [ -f "$MAC_ARM64_BUILD_DIR/src/libsecp256k1.a" ]; then
    echo -e "${GREEN}✓ macOS arm64 build complete${NC}"
else
    echo -e "${RED}✗ macOS arm64 build failed${NC}"
    exit 1
fi

# ============================================
# Create universal simulator library
# ============================================
echo ""
echo -e "${GREEN}Creating universal simulator library...${NC}"

UNIVERSAL_SIM_DIR="$BUILD_DIR/ios-sim-universal"
mkdir -p "$UNIVERSAL_SIM_DIR"

if [ -f "$SIM_X64_BUILD_DIR/src/libsecp256k1.a" ]; then
    lipo -create \
        "$SIM_ARM64_BUILD_DIR/src/libsecp256k1.a" \
        "$SIM_X64_BUILD_DIR/src/libsecp256k1.a" \
        -output "$UNIVERSAL_SIM_DIR/libsecp256k1.a"
else
    cp "$SIM_ARM64_BUILD_DIR/src/libsecp256k1.a" "$UNIVERSAL_SIM_DIR/libsecp256k1.a"
fi

echo -e "${GREEN}✓ Universal simulator library created${NC}"

# ============================================
# Create xcframework
# ============================================
echo ""
echo -e "${GREEN}Creating xcframework...${NC}"

XCFRAMEWORK_DIR="$BUILD_DIR/secp256k1.xcframework"
rm -rf "$XCFRAMEWORK_DIR"

# Copy headers
HEADERS_DIR="$BUILD_DIR/Headers"
rm -rf "$HEADERS_DIR"
mkdir -p "$HEADERS_DIR"
cp "$SECP_DIR/include/"*.h "$HEADERS_DIR/"

xcodebuild -create-xcframework \
    -library "$IOS_BUILD_DIR/src/libsecp256k1.a" -headers "$HEADERS_DIR" \
    -library "$UNIVERSAL_SIM_DIR/libsecp256k1.a" -headers "$HEADERS_DIR" \
    -library "$MAC_ARM64_BUILD_DIR/src/libsecp256k1.a" -headers "$HEADERS_DIR" \
    -output "$XCFRAMEWORK_DIR"

echo ""
echo "========================================"
echo -e "${GREEN}Build complete!${NC}"
echo "========================================"
echo ""
echo "Outputs:"
echo "  iOS device:      $IOS_BUILD_DIR/src/libsecp256k1.a"
echo "  iOS simulator:   $UNIVERSAL_SIM_DIR/libsecp256k1.a"
echo "  macOS arm64:     $MAC_ARM64_BUILD_DIR/src/libsecp256k1.a"
echo "  XCFramework:     $XCFRAMEWORK_DIR"
echo ""
echo "Headers: $HEADERS_DIR"
echo ""
echo "Library info:"
lipo -info "$IOS_BUILD_DIR/src/libsecp256k1.a"
lipo -info "$UNIVERSAL_SIM_DIR/libsecp256k1.a"
lipo -info "$MAC_ARM64_BUILD_DIR/src/libsecp256k1.a" 2>/dev/null || file "$MAC_ARM64_BUILD_DIR/src/libsecp256k1.a"
echo ""
echo "To use in DPI iOS build:"
echo "  1. Copy xcframework to DineroDPI project"
echo "  2. Enable rangeproof-ffi feature in Cargo.toml"
echo "  3. Link libsecp256k1.a in build script"
echo ""
