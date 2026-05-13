#!/bin/bash
# Build vendored protobuf statically for zero-dependency release builds
#
# This script builds protobuf 25.3 from source with:
# - Static linking only (no shared libraries)
# - Bundled abseil-cpp (no system dependencies)
# - Minimal build (no tests, examples)
# - Optimized for size and portability
#
# Result: libprotobuf.a + protoc binary with zero runtime dependencies
# (except system libc++/libSystem on macOS, glibc on Linux)

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Auto-detect vendored protobuf directory (version-agnostic)
PROTOBUF_SRC=$(ls -d "$PROJECT_ROOT"/third_party/protobuf* 2>/dev/null | head -n 1)

if [ -z "$PROTOBUF_SRC" ] || [ ! -d "$PROTOBUF_SRC" ]; then
  echo "❌ Vendored protobuf source not found in third_party/"
  echo "   Expected a directory like:"
  echo "     third_party/protobuf-*"
  echo "     third_party/protobuf"
  exit 1
fi

PROTOBUF_BUILD="$PROJECT_ROOT/third_party/protobuf-build"
PROTOBUF_INSTALL="$PROJECT_ROOT/third_party/protobuf-install"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Building Vendored Protobuf (Static, Zero Dependencies)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔒 Using vendored protobuf: $(basename "$PROTOBUF_SRC")"
echo "Source:  $PROTOBUF_SRC"
echo "Build:   $PROTOBUF_BUILD"
echo "Install: $PROTOBUF_INSTALL"
echo ""

# Check if already built
if [ -f "$PROTOBUF_INSTALL/lib/libprotobuf.a" ] && [ -f "$PROTOBUF_INSTALL/bin/protoc" ]; then
    echo "✅ Vendored protobuf already built at: $PROTOBUF_INSTALL"
    echo "   To rebuild, run: rm -rf $PROTOBUF_BUILD $PROTOBUF_INSTALL"
    exit 0
fi

# Initialize submodules (abseil-cpp is required)
echo "📦 Initializing protobuf submodules (abseil-cpp)..."
cd "$PROTOBUF_SRC"
git submodule update --init --depth 1 third_party/abseil-cpp

# Patch abseil for ARM64 (remove x86-specific compiler flags)
if [[ "$(uname -m)" == "arm64" ]]; then
    echo "🔧 Patching abseil for ARM64 (removing x86-specific compiler flags)..."
    # Remove SSE and AES flags that don't work on ARM64
    if [ -f third_party/abseil-cpp/absl/copts/GENERATED_AbseilCopts.cmake ]; then
        sed -i.bak '/-msse4\.1/d;/-maes/d;/-mavx/d' \
            third_party/abseil-cpp/absl/copts/GENERATED_AbseilCopts.cmake
        echo "   Patched GENERATED_AbseilCopts.cmake"
    fi
    # Also patch the source generator to prevent regeneration
    if [ -f third_party/abseil-cpp/absl/copts/copts.py ]; then
        sed -i.bak 's/"-msse4\.1"/""/g;s/"-maes"/""/g;s/"-mavx"/""/g' \
            third_party/abseil-cpp/absl/copts/copts.py
        echo "   Patched copts.py"
    fi
fi

cd "$PROJECT_ROOT"

# Clean previous build
rm -rf "$PROTOBUF_BUILD" "$PROTOBUF_INSTALL"
mkdir -p "$PROTOBUF_BUILD"

# Configure
echo "⚙️  Configuring protobuf..."
cd "$PROTOBUF_BUILD"

# Detect architecture for cross-platform builds
if [[ "$OSTYPE" == "darwin"* ]]; then
    ARCH=$(uname -m)
    if [ "$ARCH" = "arm64" ]; then
        CMAKE_ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=arm64"
    else
        CMAKE_ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=x86_64"
    fi
else
    CMAKE_ARCH_FLAGS=""
fi

cmake "$PROTOBUF_SRC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PROTOBUF_INSTALL" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    ${CMAKE_ARCH_FLAGS} \
    -DBUILD_SHARED_LIBS=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_EXAMPLES=OFF \
    -Dprotobuf_BUILD_PROTOC_BINARIES=ON \
    -Dprotobuf_BUILD_LIBPROTOC=ON \
    -Dprotobuf_ABSL_PROVIDER=module \
    -Dprotobuf_WITH_ZLIB=OFF \
    -Dprotobuf_INSTALL=ON \
    -DABSL_USE_SYSTEM_INCLUDES=ON \
    -DABSL_ENABLE_INSTALL=ON \
    -DABSL_USE_EXTERNAL_GOOGLETEST=OFF \
    -DABSL_BUILD_TESTING=OFF

echo ""
echo "🔨 Building protobuf (this may take 2-3 minutes)..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "📦 Installing to: $PROTOBUF_INSTALL"
make install

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ Vendored Protobuf Build Complete!"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Installed files:"
echo "  Compiler:     $PROTOBUF_INSTALL/bin/protoc"
echo "  Library:      $PROTOBUF_INSTALL/lib/libprotobuf.a"
echo "  Headers:      $PROTOBUF_INSTALL/include/"
echo ""

# Verify binary has no Homebrew dependencies (macOS only)
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "🔍 Verifying protoc has no Homebrew dependencies..."
    HOMEBREW_DEPS=$(otool -L "$PROTOBUF_INSTALL/bin/protoc" | grep -c "/opt/homebrew" || true)
    if [ "$HOMEBREW_DEPS" -eq 0 ]; then
        echo "✅ VERIFIED: protoc has zero Homebrew dependencies"
        otool -L "$PROTOBUF_INSTALL/bin/protoc" | grep -v "$(basename "$PROTOBUF_INSTALL/bin/protoc")"
    else
        echo "❌ WARNING: protoc has $HOMEBREW_DEPS Homebrew dependencies:"
        otool -L "$PROTOBUF_INSTALL/bin/protoc" | grep "/opt/homebrew"
        exit 1
    fi
fi

echo ""
echo "Next: Rebuild dinero with DINERO_RELEASE=ON to use vendored protobuf"
