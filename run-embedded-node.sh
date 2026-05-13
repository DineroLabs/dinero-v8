#!/bin/bash

# Run Dinero Embedded Node (Server Mode)
echo "🖥️  Dinero Embedded Node - Server Mode"
echo "======================================"

# Set OpenSSL environment for the embedded app
export OPENSSL_MODULES="$(pwd)/build-qt/bin/dinero-embedded-qt6.app/Contents/Resources/ossl-modules"
export QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1

# Get this Mac's IP
MAC_IP=$(ifconfig | grep "inet " | grep -v 127.0.0.1 | awk '{print $2}' | head -1)
echo "🌐 This Mac's IP: $MAC_IP"
echo "🔗 P2P Network: $MAC_IP:22999 (for other nodes to connect)"
echo "💡 GUI uses in-process API calls (no RPC server needed)"
echo "🚀 Starting embedded node in server mode..."
echo ""

# Start the embedded Qt app with built-in node
# No RPC server needed - GUI communicates directly with embedded node
exec ./build-qt/bin/dinero-embedded-qt6.app/Contents/MacOS/dinero-embedded-qt6 \
    -regtest \
    -datadir=./data \
    -port=22999 \
    -listen=1 \
    -maxconnections=125 \
    -debug=1
