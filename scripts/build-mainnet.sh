#!/bin/bash
# Dinero Mainnet Production Build Script
# This script creates a production-ready mainnet build with all security features enabled

set -e

echo "🚀 Building Dinero for MAINNET production..."

# Ensure we're in the project root
cd "$(dirname "$0")/.."

# Clean previous builds
echo "🧹 Cleaning previous builds..."
rm -rf build-mainnet

# Configure for production
echo "⚙️  Configuring mainnet build..."
cmake -S . -B build-mainnet \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos" \
    -DENABLE_SANITIZERS=OFF \
    -DCMAKE_CXX_FLAGS="-DNDEBUG -O3 -march=native" \
    -DCMAKE_INSTALL_PREFIX="/usr/local"

# Build all components
echo "🔨 Building mainnet binaries..."
cmake --build build-mainnet -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) --target all

# Run critical tests
echo "🧪 Running production tests..."
./build-mainnet/bin/test_premine_creation
./build-mainnet/bin/test_dinero_mining
./build-mainnet/bin/test_crypto_vectors

# Verify premine configuration
echo "🔍 Verifying premine configuration..."
if grep -q "PREMINE_BYPASS_POW = false" src/consensus/chain_facts.hpp; then
    echo "✅ Real PoW enabled for premine"
else
    echo "❌ WARNING: PoW bypass still enabled!"
    exit 1
fi

if grep -q "0xde,0xad,0xbe,0xef" src/consensus/chain_facts.hpp; then
    echo "⚠️  WARNING: Using placeholder dev fund address!"
    echo "   Update DEV_FUND_P2WPKH in src/consensus/chain_facts.hpp"
else
    echo "✅ Custom dev fund address configured"
fi

echo ""
echo "🎉 Mainnet build complete!"
echo "📁 Binaries: build-mainnet/bin/"
echo "🔑 Next: Update dev fund address and import private key"
echo ""
echo "⚠️  SECURITY CHECKLIST:"
echo "   □ Replace DEV_FUND_P2WPKH with real address"
echo "   □ Store dev fund private key offline"
echo "   □ Test on testnet first"
echo "   □ Verify premine block hash is deterministic"
echo ""
