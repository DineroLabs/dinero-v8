#!/bin/bash
# Linux native build script for Dinero
# Run this on each Linux server after git pull

set -e

echo "🔨 Building Dinero for Linux x86-64..."

# Clean old builds (optional)
# rm -rf build

# Configure
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SANITIZERS=OFF \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_CXX_COMPILER=g++

# Build daemon + CLI
cmake --build build --target dinerod dinero-cli -j$(nproc)

echo "✅ Build complete!"
echo ""
echo "Binaries:"
ls -lh build/bin/dinerod build/bin/dinero-cli

echo ""
echo "To start daemon:"
echo "  ./build/bin/dinerod -daemon -datadir=~/dinero-data"

