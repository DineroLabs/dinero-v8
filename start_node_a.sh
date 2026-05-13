#!/bin/bash

echo "🚀 **STARTING NODE A (P2P HUB)**"
echo "==============================="

# Kill any existing daemon
pkill -f dinerod 2>/dev/null
sleep 1

# Start Node A as P2P hub
./build/dinerod \
  -regtest \
  -datadir="$HOME/dinero/nodes/A" \
  -p2p \
  -port=20333 \
  -rpcport=20999 \
  -gen \
  -genthreads=8 \
  -miningaddress=din1qy9w9lg8akyrg2dnvfvt2ezgfaa6h2k8nhs88l5 \
  -daemon

sleep 3

echo ""
echo "Node A Status:"
pgrep -fl dinerod
echo ""
curl -s http://127.0.0.1:20999/healthz
echo ""
echo ""
echo "🎯 **FOR THE OTHER MAC:**"
echo "========================"
echo ""
echo "cd ~/Desktop/NODE_B"
echo ""
echo "./dinerod -regtest -datadir=regtestB -p2p -port=21333 -rpcport=21999 -addnode=192.168.1.87:20333 -gen -genthreads=6 -miningaddress=din1q58u9gl8pxs69w63tfghl6qwwggv4d70w0f004j"
echo ""
echo "✅ Both nodes will use DIN addresses and mine together!"
