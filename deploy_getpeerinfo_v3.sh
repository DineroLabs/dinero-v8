#!/bin/bash
set -e

echo "========================================="
echo "Deploying getpeerinfo RPC fix to Linux servers"
echo "File copy + full rebuild"
echo "========================================="
echo ""

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
SERVER1="172.93.160.131"
SERVER2="173.249.195.59"
FILE="src/rpc/methods_network_context.cpp"

deploy_to_server() {
    local SERVER=$1
    local NAME=$2

    echo "📡 Deploying to $NAME ($SERVER)..."
    echo ""

    # Copy the fixed file
    echo "  1️⃣  Copying fixed RPC file..."
    scp -i "$SSH_KEY" "$FILE" "root@$SERVER:/opt/dinero/$FILE"

    # Stop daemon first
    echo "  2️⃣  Stopping daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "systemctl stop dinerod || true"

    # Rebuild using linux_build.sh
    echo "  3️⃣  Rebuilding daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "cd /opt/dinero && rm -rf build && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc) 2>&1 | tail -30"

    # Copy binary
    echo "  4️⃣  Installing new binary..."
    ssh -i "$SSH_KEY" root@$SERVER "cp /opt/dinero/build/bin/dinerod /opt/dinero/dinerod && chmod +x /opt/dinero/dinerod"

    # Restart
    echo "  5️⃣  Restarting daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "systemctl start dinerod"

    # Wait for startup
    echo "  ⏳  Waiting 10 seconds for daemon to start..."
    sleep 10

    # Verify
    echo "  6️⃣  Testing getpeerinfo RPC..."
    ssh -i "$SSH_KEY" root@$SERVER "dinero-cli getpeerinfo | python3 -m json.tool 2>/dev/null | head -40 || dinero-cli getpeerinfo | head -40"

    echo ""
    echo "✅ $NAME deployment complete!"
    echo ""
    echo "========================================="
    echo ""
}

# Deploy to Server 1
deploy_to_server "$SERVER1" "Server 1"

# Deploy to Server 2
deploy_to_server "$SERVER2" "Server 2"

echo "🎉 All deployments complete!"
echo ""
echo "Verification:"
echo "  Server 1: ssh -i $SSH_KEY root@$SERVER1 'dinero-cli getpeerinfo'"
echo "  Server 2: ssh -i $SSH_KEY root@$SERVER2 'dinero-cli getpeerinfo'"
echo ""
