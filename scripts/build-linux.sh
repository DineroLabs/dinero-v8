#!/usr/bin/env bash
# Linux Build Script - Reproducible static build targeting glibc 2.17+

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== Dinero Linux Build ==="
echo "Project root: $PROJECT_ROOT"
echo "Build type: Release (reproducible)"
echo "Target: glibc 2.17+ (mostly static)"
echo "=========================="

cd "$PROJECT_ROOT"

# Clean previous build
rm -rf build
mkdir build
cd build

# Set reproducible timestamp
export SOURCE_DATE_EPOCH=1692576000

# Configure with static dependencies and security hardening
SOURCE_DATE_EPOCH=1692576000 cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_AR=/usr/bin/ar \
  -DCMAKE_RANLIB=/usr/bin/ranlib \
  -DDINERO_VENDOR_ROCKSDB=ON \
  -DDINERO_WITH_SNAPPY=ON \
  -DDINERO_WITH_LZ4=ON \
  -DDINERO_WITH_ZSTD=ON \
  -DDINERO_WITH_VULKAN=OFF \
  -DDINERO_LINUX_HARDENING=ON

echo ""
echo "=== Building Dinero ==="
cmake --build . --parallel

echo ""
echo "=== Verifying Static Linking ==="
if command -v ldd >/dev/null 2>&1; then
    echo "Checking dinerod dependencies:"
    echo "Expected: only glibc, libstdc++, libgcc_s, and system libs"
    echo ""
    
    if ldd bin/dinerod | grep -E "(ssl|crypto|rocksdb|snappy|lz4|zstd)"; then
        echo "❌ Found external dependencies!"
        exit 1
    else
        echo "✅ No external static dependencies found"
    fi
    
    echo ""
    echo "All dinerod dependencies:"
    ldd bin/dinerod
    
    echo ""
    echo "Dependency summary:"
    ldd bin/dinerod | grep -v 'linux-vdso\|ld-linux' | wc -l | xargs echo "Total dependencies:"
fi

echo ""
echo "=== Verifying glibc Version ==="
if command -v objdump >/dev/null 2>&1; then
    echo "Required glibc version:"
    objdump -T bin/dinerod | grep GLIBC | sed 's/.*GLIBC_/GLIBC_/' | sort -V | tail -1
fi

echo ""
echo "=== Build Summary ==="
echo "✅ Linux build completed successfully"
echo "📦 Binaries: $(pwd)/bin/"
echo "🔍 Static dependencies verified (glibc dynamic)"
echo "🛡️ Security hardening enabled"
echo "📋 SBOM: $(pwd)/sbom.json"
echo ""
echo "To verify reproducibility, run this script again and compare binaries."
