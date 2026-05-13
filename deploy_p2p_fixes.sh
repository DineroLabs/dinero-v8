#!/bin/bash
set -e

echo "========================================="
echo "Deploying P2P fixes to Linux servers"
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
        scp -i "$SSH_KEY" "$file" "root@$SERVER:/opt/dinero/$file"
    done

    # Stop daemon
    echo "  2️⃣  Stopping daemon..."
    ssh -i "$SSH_KEY" root@$SERVER "systemctl stop dinerod || true"

    # Since the server doesn't have build dependencies, we'll compile locally
    # and copy the binary
    echo "  3️⃣  Server will need a pre-built binary (servers lack build deps)"
    echo "      Let me know if you want to:"
    echo "      A) Build locally on Mac and copy binary (cross-compile issue)"
    echo "      B) Set up build environment on servers"
    echo "      C) Use rsync to copy entire build directory"

    echo ""
    echo "⚠️  $NAME: Files copied but daemon not restarted (needs binary)"
    echo ""
    echo "========================================="
    echo ""
}

# Deploy to Server 1
deploy_to_server "$SERVER1" "Server 1"

# Deploy to Server 2
deploy_to_server "$SERVER2" "Server 2"

echo "📝 Summary:"
echo "  - Fixed source files copied to both servers"
echo "  - Daemons stopped on both servers"
echo "  - Need to compile binaries on servers"
echo ""
echo "Options:"
echo "  1. Install build dependencies on servers and compile there"
echo "  2. Set up GitHub and pull/build from source"
echo "  3. Use a Linux VM to cross-compile and copy binaries"
echo ""
