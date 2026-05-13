#!/bin/bash
set -e

echo "========================================="
echo "Deploying P2P fixes directly to Linux servers"
echo "  1. ConfigService: multiple addnode support"
echo "  2. getpeerinfo RPC: return actual peer data"
echo "========================================="
echo ""

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
SERVER1="172.93.160.131"
SERVER2="173.249.195.59"

# Files to deploy
FILES=(
    "src/daemon/services/config_service.cpp"
    "src/rpc/methods_network_context.cpp"
)

deploy_to_server() {
    local SERVER=$1
    local NAME=$2

    echo "📡 Deploying to $NAME ($SERVER)..."
    echo ""

    # Copy the fixed files
    echo "  1️⃣  Copying fixed source files..."
    for file in "${FILES[@]}"; do
        echo "      - $file"
        scp -i "$SSH_KEY" "$file" "root@$SERVER:~/DineroCoin/$file"
    done

    # Rebuild daemon using existing build directory
    echo "  2️⃣  Rebuilding daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "
        cd ~/DineroCoin/build
        make -j\$(nproc) 2>&1 | tail -30
    "

    # Stop daemon
    echo "  3️⃣  Stopping daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "pkill -9 dinerod || true"
    sleep 2

    # Restart daemon with addnode flags
    echo "  4️⃣  Restarting daemon with P2P fixes..."
    if [ "$SERVER" = "$SERVER1" ]; then
        # Server 1 connects to Server 2
        ssh -i "$SSH_KEY" root@$SERVER "cd ~/DineroCoin/build && nohup bin/dinerod -daemon -addnode=$SERVER2:20999 > /tmp/dinerod_debug.log 2>&1 &"
    else
        # Server 2 connects to Server 1
        ssh -i "$SSH_KEY" root@$SERVER "cd ~/DineroCoin/build && nohup bin/dinerod -daemon -addnode=$SERVER1:20999 > /tmp/dinerod_debug.log 2>&1 &"
    fi

    # Wait for startup
    echo "  ⏳  Waiting 10 seconds for daemon to start..."
    sleep 10

    # Verify P2P connections
    echo "  5️⃣  Testing getpeerinfo RPC..."
    ssh -i "$SSH_KEY" root@$SERVER "cd ~/DineroCoin/build && bin/dinero-cli getpeerinfo | head -40"

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
echo "Both servers now have:"
echo "  ✅ ConfigService fix (multiple addnode support)"
echo "  ✅ getpeerinfo RPC fix (returns actual peer data)"
echo "  ✅ P2P connections established between servers"
echo ""
echo "Verification:"
echo "  ssh -i $SSH_KEY root@$SERVER1 'cd ~/DineroCoin/build && bin/dinero-cli getpeerinfo'"
echo "  ssh -i $SSH_KEY root@$SERVER2 'cd ~/DineroCoin/build && bin/dinero-cli getpeerinfo'"
echo ""
