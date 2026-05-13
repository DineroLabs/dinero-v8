#!/bin/bash
set -e

# Build OpenSSL for iOS arm64
# This script builds static libraries for iOS device (arm64)

OPENSSL_VERSION="3.2.1"
BUILD_DIR="build-openssl-ios"
INSTALL_DIR="${PWD}/${BUILD_DIR}/install"
IOS_SDK_VERSION=$(xcrun --sdk iphoneos --show-sdk-version)
IOS_SDK_PATH=$(xcrun --sdk iphoneos --show-sdk-path)
IOS_MIN_VERSION="11.0"

echo "🔧 Building OpenSSL ${OPENSSL_VERSION} for iOS"
echo "   SDK: ${IOS_SDK_VERSION}"
echo "   Path: ${IOS_SDK_PATH}"
echo "   Min iOS: ${IOS_MIN_VERSION}"

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Download OpenSSL if not already present
if [ ! -d "openssl-${OPENSSL_VERSION}" ]; then
    echo "📥 Downloading OpenSSL ${OPENSSL_VERSION}..."
    curl -L "https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz" -o openssl-${OPENSSL_VERSION}.tar.gz
    tar -xzf openssl-${OPENSSL_VERSION}.tar.gz
fi

cd "openssl-${OPENSSL_VERSION}"

# Clean previous build
make clean 2>/dev/null || true

# Configure for iOS arm64
echo "⚙️  Configuring OpenSSL for iOS arm64..."
CC=$(xcrun --sdk iphoneos --find clang)
CXX=$(xcrun --sdk iphoneos --find clang++)
CFLAGS="-arch arm64 -mios-version-min=${IOS_MIN_VERSION} -isysroot ${IOS_SDK_PATH} -fembed-bitcode"
CXXFLAGS="${CFLAGS}"

export CC
export CXX
export CFLAGS
export CXXFLAGS

./Configure \
    ios64-cross \
    --prefix="${INSTALL_DIR}" \
    --openssldir="${INSTALL_DIR}" \
    no-shared \
    no-tests \
    no-docs \
    no-async \
    no-weak-ssl-ciphers \
    enable-ec_nistp_64_gcc_128

# Build
echo "🔨 Building OpenSSL..."
make -j$(sysctl -n hw.ncpu)

# Install
echo "📦 Installing OpenSSL..."
make install_sw

echo ""
echo "✅ OpenSSL build complete!"
echo "   Libraries: ${INSTALL_DIR}/lib/"
ls -lh "${INSTALL_DIR}/lib/"*.a 2>/dev/null || echo "   (checking for libraries...)"

# Copy to iOS project
if [ -f "${INSTALL_DIR}/lib/libcrypto.a" ] && [ -f "${INSTALL_DIR}/lib/libssl.a" ]; then
    echo ""
    echo "📋 Libraries found:"
    ls -lh "${INSTALL_DIR}/lib/"*.a
    echo ""
    echo "💡 Next step: Copy libraries to iOS project FFI directory"
    echo "   cp ${INSTALL_DIR}/lib/libcrypto.a /Users/haydarevich/Documents/DineroiOS/Dinero/Dinero/FFI/"
    echo "   cp ${INSTALL_DIR}/lib/libssl.a /Users/haydarevich/Documents/DineroiOS/Dinero/Dinero/FFI/"
fi
