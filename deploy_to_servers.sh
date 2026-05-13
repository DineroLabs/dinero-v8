#!/usr/bin/env bash
set -e

echo "🚀 Deploying dinerodlinux to all servers..."
echo ""

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

BINARY="build-linux/dinerodlinux"

if [ ! -f "$BINARY" ]; then
    echo "❌ Binary not found: $BINARY"
    exit 1
fi

for i in "${!SERVERS[@]}"; do
    IFS=':' read -r IP NAME <<< "${SERVERS[$i]}"
    KEY="${SSH_KEYS[$i]}"
    
    echo "📡 Deploying to $NAME ($IP)..."
    
    # Upload binary
    scp -i "$KEY" -o StrictHostKeyChecking=no "$BINARY" root@$IP:/root/dinero/dinerodlinux
    
    # Stop old daemon, start new one
    ssh -i "$KEY" -o StrictHostKeyChecking=no root@$IP << 'REMOTE'
        pkill dinerod || true
        sleep 2
        rm -rf /root/dinero/data
        mkdir -p /root/dinero/data
        cd /root/dinero
        chmod +x dinerodlinux
        nohup ./dinerodlinux -datadir=/root/dinero/data -rpcport=20998 -port=20999 > daemon.log 2>&1 &
        sleep 2
        echo "✅ Daemon started"
        tail -20 daemon.log | grep -E "Genesis|Premine|RPC|listening" || true
REMOTE
    
    echo "✅ $NAME deployed!"
    echo ""
done

echo "🎉 All servers deployed!"
