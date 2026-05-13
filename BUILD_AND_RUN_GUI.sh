#!/bin/bash
# Build and Run Dinero Qt GUI
# Date: October 3, 2025

set -e

echo "🔨 Building Dinero Qt GUI..."
echo "=============================="
cd /Users/haydarevich/Documents/DineroCoin/gui

# Configure if needed
if [ ! -d "build" ]; then
  echo "Configuring CMake..."
  cmake -S . -B build \
    -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos" \
    -DCMAKE_BUILD_TYPE=Release
fi

# Build
echo "Building..."
cmake --build build -j8

echo ""
echo "✅ Build complete!"
echo ""
echo "🚀 To run the GUI:"
echo "   ./gui/build/dinero-qt"
echo ""
echo "📝 Make sure the daemon is running first:"
echo "   ./build/bin/dinerod -datadir=/Users/haydarevich/Documents/DineroCoin/data -rpcport=20998"

