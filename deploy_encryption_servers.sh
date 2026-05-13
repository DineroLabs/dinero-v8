#!/bin/bash
# Full encryption deployment to Linux servers

SERVER=$1
KEY=$2
NAME=$3

echo "═══════════════════════════════════════════════════════════════"
echo "🚀 Deploying encryption update to $NAME ($SERVER)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

echo "📦 Step 1: Syncing source files..."
rsync -avz --progress \
  --exclude 'build/' --exclude 'data/' --exclude '.git/' \
  --exclude '*.log' --exclude 'nohup.out' \
  -e "ssh -i $KEY -o StrictHostKeyChecking=no" \
  src/daemon/main.cpp \
  src/wallet/hd_wallet.cpp \
  include/wallet/hd_wallet.h \
  root@$SERVER:~/DineroCoin/ 2>&1 | tail -10

echo ""
echo "🔨 Step 2: Rebuilding daemon..."
ssh -i $KEY -o StrictHostKeyChecking=no root@$SERVER << 'REMOTE'
  cd ~/DineroCoin/build
  echo "Cleaning old build artifacts..."
  rm -f CMakeFiles/dinerod.dir/src/daemon/main.cpp.o
  rm -f CMakeFiles/dinero_wallet.dir/src/wallet/hd_wallet.cpp.o
  rm -f dinerod
  
  echo ""
  echo "Building..."
  make -j$(nproc) 2>&1 | tail -20
  
  if [ -f dinerod ]; then
    echo ""
    echo "✅ Build successful"
    ls -lh dinerod
  else
    echo ""
    echo "❌ Build failed"
    exit 1
  fi
REMOTE

if [ $? -ne 0 ]; then
  echo ""
  echo "❌ Deployment failed on $NAME"
  exit 1
fi

echo ""
echo "🔄 Step 3: Restarting daemon..."
ssh -i $KEY -o StrictHostKeyChecking=no root@$SERVER << 'REMOTE'
  cd ~/DineroCoin/build
  
  echo "Stopping old daemon..."
  pkill -9 dinerod || true
  sleep 2
  
  echo "Starting new daemon with encryption support..."
  nohup ./dinerod --testnet --datadir=~/dinero-data --port=20999 --rpcport=20998 -dev > ~/dinerod.log 2>&1 &
  
  sleep 3
  
  echo ""
  echo "✅ Daemon status:"
  ps aux | grep dinerod | grep -v grep | head -1
  
  echo ""
  echo "Recent log:"
  tail -5 ~/dinerod.log
REMOTE

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "✅ Deployment to $NAME complete!"
echo "═══════════════════════════════════════════════════════════════"
echo ""

