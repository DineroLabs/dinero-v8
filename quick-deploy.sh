#!/usr/bin/env bash
# Quick Deployment - Just sync source files, no build
# Use this when you only changed a few files and want to rebuild manually

set -e

CA_SERVER="172.93.160.131"
VA_SERVER="173.249.195.59"
SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
REMOTE_PATH="/root/DineroCoin"

echo "🚀 Quick sync (no build)..."

# Function to sync to a server
sync_to_server() {
    local SERVER=$1
    local NAME=$2

    echo "  → Syncing to ${NAME}..."
    rsync -avz --delete \
        --exclude='build*/' \
        --exclude='.git/' \
        --exclude='data/' \
        -e "ssh -i $SSH_KEY" \
        src/ include/ tools/ CMakeLists.txt \
        root@${SERVER}:${REMOTE_PATH}/
}

# Sync to both servers in parallel
sync_to_server "$CA_SERVER" "California" &
CA_PID=$!

sync_to_server "$VA_SERVER" "Virginia" &
VA_PID=$!

# Wait for both
wait $CA_PID
wait $VA_PID

echo "✅ Files synced to both servers"
echo ""
echo "To rebuild on servers, run:"
echo "  ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 'cd /root/DineroCoin/build-linux && make -j\$(nproc)'"
echo "  ssh -i ~/.ssh/dinero_deployment_2025 root@173.249.195.59 'cd /root/DineroCoin/build-linux && make -j\$(nproc)'"
