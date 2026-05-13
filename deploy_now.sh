#!/bin/bash
# Deploy Linux binary to all 3 servers

set -e

echo "🚀 Deploy Linux Binary to Servers"
echo "=================================="
echo ""

# Verify binary exists
if [ ! -f "build-linux/dinerod" ]; then
    echo "❌ Linux binary not found at build-linux/dinerod"
    echo "   Run ./build_docker_linux.sh first"
    exit 1
fi

echo "✅ Linux binary found: build-linux/dinerod"
file build-linux/dinerod
ls -lh build-linux/dinerod
echo ""

SERVERS="DineroCA:206.188.199.122:~/.ssh/dinero_key DineroVA:206.188.199.123:~/.ssh/dinero_va.key DineroLA:206.188.199.131:~/.ssh/dinerola.key"

SUCCESS_COUNT=0
FAIL_COUNT=0

for server_info in $SERVERS; do
    NAME=$(echo "$server_info" | cut -d':' -f1)
    IP=$(echo "$server_info" | cut -d':' -f2)
    KEY=$(echo "$server_info" | cut -d':' -f3)
    
    echo "📡 Deploying to $NAME ($IP)..."
    echo "=========================================="
    
    # Test SSH
    if ! ssh -i "$KEY" -o ConnectTimeout=5 -o StrictHostKeyChecking=no "root@$IP" "echo 'Connected'" > /dev/null 2>&1; then
        echo "  ❌ Cannot connect to $IP"
        ((FAIL_COUNT++))
        continue
    fi
    
    echo "  ✅ SSH connected"
    
    # Stop old daemon
    echo "  ⏹️  Stopping old daemon..."
    ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" "pkill -9 dinerod || true" 2>/dev/null
    sleep 2
    
    # Backup old binary
    echo "  💾 Backing up old binary..."
    ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" \
        "if [ -f /root/dinerod ]; then mv /root/dinerod /root/dinerod.backup.\$(date +%s); fi" 2>/dev/null
    
    # Upload new binary
    echo "  📤 Uploading new binary..."
    scp -i "$KEY" -o StrictHostKeyChecking=no build-linux/dinerod "root@$IP:/root/" > /dev/null 2>&1
    
    # Set permissions and verify
    echo "  🔧 Setting permissions..."
    ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" "chmod +x /root/dinerod && /root/dinerod --version"
    
    # Start daemon
    echo "  🚀 Starting daemon..."
    ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" \
        "nohup /root/dinerod -datadir=/root/dinero_data -rpcport=20998 -port=20999 > /root/daemon.log 2>&1 &" 2>/dev/null
    
    sleep 3
    
    # Verify running
    if ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" "pgrep -f dinerod" > /dev/null 2>&1; then
        PID=$(ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" "pgrep -f dinerod")
        echo "  ✅✅✅ Daemon started! PID: $PID"
        ((SUCCESS_COUNT++))
    else
        echo "  ❌ Daemon failed to start"
        echo "  📋 Last 10 lines of log:"
        ssh -i "$KEY" -o StrictHostKeyChecking=no "root@$IP" "tail -10 /root/daemon.log" 2>/dev/null || echo "  (log not available)"
        ((FAIL_COUNT++))
    fi
    echo ""
done

echo "============================================="
echo "📊 Deployment Summary"
echo "============================================="
echo "  ✅ Successful: $SUCCESS_COUNT servers"
echo "  ❌ Failed:     $FAIL_COUNT servers"
echo ""

if [ $SUCCESS_COUNT -gt 0 ]; then
    echo "✅ Deployment successful on $SUCCESS_COUNT server(s)!"
    echo ""
    echo "🔍 Monitor logs:"
    for server_info in $SERVERS; do
        IP=$(echo "$server_info" | cut -d':' -f2)
        KEY=$(echo "$server_info" | cut -d':' -f3)
        NAME=$(echo "$server_info" | cut -d':' -f1)
        if ssh -i "$KEY" -o ConnectTimeout=3 "root@$IP" "pgrep -f dinerod" > /dev/null 2>&1; then
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

