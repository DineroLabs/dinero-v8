#!/bin/bash
# Start Dinero mining on Mac connected to server

set -e

echo "🔥 Starting Dinero Mining on Mac"
echo "=================================="
echo ""

# Copy RPC cookie from server
echo "Step 1: Getting RPC authentication from server..."
scp -i .server-key root@96.9.226.98:/var/lib/dinero/.cookie ./data-mining/.cookie
echo "✅ Cookie downloaded"
echo ""

# Start mining daemon
echo "Step 2: Starting mining daemon..."
echo "  - Connecting to: 96.9.226.98:20998"
echo "  - Mining threads: 4"
echo "  - Data dir: ./data-mining"
echo ""

./build/dinerod -conf=./mac-mining-config.conf

echo ""
echo "✅ Mining daemon starting..."
echo ""
echo "Check status with:"
echo "  ./build/dinero-cli -rpcconnect=96.9.226.98 -rpcport=20998 getmininginfo"
echo ""
