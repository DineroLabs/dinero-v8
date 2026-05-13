#!/bin/bash
# Clean test on servers

SERVER=$1
KEY=$2
NAME=$3

echo "═══════════════════════════════════════════════════════════════"
echo "🧹 Clean testing $NAME ($SERVER)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

ssh -i $KEY -o StrictHostKeyChecking=no root@$SERVER << 'REMOTE'
  echo "Step 1: Stopping daemon..."
  pkill -9 dinerod
  sleep 2
  
  echo "Step 2: Cleaning wallet data..."
  rm -rf ~/dinero-data/wallet
  rm -rf ./data/wallet
  
  echo "Step 3: Starting fresh daemon..."
  cd ~/DineroCoin/build
  nohup ./dinerod --testnet --datadir=~/dinero-data --port=20999 --rpcport=20998 -dev > ~/dinerod.log 2>&1 &
  
  sleep 4
  
  echo "Step 4: Daemon status..."
  ps aux | grep dinerod | grep -v grep | head -1
  
  echo ""
  echo "✅ Ready for testing"
REMOTE

echo ""

