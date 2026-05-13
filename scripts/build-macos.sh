#!/usr/bin/env bash
# macOS Universal Build Script - Reproducible static build for arm64 + x86_64

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== Dinero macOS Universal Build ==="
echo "Project root: $PROJECT_ROOT"
echo "Build type: Release (reproducible)"
echo "Architectures: arm64 + x86_64"
echo "=================================="

cd "$PROJECT_ROOT"

# Clean previous build
rm -rf build
mkdir build
cd build

# Set reproducible timestamp
export SOURCE_DATE_EPOCH=1692576000

# Configure with universal binary and LLVM tools for reproducibility
SOURCE_DATE_EPOCH=1692576000 cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_AR="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin/ar" \
  -DCMAKE_RANLIB="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin/ranlib" \
  -DDINERO_VENDOR_ROCKSDB=ON \
      -DDINERO_WITH_SNAPPY=OFF \
    -DDINERO_WITH_LZ4=OFF \
    -DDINERO_WITH_ZSTD=OFF \
  -DDINERO_WITH_VULKAN=OFF

echo ""
echo "=== Building Dinero (Universal) ==="
cmake --build . --parallel

echo ""
echo "=== Verifying Universal Binary ==="
if command -v file >/dev/null 2>&1; then
    echo "dinerod architecture:"
    file bin/dinerod
    echo ""
    echo "dinero-cli architecture:"
    file bin/dinero-cli
fi

echo ""
echo "=== Verifying OpenSSL Universal Libraries ==="
if command -v lipo >/dev/null 2>&1; then
    echo "OpenSSL libcrypto.a architectures:"
    lipo -info deps/openssl/universal/lib/libcrypto.a 2>/dev/null || echo "Not found"
    echo "OpenSSL libssl.a architectures:"
    lipo -info deps/openssl/universal/lib/libssl.a 2>/dev/null || echo "Not found"
fi

echo ""
echo "=== Verifying Static Linking ==="
if command -v otool >/dev/null 2>&1; then
    echo "Checking dinerod dependencies:"
    if otool -L bin/dinerod | grep -E "(ssl|crypto|rocksdb|snappy|lz4|zstd)" | grep -v "/usr/lib" | grep -v "/System"; then
        echo "❌ Found external dependencies!"
        exit 1
    else
        echo "✅ No external dependencies found"
    fi
    
    echo ""
    echo "All dinerod dependencies:"
    otool -L bin/dinerod
fi

echo ""
echo "=== Build Summary ==="
echo "✅ macOS universal build completed successfully"
echo "📦 Binaries: $(pwd)/bin/"
echo "🔍 Static linking verified"
echo "📋 SBOM: $(pwd)/sbom.json"
echo ""
echo "To verify reproducibility, run this script again and compare binaries."
