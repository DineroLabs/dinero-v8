#!/bin/bash
# Dinero Testnet Deployment Script
# Deploy PSBT-enabled daemon to both testnet servers

set -e

echo "═══════════════════════════════════════════════════════════════"
echo "🚀 DINERO TESTNET DEPLOYMENT"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Server configuration
SERVER1="207.154.239.13"
SERVER2="64.23.176.109"
SSH_KEY1="${HOME}/.ssh/server2.key"
SSH_KEY2="${HOME}/.ssh/server1.key"

# Check for SSH keys
if [ ! -f "$SSH_KEY1" ]; then
    echo "❌ Error: SSH key not found: $SSH_KEY1"
    echo ""
    echo "Please place your Server 1 SSH key at: $SSH_KEY1"
    echo "Then run: chmod 600 $SSH_KEY1"
    exit 1
fi

if [ ! -f "$SSH_KEY2" ]; then
    echo "❌ Error: SSH key not found: $SSH_KEY2"
    echo ""
    echo "Please place your Server 2 SSH key at: $SSH_KEY2"
    echo "Then run: chmod 600 $SSH_KEY2"
    exit 1
fi

echo "✅ SSH keys found"
echo ""

# Phase 1: Deploy to Server 1
echo "──────────────────────────────────────────────────────────────"
echo "📦 Phase 1: Deploying to Server 1 ($SERVER1)"
echo "──────────────────────────────────────────────────────────────"
echo ""

rsync -avz -e "ssh -i $SSH_KEY1 -o StrictHostKeyChecking=no" \
    --delete \
    --exclude 'build/' \
    --exclude 'build-asan/' \
    --exclude 'data/' \
    --exclude '.git/' \
    --exclude '*.log' \
    --exclude 'gui/' \
    src/ include/ CMakeLists.txt external/ \
    root@${SERVER1}:~/DineroCoin/

echo "✅ Code deployed to Server 1"
echo ""

# Phase 2: Deploy to Server 2
echo "──────────────────────────────────────────────────────────────"
echo "📦 Phase 2: Deploying to Server 2 ($SERVER2)"
echo "──────────────────────────────────────────────────────────────"
echo ""

rsync -avz -e "ssh -i $SSH_KEY2 -o StrictHostKeyChecking=no" \
    --delete \
    --exclude 'build/' \
    --exclude 'build-asan/' \
    --exclude 'data/' \
    --exclude '.git/' \
    --exclude '*.log' \
    --exclude 'gui/' \
    src/ include/ CMakeLists.txt external/ \
    root@${SERVER2}:~/DineroCoin/

echo "✅ Code deployed to Server 2"
echo ""

# Phase 3: Build on Server 1
echo "──────────────────────────────────────────────────────────────"
echo "🔨 Phase 3: Building on Server 1"
echo "──────────────────────────────────────────────────────────────"
echo ""

ssh -i $SSH_KEY1 -o StrictHostKeyChecking=no root@${SERVER1} << 'ENDSSH'
cd ~/DineroCoin
rm -rf build
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc) dinerod
ENDSSH

echo "✅ Build complete on Server 1"
echo ""

# Phase 4: Build on Server 2
echo "──────────────────────────────────────────────────────────────"
echo "🔨 Phase 4: Building on Server 2"
echo "──────────────────────────────────────────────────────────────"
echo ""

ssh -i $SSH_KEY2 -o StrictHostKeyChecking=no root@${SERVER2} << 'ENDSSH'
cd ~/DineroCoin
rm -rf build
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc) dinerod
ENDSSH

echo "✅ Build complete on Server 2"
echo ""

# Phase 5: Stop any running daemons
echo "──────────────────────────────────────────────────────────────"
echo "🛑 Phase 5: Stopping old daemons"
echo "──────────────────────────────────────────────────────────────"
echo ""

ssh -i $SSH_KEY1 -o StrictHostKeyChecking=no root@${SERVER1} 'pkill -9 dinerod || true'
ssh -i $SSH_KEY2 -o StrictHostKeyChecking=no root@${SERVER2} 'pkill -9 dinerod || true'

sleep 2
echo "✅ Old daemons stopped"
echo ""

# Phase 6: Start Server 1 (Seed Node)
echo "──────────────────────────────────────────────────────────────"
echo "🚀 Phase 6: Starting Server 1 (Seed Node)"
echo "──────────────────────────────────────────────────────────────"
echo ""

ssh -i $SSH_KEY1 -o StrictHostKeyChecking=no root@${SERVER1} << 'ENDSSH'
cd ~/DineroCoin
mkdir -p ~/dinero-data
nohup ./build/dinerod \
    -datadir=/root/dinero-data \
    -testnet \
    -rpcport=20998 \
    -port=20999 \
    > ~/dinero-data/daemon.log 2>&1 &
sleep 3
echo "Daemon PID: $(pgrep dinerod)"
ENDSSH

echo "✅ Server 1 started"
echo ""

# Phase 7: Start Server 2 (Peer Node)
echo "──────────────────────────────────────────────────────────────"
echo "🚀 Phase 7: Starting Server 2 (Peer Node)"
echo "──────────────────────────────────────────────────────────────"
echo ""

ssh -i $SSH_KEY2 -o StrictHostKeyChecking=no root@${SERVER2} << 'ENDSSH'
cd ~/DineroCoin
mkdir -p ~/dinero-data
nohup ./build/dinerod \
    -datadir=/root/dinero-data \
    -testnet \
    -rpcport=20998 \
    -port=20999 \
    -addnode=207.154.239.13:20999 \
    > ~/dinero-data/daemon.log 2>&1 &
sleep 3
echo "Daemon PID: $(pgrep dinerod)"
ENDSSH

echo "✅ Server 2 started and connected to Server 1"
echo ""

# Phase 8: Verify connection
echo "──────────────────────────────────────────────────────────────"
echo "✅ Phase 8: Verifying Connection"
echo "──────────────────────────────────────────────────────────────"
echo ""

sleep 5

echo "Server 1 Status:"
ssh -i $SSH_KEY1 -o StrictHostKeyChecking=no root@${SERVER1} \
    'tail -30 ~/dinero-data/daemon.log | grep -E "Dinero|Started|peer|block"' || echo "Check logs manually"

echo ""
echo "Server 2 Status:"
ssh -i $SSH_KEY2 -o StrictHostKeyChecking=no root@${SERVER2} \
    'tail -30 ~/dinero-data/daemon.log | grep -E "Dinero|Started|peer|block"' || echo "Check logs manually"

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "✅ DEPLOYMENT COMPLETE"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "NEXT STEPS:"
echo "1. Test RPC: curl --user \$(cat ~/dinero-data/.cookie) -X POST http://207.154.239.13:20998 -d '{\"method\":\"getblockchaininfo\"}'"
echo "2. Check logs: ssh -i $SSH_KEY1 root@$SERVER1 'tail -f ~/dinero-data/daemon.log'"
echo "3. Test PSBT: dinero-cli walletcreatefundedpsbt '{\"din1q...\": 1.0}'"
echo ""

