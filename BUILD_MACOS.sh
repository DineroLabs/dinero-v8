#!/bin/bash

# 🚀 Dinero Cryptocurrency - macOS Build Script
# This script builds Dinero on macOS (Intel/Apple Silicon)

set -e  # Exit on any error

echo "🚀 Building Dinero Cryptocurrency on macOS..."
echo "=============================================="

# Check prerequisites
echo "🔍 Checking prerequisites..."

# Check if Xcode Command Line Tools are installed
if ! command -v clang++ &> /dev/null; then
    echo "❌ Xcode Command Line Tools not found!"
    echo "   Install with: xcode-select --install"
    exit 1
fi

# Check if CMake is installed
if ! command -v cmake &> /dev/null; then
    echo "❌ CMake not found!"
    echo "   Install with: brew install cmake"
    echo "   Or download from: https://cmake.org/download/"
    exit 1
fi

# Check if Qt6 is available (optional)
QT_PATH=""
if [ -d "$HOME/Qt/6.9.1/macos" ]; then
    QT_PATH="$HOME/Qt/6.9.1/macos"
    echo "✅ Found Qt6 at: $QT_PATH"
elif [ -d "/usr/local/opt/qt6" ]; then
    QT_PATH="/usr/local/opt/qt6"
    echo "✅ Found Qt6 at: $QT_PATH"
elif command -v brew &> /dev/null && brew list qt6 &> /dev/null; then
    QT_PATH="$(brew --prefix qt6)"
    echo "✅ Found Qt6 via Homebrew at: $QT_PATH"
else
    echo "⚠️  Qt6 not found - GUI components will not be built"
    echo "   Install Qt6 for GUI support"
fi

echo "✅ Prerequisites check passed!"

# Create build directory
echo "📁 Creating build directory..."
rm -rf build
mkdir -p build
cd build

# Configure build
echo "⚙️  Configuring build..."
if [ -n "$QT_PATH" ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PATH" -DENABLE_SANITIZERS=OFF
else
    cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF -DBUILD_GUI=OFF
fi

# Build
echo "🔨 Building Dinero..."
cmake --build . -j$(sysctl -n hw.ncpu)

echo "✅ Build completed successfully!"
echo ""
echo "🚀 Your Dinero binaries are ready in: ./build/bin/"
echo ""
echo "📋 Next steps:"
echo "1. Create data directory: mkdir -p data"
echo "2. Start daemon: ./build/bin/dinerod -datadir=./data -rpcport=20998 -port=20999 -daemon=0 -server=1"
echo "3. Create wallet and start mining!"
echo ""
echo "📚 See README.md for complete usage instructions"
echo "⚡ See QUICK_START.md for essential commands"
