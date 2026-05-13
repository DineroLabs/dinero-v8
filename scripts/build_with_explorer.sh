#!/bin/bash
# Build script for Dinero daemon with Explorer API v1
# This script shows how to build your daemon with the Explorer API integrated

set -e

echo "🚀 Building Dinero daemon with Explorer API v1..."

# Configuration
BUILD_TYPE=${BUILD_TYPE:-Release}
BUILD_DIR=${BUILD_DIR:-build-explorer}
INSTALL_PREFIX=${INSTALL_PREFIX:-/usr/local}
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}

echo "Configuration:"
echo "  Build type: $BUILD_TYPE"
echo "  Build directory: $BUILD_DIR"
echo "  Jobs: $JOBS"
echo

# Check dependencies
echo "🔍 Checking dependencies..."

# Check for required tools
for tool in cmake make pkg-config; do
    if ! command -v $tool &> /dev/null; then
        echo "❌ $tool is required but not installed"
        exit 1
    fi
done

# Check for required libraries
echo "Checking for SQLite3..."
if ! pkg-config --exists sqlite3; then
    echo "❌ SQLite3 development package is required"
    echo "   Ubuntu/Debian: sudo apt install libsqlite3-dev"
    echo "   macOS: brew install sqlite3"
    exit 1
fi

echo "Checking for OpenSSL..."
if ! pkg-config --exists openssl; then
    echo "❌ OpenSSL development package is required"
    echo "   Ubuntu/Debian: sudo apt install libssl-dev"
    echo "   macOS: brew install openssl"
    exit 1
fi

echo "Checking for JsonCpp..."
if ! pkg-config --exists jsoncpp; then
    echo "⚠️  JsonCpp not found via pkg-config, will use bundled version"
fi

echo "✅ Dependencies check complete"
echo

# Create build directory
echo "📁 Creating build directory..."
mkdir -p $BUILD_DIR
cd $BUILD_DIR

# Configure with CMake
echo "⚙️  Configuring with CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX \
    -DBUILD_TESTING=ON \
    -DBUILD_EXPLORER_API=ON \
    -DENABLE_SANITIZERS=OFF

# Build
echo "🔨 Building..."
cmake --build . -j$JOBS

# Run tests
echo "🧪 Running tests..."
if command -v ctest &> /dev/null; then
    ctest --output-on-failure -j$JOBS
else
    echo "⚠️  CTest not available, skipping tests"
fi

# Check if binaries were built
echo "✅ Checking built binaries..."
if [ -f "bin/dinerod" ]; then
    echo "✅ dinerod built successfully"
    ./bin/dinerod --version || echo "⚠️  Version check failed"
else
    echo "❌ dinerod not found"
    exit 1
fi

if [ -f "bin/test_explorer_api" ]; then
    echo "✅ Explorer API tests built"
    echo "Running Explorer API tests..."
    ./bin/test_explorer_api || echo "⚠️  Some tests failed"
else
    echo "⚠️  Explorer API tests not built"
fi

echo
echo "🎉 Build complete!"
echo
echo "📋 Next steps:"
echo "1. Start the daemon:"
echo "   cd $BUILD_DIR"
echo "   ./bin/dinerod -datadir=./test-data -rpcport=20998"
echo
echo "2. Test the Explorer API:"
echo "   curl -s localhost:20998/api/v1/health | jq"
echo "   curl -s localhost:20998/api/v1/chain/tip | jq"
echo
echo "3. Run comprehensive tests:"
echo "   ../scripts/test_explorer_api.sh"
echo
echo "4. View API documentation:"
echo "   Open docs/explorer_openapi_complete.yaml in Swagger UI"
echo

# Optional: Install
if [ "$1" = "--install" ]; then
    echo "📦 Installing..."
    sudo cmake --install .
    echo "✅ Installation complete"
fi

echo "🚀 Ready to explore the blockchain!"
