#!/bin/bash
set -e

echo "========================================="
echo "Setting up build environment and deploying P2P fixes"
echo "========================================="
echo ""

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
SERVER1="172.93.160.131"
SERVER2="173.249.195.59"
REPO_URL="https://github.com/Trucker2827/Dinero-Coin.git"
BRANCH="feat/sqlite-raii"

setup_and_deploy() {
    local SERVER=$1
    local NAME=$2

    echo "📡 Setting up $NAME ($SERVER)..."
    echo ""

    # Step 1: Install build dependencies
    echo "  1️⃣  Installing build dependencies..."
    ssh -i "$SSH_KEY" root@$SERVER "apt-get update && apt-get install -y \
        build-essential \
        cmake \
        git \
        libssl-dev \
        libboost-all-dev \
        pkg-config \
        python3 \
        2>&1 | tail -20"

    # Step 2: Check if git repo exists, if not clone
    echo "  2️⃣  Setting up git repository..."
    ssh -i "$SSH_KEY" root@$SERVER "
        cd /opt/dinero
        if [ ! -d .git ]; then
            echo 'Initializing git repository...'
            git init
            git remote add origin $REPO_URL || git remote set-url origin $REPO_URL
            git fetch origin
            git checkout -b $BRANCH origin/$BRANCH || git checkout $BRANCH
        else
            echo 'Git repo exists, pulling latest...'
            git fetch origin
            git checkout $BRANCH
            git pull origin $BRANCH
        fi
    " 2>&1 | tail -20

    # Step 3: Build
    echo "  3️⃣  Building daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "
        cd /opt/dinero
        rm -rf build
        mkdir build
        cd build
        cmake .. -DCMAKE_BUILD_TYPE=Release
        make -j\$(nproc)
    " 2>&1 | tail -30

    # Step 4: Stop daemon
    echo "  4️⃣  Stopping daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "systemctl stop dinerod || true"

    # Step 5: Install new binary
    echo "  5️⃣  Installing new binary..."
    ssh -i "$SSH_KEY" root@$SERVER "
        cp /opt/dinero/build/bin/dinerod /opt/dinero/dinerod
        chmod +x /opt/dinero/dinerod
    "

    # Step 6: Restart daemon
    echo "  6️⃣  Restarting daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "systemctl start dinerod"

    # Step 7: Wait and verify
    echo "  ⏳  Waiting 10 seconds for startup..."
    sleep 10

    echo "  7️⃣  Verifying P2P connections..."
    ssh -i "$SSH_KEY" root@$SERVER "dinero-cli getpeerinfo | head -40"

    echo ""
    echo "✅ $NAME deployment complete!"
    echo ""
    echo "========================================="
    echo ""
}

# Note: This script requires a GitHub repository with the latest code
echo "⚠️  IMPORTANT: Update REPO_URL in this script with your GitHub repository"
echo ""
read -p "Have you pushed the latest commits to GitHub? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Please push commits first:"
    echo "  git push origin $BRANCH"
    exit 1
fi

# Deploy to Server 1
setup_and_deploy "$SERVER1" "Server 1"

# Deploy to Server 2
setup_and_deploy "$SERVER2" "Server 2"

echo "🎉 All deployments complete!"
echo ""
echo "Both servers now have:"
echo "  ✅ Build dependencies installed"
echo "  ✅ Git repository configured"
echo "  ✅ Latest code with P2P fixes"
echo "  ✅ Freshly compiled binaries"
echo "  ✅ Daemons running"
echo ""
