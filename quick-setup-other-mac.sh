#!/bin/bash

# Quick Setup Script for Other Mac - Automatically handles OpenSSL providers
echo "🚀 Dinero Quick Setup for Other Mac"
echo "==================================="

# Check if OpenSSL 3.x is installed
if ! brew list openssl@3 >/dev/null 2>&1; then
    echo "❌ OpenSSL 3.x not found. Installing..."
    brew install openssl@3
    if [ $? -ne 0 ]; then
        echo "❌ Failed to install OpenSSL 3.x"
        exit 1
    fi
    echo "✅ OpenSSL 3.x installed successfully"
else
    echo "✅ OpenSSL 3.x already installed"
fi

# Auto-detect OpenSSL version
OPENSSL_VERSION=$(ls /opt/homebrew/Cellar/openssl@3/ | head -1)
if [ -z "$OPENSSL_VERSION" ]; then
    echo "❌ Could not detect OpenSSL version"
    exit 1
fi

echo "🔍 Detected OpenSSL version: $OPENSSL_VERSION"

# Check if the app bundle exists
if [ ! -d "bin/dinero-embedded-qt6.app" ]; then
    echo "❌ App bundle not found. Please run the build first."
    echo "   Expected path: bin/dinero-embedded-qt6.app"
    exit 1
fi

# Create ossl-modules directory
echo "📁 Creating ossl-modules directory..."
mkdir -p bin/dinero-embedded-qt6.app/Contents/Resources/ossl-modules

# Copy OpenSSL providers
echo "📋 Copying OpenSSL providers from version $OPENSSL_VERSION..."
cp /opt/homebrew/Cellar/openssl@3/$OPENSSL_VERSION/lib/ossl-modules/*.dylib \
   bin/dinero-embedded-qt6.app/Contents/Resources/ossl-modules/

if [ $? -eq 0 ]; then
    echo "✅ OpenSSL providers copied successfully"
    
    # List what was copied
    echo "📋 Copied providers:"
    ls -la bin/dinero-embedded-qt6.app/Contents/Resources/ossl-modules/
else
    echo "❌ Failed to copy OpenSSL providers"
    exit 1
fi

# Make scripts executable
echo "🔧 Making scripts executable..."
chmod +x run-peer-node.sh

echo ""
echo "🎉 Setup Complete! Now you can:"
echo "1. Run the peer node: ./run-peer-node.sh"
echo "2. Connect to server at: 192.168.1.179:22999"
echo "3. GUI will work with crypto operations functioning"
echo ""
echo "💡 The app will automatically:"
echo "   - Load OpenSSL providers from the bundled ossl-modules"
echo "   - Connect to the server node via P2P protocol"
echo "   - Sync blockchain data automatically"
echo "   - Use in-process API calls (no RPC authentication needed)"
