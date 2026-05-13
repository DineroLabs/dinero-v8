#!/bin/bash

# Start DineroCoin Daemon for iOS App Development
# This script starts dinerod in dev mode with RPC enabled on network interface

cd /Users/haydarevich/Documents/DineroCoin

echo "🔄 Stopping any existing dinerod processes..."
pkill -f dinerod
sleep 2

echo "🚀 Starting dinerod in development mode..."
./build/dinerod \
  -datadir=/Users/haydarevich/Documents/DineroCoin/data \
  -rpcport=20998 \
  -rpcbind=0.0.0.0 \
  -dev \
  -daemon

sleep 3

echo ""
echo "✅ DineroCoin Daemon Started!"
echo ""
echo "📱 Connection Info:"
echo "   - Local:   http://127.0.0.1:20998"
echo "   - Network: http://192.168.1.87:20998"
echo ""
echo "🔓 Dev Mode: No authentication required"
echo ""
echo "📊 Test connection:"
echo '   curl -s -X POST http://192.168.1.87:20998 -H "Content-Type: application/json" -d '"'"'{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo","params":[]}'"'"
echo ""
echo "📱 Your iOS app can now connect!"
