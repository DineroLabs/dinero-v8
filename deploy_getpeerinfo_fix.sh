#!/bin/bash
set -e

echo "========================================="
echo "Deploying getpeerinfo RPC fix to Linux servers"
echo "========================================="
echo ""

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
SERVER1="172.93.160.131"
SERVER2="173.249.195.59"

deploy_to_server() {
    local SERVER=$1
    local NAME=$2

    echo "📡 Deploying to $NAME ($SERVER)..."
    echo ""

    # Pull latest code
    echo "  1️⃣  Pulling latest code..."
    ssh -i "$SSH_KEY" root@$SERVER "cd /opt/dinero && git pull origin feat/sqlite-raii"

    # Build
    echo "  2️⃣  Building daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "cd /opt/dinero && ./linux_build.sh 2>&1 | tail -20"

    # Restart
    echo "  3️⃣  Restarting daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "systemctl restart dinerod"

    # Wait for startup
    echo "  ⏳  Waiting 10 seconds for daemon to start..."
    sleep 10

    # Verify
    echo "  4️⃣  Testing getpeerinfo RPC..."
    ssh -i "$SSH_KEY" root@$SERVER "dinero-cli getpeerinfo | head -30"

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
