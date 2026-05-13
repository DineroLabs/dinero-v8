#!/bin/bash
# DineroCoin v0.6.0 Minimal Release Script
# Builds core components without problematic comprehensive app

set -e

echo "🚀 Building DineroCoin v0.6.0 Release Artifacts"
echo "==============================================="

# Clean build
rm -rf build-release
mkdir -p build-release
cd build-release

# Configure with minimal targets
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DDIN_BUILD_TESTS=OFF \
  -DDIN_BUILD_QT_TESTS=OFF \
  -DDIN_BUILD_LEGACY_TESTS=OFF \
  -DDIN_BUILD_SANDBOX=OFF \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

echo "📦 Building core components..."

# Build only essential targets
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) \
  dinerod \
  dinero-cli \
  dinero-modern-gui

echo "✅ Core build complete"

# Create distribution directory
mkdir -p dist

# Copy binaries
cp bin/dinerod dist/
cp bin/dinero-cli dist/
if [ -f "bin/dinero-modern-gui.app/Contents/MacOS/dinero-modern-gui" ]; then
    cp -r bin/dinero-modern-gui.app dist/
fi

# Generate checksums
cd dist
echo "🔐 Generating SHA256 checksums..."
shasum -a 256 * > SHA256SUMS.txt

echo "📋 Release artifacts:"
ls -la

echo ""
echo "🎉 DineroCoin v0.6.0 minimal release build complete!"
echo "📁 Artifacts available in: build-release/dist/"
