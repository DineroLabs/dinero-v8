#!/usr/bin/env bash
# Deploy dinerodlinux to all 3 servers with new genesis

set -e

SERVERS=(
    "96.9.226.98:DineroCA:$HOME/.ssh/dinero_key"
    "173.249.195.59:DineroVA:$HOME/.ssh/server2.key"
    "172.93.160.131:DineroLA:$HOME/.ssh/dinerola.key"
)

BINARY="build-linux/dinerodlinux"

echo "🚀 Deploying dinerodlinux with NEW genesis to all servers"
echo "=========================================================="
echo ""

for server_info in "${SERVERS[@]}"; do
    IP=$(echo "$server_info" | cut -d':' -f1)
    NAME=$(echo "$server_info" | cut -d':' -f2)
    KEY=$(echo "$server_info" | cut -d':' -f3)
    
    echo "=== $NAME ($IP) ==="
    
    # Stop old daemon
    ssh -i "$KEY" root@$IP "pkill -9 dinerod 2>/dev/null || true"
    echo "✅ Stopped old daemon"
    
    # Upload binary
    scp -i "$KEY" "$BINARY" root@$IP:/root/dinero/dinerodlinux
    echo "✅ Uploaded dinerodlinux"
    
    # Start new daemon
    ssh -i "$KEY" root@$IP "cd /root/dinero && chmod +x dinerodlinux && rm -rf data && nohup ./dinerodlinux -datadir=./data > daemon.log 2>&1 &"
    echo "✅ Started daemon"
    
    # Wait and verify
    sleep 5
    RUNNING=$(ssh -i "$KEY" root@$IP "ps aux | grep dinerodlinux | grep -v grep" || echo "")
    if [ -n "$RUNNING" ]; then
        echo "✅ $NAME running!"
    else
        echo "⚠️  $NAME may not be running"
    fi
    
    echo ""
done

echo "=========================================================="
echo "✅ Deployment complete!"
echo ""
echo "Verifying genesis on all servers..."
echo ""

for server_info in "${SERVERS[@]}"; do
    IP=$(echo "$server_info" | cut -d':' -f1)
    NAME=$(echo "$server_info" | cut -d':' -f2)
    KEY=$(echo "$server_info" | cut -d':' -f3)
    
    echo "=== $NAME ($IP) ==="
    COOKIE=$(ssh -i "$KEY" root@$IP "cat /root/dinero/data/.cookie 2>/dev/null | cut -d':' -f2" || echo "")
    
    if [ -n "$COOKIE" ]; then
        HASH=$(ssh -i "$KEY" root@$IP "curl -s --user '__cookie__:$COOKIE' --data-binary '{\"method\":\"getblock\",\"params\":[0]}' http://127.0.0.1:20998/ 2>/dev/null" | grep -o '"hash":"[^"]*"' | cut -d'"' -f4 || echo "")
        
        if [ "$HASH" = "00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e" ]; then
            echo "✅ Genesis verified: $HASH"
        else
            echo "⚠️  Genesis mismatch: $HASH"
        fi
    else
        echo "⏳ Daemon still starting..."
    fi
    echo ""
done

echo "=========================================================="
echo "🎉 Hard fork deployment complete!"
echo ""
echo "New Genesis:"
echo "  Hash: 00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e"
echo "  Difficulty: 0x1d3fffff (~300 days Phase 1)"
echo "  Coin Type: 1447 (Dinero)"
echo ""
echo "Premine:"
echo "  Hash: 0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b"
echo "  Address: din1qfmy8slqyt9zasexg7e849x9q08hr7da4d4hjmc"
echo "  Amount: 1M DIN"
echo ""
