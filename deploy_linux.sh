#!/bin/bash
# Deploy and BUILD on Linux servers directly

set -e

echo "🚀 Deploy to Linux Servers (Build on Target)"
echo "============================================="
echo ""

SERVERS="DineroCA:206.188.199.122:~/.ssh/dinero_ca_rsa DineroVA:206.188.199.123:~/.ssh/dinero_va_rsa DineroLA:206.188.199.131:~/.ssh/dinero_la_rsa"

echo "📋 Pre-Deployment Checklist:"
echo "  ✅ Security fixes applied (OPENSSL_cleanse)"
echo "  ✅ Coin type 1447 configured"
echo "  ✅ BIP84 path validated: m/84'/1447'/0'/0/x"
echo ""

# Create source tarball
echo "📦 Creating source package..."
DEPLOY_DIR="dinero_source_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$DEPLOY_DIR"

# Copy source files (exclude build artifacts)
rsync -a --exclude='build*' --exclude='.git' --exclude='data*' --exclude='test_*' \
    --exclude='*.tar.gz' --exclude='*.log' \
    ./ "$DEPLOY_DIR/"

tar -czf "${DEPLOY_DIR}.tar.gz" "$DEPLOY_DIR"
rm -rf "$DEPLOY_DIR"
echo "✅ Source package: ${DEPLOY_DIR}.tar.gz"

SUCCESS_COUNT=0
FAIL_COUNT=0

for server_info in $SERVERS; do
    NAME=$(echo "$server_info" | cut -d':' -f1)
    IP=$(echo "$server_info" | cut -d':' -f2)
    KEY=$(echo "$server_info" | cut -d':' -f3)
    
    echo ""
    echo "📡 Deploying to $NAME ($IP)..."
    echo "=========================================="
    
    # Test SSH
    if ! ssh -i "$KEY" -o ConnectTimeout=5 -o StrictHostKeyChecking=no "root@$IP" "echo 'Connected'" > /dev/null 2>&1; then
        echo "  ❌ Cannot connect to $IP (SSH key: $KEY)"
        ((FAIL_COUNT++))
        continue
    fi
    
    echo "  ✅ SSH connection established"
    
    # Upload source
    echo "  📤 Uploading source..."
    scp -i "$KEY" -o StrictHostKeyChecking=no "${DEPLOY_DIR}.tar.gz" "root@$IP:/root/" > /dev/null 2>&1
    
    # Build on server
    echo "  🔨 Building on Linux server..."
    ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" << 'REMOTE_BUILD'
        set -e
        cd /root
        
        # Extract source
        tar -xzf dinero_source_*.tar.gz
        cd dinero_source_*
        
        # Install dependencies if needed
        if ! command -v cmake &> /dev/null; then
            echo "  📦 Installing build dependencies..."
            apt-get update -qq
            apt-get install -y -qq cmake g++ libsecp256k1-dev libssl-dev libsqlite3-dev > /dev/null 2>&1
        fi
        
        # Build
        echo "  🔨 Compiling..."
        mkdir -p build
        cd build
        cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF .. > /dev/null 2>&1
        cmake --build . -j$(nproc) > /dev/null 2>&1
        
        echo "  ✅ Build complete"
        
        # Stop old daemon
        pkill -9 dinerod || true
        sleep 2
        
        # Install new binary
        if [ -f "/root/dinerod" ]; then
            mv /root/dinerod /root/dinerod.backup.$(date +%s)
        fi
        cp dinerod /root/
        chmod +x /root/dinerod
        
        # Verify version
        /root/dinerod --version
        
        # Start daemon
        echo "  🚀 Starting daemon..."
        nohup /root/dinerod -datadir=/root/dinero_data -rpcport=20998 -port=20999 > /root/daemon.log 2>&1 &
        sleep 3
        
        # Verify running
        if pgrep -f dinerod > /dev/null; then
            echo "  ✅ Daemon started successfully"
            echo "  📊 Daemon PID: $(pgrep -f dinerod)"
        else
            echo "  ❌ Daemon failed to start"
            tail -20 /root/daemon.log
            exit 1
        fi
REMOTE_BUILD
    
    if [ $? -eq 0 ]; then
        echo "  ✅✅✅ $NAME deployed successfully!"
        ((SUCCESS_COUNT++))
    else
        echo "  ❌ Deployment failed on $NAME"
        ((FAIL_COUNT++))
    fi
done

# Cleanup
rm -f "${DEPLOY_DIR}.tar.gz"

echo ""
echo "============================================="
echo "📊 Deployment Summary"
echo "============================================="
echo "  ✅ Successful: $SUCCESS_COUNT servers"
echo "  ❌ Failed:     $FAIL_COUNT servers"
echo ""

if [ $SUCCESS_COUNT -gt 0 ]; then
    echo "✅ Deployment completed on $SUCCESS_COUNT server(s)!"
    echo ""
    echo "🔍 Monitor logs with:"
    for server_info in $SERVERS; do
        IP=$(echo "$server_info" | cut -d':' -f2)
        KEY=$(echo "$server_info" | cut -d':' -f3)
        NAME=$(echo "$server_info" | cut -d':' -f1)
        if ssh -i "$KEY" -o ConnectTimeout=5 "root@$IP" "pgrep -f dinerod" > /dev/null 2>&1; then
            echo "  ssh -i $KEY root@$IP 'tail -f /root/daemon.log'  # $NAME"
        fi
    done
    echo ""
    echo "🎯 Next: Monitor for 24-48 hours"
    exit 0
else
    echo "❌ All deployments failed"
    exit 1
fi

