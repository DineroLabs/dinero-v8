#!/bin/bash
# Build script for Dinero Mobile Wallet
# This builds the C++ wallet core first, then the Tauri mobile app

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOBILE_DIR="$PROJECT_ROOT/mobile-tauri"

echo "🚀 Building Dinero Mobile Wallet"
echo "=================================="
echo ""

# Step 1: Build C++ wallet core library
echo "📦 Step 1: Building C++ wallet core..."
cd "$PROJECT_ROOT"

# Create build directory if it doesn't exist
mkdir -p build-mobile
cd build-mobile

# Configure CMake (if not already configured)
if [ ! -f "CMakeCache.txt" ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_INSTALL_PREFIX="$PROJECT_ROOT/build-mobile/install"
fi

# Build wallet core library target
echo "   Building libdinero_wallet.a..."
cmake --build . --target dinero_wallet -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

if [ ! -f "libdinero_wallet.a" ]; then
    echo "   ⚠️  Warning: libdinero_wallet.a not found in build-mobile/"
    echo "   Trying alternative build location..."
    
    # Try build/ directory
    if [ -f "$PROJECT_ROOT/build/libdinero_wallet.a" ]; then
        echo "   ✅ Found in build/"
        cp "$PROJECT_ROOT/build/libdinero_wallet.a" .
    else
        echo "   ❌ Error: libdinero_wallet.a not found!"
        echo "   Please build wallet core first:"
        echo "     cd DineroCoin && cmake --build build -t dinero_wallet"
        exit 1
    fi
fi

echo "   ✅ Wallet core built successfully"
echo ""

# Step 2: Build Tauri mobile app
echo "📱 Step 2: Building Tauri mobile app..."
cd "$MOBILE_DIR"

# Install npm dependencies if needed
if [ ! -d "node_modules" ]; then
    echo "   Installing npm dependencies..."
    npm install
fi

# Build Tauri app
echo "   Building Tauri app..."
npm run tauri build

echo ""
echo "✅ Build complete!"
echo ""
echo "📦 Output:"
echo "   iOS: $MOBILE_DIR/src-tauri/target/aarch64-apple-ios/release/dinero-mobile.app"
echo "   Android: $MOBILE_DIR/src-tauri/target/aarch64-linux-android/release/dinero-mobile.apk"
echo ""

