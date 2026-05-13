#!/usr/bin/env bash
set -e

SERVERS=("96.9.226.98:DineroCA:$HOME/.ssh/dinero_key" "173.249.195.59:DineroVA:$HOME/.ssh/server2.key" "172.93.160.131:DineroLA:$HOME/.ssh/dinerola.key")

for server in "${SERVERS[@]}"; do
    IFS=':' read -r IP NAME KEY <<< "$server"
    KEY=$(eval echo $KEY)
    
    echo "🚀 Deploying to $NAME ($IP)..."
    
    # Upload
    scp -i "$KEY" -o StrictHostKeyChecking=no build-linux/dinerodlinux root@$IP:/root/dinero/dinerodlinux.new
    
    # Install and start
    ssh -i "$KEY" -o StrictHostKeyChecking=no root@$IP << 'REMOTE'
        cd /root/dinero
        pkill dinerod || true
        pkill dinerodlinux || true
        sleep 2
        rm -rf data
        mkdir -p data
        rm -f dinerodlinux
        mv dinerodlinux.new dinerodlinux
        chmod +x dinerodlinux
        nohup ./dinerodlinux -datadir=/root/dinero/data -rpcport=20998 -port=20999 > daemon.log 2>&1 &
        sleep 3
        echo "✅ Started!"
        tail -15 daemon.log | grep -E "Genesis|Premine|RPC|listening|height" || tail -15 daemon.log
REMOTE
    
    echo "✅ $NAME deployed!"
    echo ""
done

echo "🎉 All servers deployed with new genesis!"
