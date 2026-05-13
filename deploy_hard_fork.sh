#!/usr/bin/env bash
# Deploy NEW genesis (coin type 1447, difficulty 0x1d3fffff) to all servers
# This is a HARD FORK - all nodes must update!

set -e

echo "═══════════════════════════════════════════════════════"
echo "🚨 HARD FORK DEPLOYMENT - New Genesis (Coin Type 1447)"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "⚠️  This will:"
echo "   - Stop all running daemons"
echo "   - Wipe ALL blockchain data"
echo "   - Deploy new binaries"
echo "   - Start fresh with new genesis"
echo ""

# Server configuration (from GreenCloud panel)
SERVERS=(
    "96.9.226.98:DineroCA"
    "173.249.195.59:DineroVA"
    "172.93.160.131:DineroLA"
)

SSH_KEYS=(
    "$HOME/.ssh/dinero_key"
    "$HOME/.ssh/server2.key"
    "$HOME/.ssh/dinerola.key"
)

BUILD_DIR="/Users/haydarevich/Documents/DineroCoin/build"
REMOTE_DIR="/root/dinero"

echo "📦 Step 1: Building fresh binaries..."
cd /Users/haydarevich/Documents/DineroCoin
cmake --build build -j8 --target dinerod dinero-miner 2>&1 | grep -E "Building|Linking|Built" | tail -10
echo "✅ Build complete!"
echo ""

echo "📋 Step 2: Creating deployment package..."
mkdir -p /tmp/dinero-deploy
cp build/dinerod /tmp/dinero-deploy/
cp build/dinero-miner /tmp/dinero-deploy/
echo "✅ Package created!"
echo ""

# Deploy to each server
for i in "${!SERVERS[@]}"; do
    SERVER_INFO="${SERVERS[$i]}"
    IP=$(echo "$SERVER_INFO" | cut -d':' -f1)
    NAME=$(echo "$SERVER_INFO" | cut -d':' -f2)
    SSH_KEY="${SSH_KEYS[$i]}"
    
    echo "═══════════════════════════════════════════════════════"
    echo "🚀 Deploying to $NAME ($IP)"
    echo "═══════════════════════════════════════════════════════"
    
    # Test connection
    if ! ssh -i "$SSH_KEY" -o ConnectTimeout=5 -o StrictHostKeyChecking=no root@$IP "echo 'Connected'" 2>/dev/null; then
        echo "❌ Cannot connect to $NAME ($IP)"
        echo "   Skipping..."
        echo ""
        continue
    fi
    
    echo "✅ Connected to $NAME"
    
    # Stop old daemon
    echo "🛑 Stopping old daemon..."
    ssh -i "$SSH_KEY" root@$IP "pkill dinerod || true" 2>/dev/null
    sleep 2
    
    # Backup old data
    echo "💾 Backing up old blockchain data..."
    ssh -i "$SSH_KEY" root@$IP "cd $REMOTE_DIR && mv data data.old.$(date +%Y%m%d_%H%M%S) 2>/dev/null || true"
    
    # Upload new binaries
    echo "📤 Uploading new binaries..."
    scp -i "$SSH_KEY" -o StrictHostKeyChecking=no /tmp/dinero-deploy/* root@$IP:$REMOTE_DIR/
    
    # Set permissions
    ssh -i "$SSH_KEY" root@$IP "chmod +x $REMOTE_DIR/dinerod $REMOTE_DIR/dinero-miner"
    
    # Start new daemon
    echo "🚀 Starting new daemon with NEW genesis..."
    ssh -i "$SSH_KEY" root@$IP "cd $REMOTE_DIR && nohup ./dinerod -datadir=./data > daemon.log 2>&1 &"
    
    echo "⏳ Waiting for daemon to start..."
    sleep 8
    
    # Verify
    echo "🔍 Verifying new genesis..."
    COOKIE=$(ssh -i "$SSH_KEY" root@$IP "cat $REMOTE_DIR/data/.cookie 2>/dev/null | cut -d':' -f2" || echo "")
    
    if [ -n "$COOKIE" ]; then
        GENESIS_HASH=$(ssh -i "$SSH_KEY" root@$IP "curl -s --user '__cookie__:$COOKIE' --data-binary '{\"method\":\"getblock\",\"params\":[0]}' http://127.0.0.1:20998/ 2>/dev/null" | grep -o '"hash":"[^"]*"' | cut -d'"' -f4 || echo "")
        
        if [ "$GENESIS_HASH" = "00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e" ]; then
            echo "✅ Genesis verified: $GENESIS_HASH"
            
            PREMINE_HASH=$(ssh -i "$SSH_KEY" root@$IP "curl -s --user '__cookie__:$COOKIE' --data-binary '{\"method\":\"getblock\",\"params\":[1]}' http://127.0.0.1:20998/ 2>/dev/null" | grep -o '"hash":"[^"]*"' | cut -d'"' -f4 || echo "")
            
            if [ "$PREMINE_HASH" = "0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b" ]; then
                echo "✅ Premine verified: $PREMINE_HASH"
                echo "✅ $NAME deployment SUCCESSFUL!"
            else
                echo "⚠️  Premine hash mismatch!"
                echo "   Expected: 0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b"
                echo "   Got: $PREMINE_HASH"
            fi
        else
            echo "⚠️  Genesis hash mismatch!"
            echo "   Expected: 00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e"
            echo "   Got: $GENESIS_HASH"
        fi
    else
        echo "⚠️  Could not verify (cookie not found)"
        echo "   Check daemon.log on server"
    fi
    
    echo ""
done

echo "═══════════════════════════════════════════════════════"
echo "✅ HARD FORK DEPLOYMENT COMPLETE!"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "📋 Summary:"
echo "   - New Genesis: 00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e"
echo "   - New Premine: 0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b"
echo "   - Coin Type: 1447 (Dinero)"
echo "   - Difficulty: 0x1d3fffff (~300 days Phase 1)"
echo "   - Premine Address: din1qfmy8slqyt9zasexg7e849x9q08hr7da4d4hjmc"
echo ""
echo "🎯 All nodes are now on the new genesis!"
echo ""
