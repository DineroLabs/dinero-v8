#!/bin/bash

# Build script for Dinero GUI Wrapper
# Creates a Bitcoin-Qt style GUI that embeds the daemon

set -e

echo "🚀 Building Dinero GUI Wrapper (Bitcoin-Qt Style)"
echo "=================================================="

# Check if Qt6 is available
if ! command -v qmake6 &> /dev/null && ! command -v qmake &> /dev/null; then
    echo "❌ Qt6 not found. Please install Qt6 development packages:"
    echo ""
    echo "macOS:    brew install qt6"
    echo "Ubuntu:   sudo apt install qt6-base-dev qt6-tools-dev"
    echo "Windows:  Download from https://www.qt.io/download"
    echo ""
    exit 1
fi

# Create build directory
echo "📁 Creating build directory..."
mkdir -p build
cd build

# Configure with Qt support and all optimizations
echo "⚙️  Configuring build..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_QT=ON \
    -DBUILD_WALLET_GUI=ON \
    -DDINERO_VENDOR_ROCKSDB=ON \
    -DDINERO_WITH_SNAPPY=ON \
    -DDINERO_WITH_LZ4=ON \
    -DDINERO_WITH_ZSTD=ON

# Build the GUI wrapper
echo "🔨 Building GUI wrapper..."
cmake --build . --parallel --target dinero-embedded-qt6

# Check if build succeeded
if [ -f "bin/dinero-embedded-qt6" ] || [ -d "bin/dinero-embedded-qt6.app" ]; then
    echo ""
    echo "✅ Build successful!"
    echo ""
    echo "🎯 GUI Wrapper Features:"
    echo "  • Embedded daemon (no separate process)"
    echo "  • In-process RPC (20-100x faster than HTTP)"
    echo "  • Bitcoin-Qt style debug console"
    echo "  • Real-time blockchain monitoring"
    echo "  • Integrated wallet and mining controls"
    echo ""
    echo "🚀 Run the GUI wrapper:"
    echo ""
    echo "  # Regtest mode (recommended for testing):"
    echo "  ./bin/dinero-embedded-qt6 -regtest"
    echo ""
    echo "  # Testnet mode:"
    echo "  ./bin/dinero-embedded-qt6 -testnet"
    echo ""
    echo "  # Mainnet mode:"
    echo "  ./bin/dinero-embedded-qt6"
    echo ""
    echo "💡 Tips:"
    echo "  • Use Tools → Debug Console (Ctrl+Shift+D) for RPC commands"
    echo "  • Try commands like: getblockchaininfo, getnewaddress, help"
    echo "  • In regtest mode, you can generate blocks instantly"
    echo ""
    
    # Show file size and dependencies info
    if command -v ls &> /dev/null; then
        echo "📊 Binary info:"
        if [ -f "bin/dinero-embedded-qt6" ]; then
            ls -lh bin/dinero-embedded-qt6
        elif [ -d "bin/dinero-embedded-qt6.app" ]; then
            ls -lh bin/dinero-embedded-qt6.app/Contents/MacOS/dinero-embedded-qt6
        fi
    fi
    
    # Check dependencies on macOS
    if [[ "$OSTYPE" == "darwin"* ]] && [ -f "bin/dinero-embedded-qt6.app/Contents/MacOS/dinero-embedded-qt6" ]; then
        echo ""
        echo "🔍 Checking for external dependencies..."
        if otool -L bin/dinero-embedded-qt6.app/Contents/MacOS/dinero-embedded-qt6 | grep -q '/opt/homebrew'; then
            echo "⚠️  Warning: Found Homebrew dependencies"
            otool -L bin/dinero-embedded-qt6.app/Contents/MacOS/dinero-embedded-qt6 | grep '/opt/homebrew' || true
        else
            echo "✅ No problematic external dependencies found"
        fi
    fi
    
else
    echo ""
    echo "❌ Build failed!"
    echo "Check the error messages above for details."
    exit 1
fi
