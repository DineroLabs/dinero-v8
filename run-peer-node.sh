#!/bin/bash

# Run Dinero Embedded Node (Peer Mode - Connects to Server)
echo "🖥️  Dinero Embedded Node - Peer Mode"
echo "===================================="

# Set OpenSSL environment for the embedded app
export OPENSSL_MODULES="$(pwd)/build-qt/bin/dinero-embedded-qt6.app/Contents/Resources/ossl-modules"
export QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1

# Server Mac's IP address (update this if needed)
SERVER_IP="192.168.1.179"
SERVER_PORT="22999"

echo "🌐 Server IP: $SERVER_IP:$SERVER_PORT"
echo "🔗 Connecting to server node for blockchain sync"
echo "💡 GUI uses in-process API calls (no RPC server needed)"
echo "🚀 Starting embedded node in peer mode..."
echo ""

# Start the embedded Qt app with built-in node
# Connects to server node via P2P protocol
exec ./build-qt/bin/dinero-embedded-qt6.app/Contents/MacOS/dinero-embedded-qt6 \
    -regtest \
    -datadir=./data \
    -port=22999 \
    -listen=1 \
    -maxconnections=125 \
    -addnode=$SERVER_IP:$SERVER_PORT \
    -debug=1
