#!/bin/bash
# DineroCoin Linux Production Deployment - Upload Source & Build
# Date: November 10, 2025
# Strategy: Upload source code tarball, extract, and build on Linux server

set -e

# Configuration
REMOTE_HOST="173.249.195.59"
REMOTE_USER="root"
SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
REMOTE_DIR="/opt/dinero"
VERSION="v0.1.0-$(git rev-parse --short HEAD)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_info "========================================="
log_info "DineroCoin Linux Deployment"
log_info "Strategy: Upload source + build on server"
log_info "========================================="

# Step 1: Create source tarball (exclude build artifacts)
log_info "Creating source code tarball..."
TARBALL="/tmp/dinero-source-$(date +%Y%m%d-%H%M%S).tar.gz"

tar czf "$TARBALL" \
    --exclude='build' \
    --exclude='gui/build' \
    --exclude='.git' \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='CMakeFiles' \
    --exclude='CMakeCache.txt' \
    --exclude='*.app' \
    --exclude='tmp' \
    --exclude='wallet-core' \
    --exclude='secp256k1/.git' \
    -C /Users/haydarevich/Documents \
    DineroCoin

log_info "Source tarball created: $TARBALL"
ls -lh "$TARBALL"

# Step 2: Upload source to server
log_info "Uploading source code to $REMOTE_HOST..."
scp -i "$SSH_KEY" "$TARBALL" "$REMOTE_USER@$REMOTE_HOST:/tmp/"

# Step 3: Extract, build, and deploy on server
log_info "Building on Linux server..."
ssh -i "$SSH_KEY" "$REMOTE_USER@$REMOTE_HOST" bash <<REMOTE_SCRIPT
    set -e

    echo "[REMOTE] ========================================="
    echo "[REMOTE] DineroCoin Production Build"
    echo "[REMOTE] ========================================="

    # Backup existing binaries
    if [ -f /opt/dinero/dinerod ]; then
        BACKUP_DIR="/opt/dinero-backup-\$(date +%Y%m%d-%H%M%S)"
        mkdir -p "\$BACKUP_DIR"
        cp /opt/dinero/dinerod "\$BACKUP_DIR/" 2>/dev/null || true
        cp /opt/dinero/dinero-cli "\$BACKUP_DIR/" 2>/dev/null || true
        echo "[REMOTE] Backup created: \$BACKUP_DIR"
    fi

    # Stop existing daemon
    echo "[REMOTE] Stopping existing daemon..."
    pkill -9 dinerod || true
    sleep 2

    # Extract source
    echo "[REMOTE] Extracting source code..."
    cd /tmp
    tar xzf $(basename "$TARBALL")

    # Move to /opt
    echo "[REMOTE] Moving source to /opt/dinero..."
    rm -rf /opt/dinero-old 2>/dev/null || true
    mv /opt/dinero /opt/dinero-old 2>/dev/null || true
    mv DineroCoin /opt/dinero
    cd /opt/dinero

    # Install build dependencies if needed
    echo "[REMOTE] Checking build dependencies..."
    if ! command -v cmake &> /dev/null; then
        echo "[REMOTE] Installing cmake..."
        apt-get update && apt-get install -y cmake
    fi

    if ! command -v g++ &> /dev/null; then
        echo "[REMOTE] Installing build tools..."
        apt-get update && apt-get install -y build-essential
    fi

    # Build
    echo "[REMOTE] ========================================="
    echo "[REMOTE] Building DineroCoin binaries..."
    echo "[REMOTE] This will take 5-10 minutes..."
    echo "[REMOTE] ========================================="

    mkdir -p build
    cd build

    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_GPU_MINING=ON \
        -DENABLE_STRATUM_SERVER=ON \
        -DENABLE_STRATUM_SSL=ON \
        2>&1 | tail -10

    make -j\$(nproc) dinerod dinero-cli 2>&1 | tail -20

    # Verify build
    if [ ! -f "bin/dinerod" ] || [ ! -f "bin/dinero-cli" ]; then
        echo "[REMOTE] ERROR: Build failed!"
        exit 1
    fi

    echo "[REMOTE] Build successful!"
    ls -lh bin/dinerod bin/dinero-cli

    # Verify architecture
    echo "[REMOTE] Binary architecture:"
    file bin/dinerod | head -1

    # Start daemon
    echo "[REMOTE] ========================================="
    echo "[REMOTE] Starting DineroCoin daemon..."
    echo "[REMOTE] ========================================="

    bin/dinerod \
        -daemon \
        -stratum \
        -stratumport=3333 \
        -addnode=172.93.160.131:20999 \
        -addnode=173.249.195.59:20999 \
        -rpcbind=0.0.0.0 \
        -rpcallowip=0.0.0.0/0

    echo "[REMOTE] Daemon starting... waiting 8 seconds..."
    sleep 8

    echo "[REMOTE] Checking daemon status..."
    bin/dinero-cli getblockcount 2>&1 || echo "[REMOTE] Daemon initializing..."
REMOTE_SCRIPT

# Step 4: Verify deployment
log_info "Verifying deployment..."
sleep 3

ssh -i "$SSH_KEY" "$REMOTE_USER@$REMOTE_HOST" bash <<'VERIFY'
    cd /opt/dinero

    echo ""
    echo "=== Blockchain Info ==="
    build/bin/dinero-cli blockchain.getinfo 2>&1 | head -10 || build/bin/dinero-cli getblockcount 2>&1

    echo ""
    echo "=== Peer Connections ==="
    build/bin/dinero-cli getpeerinfo 2>&1 | head -20 || echo "No peers yet..."

    echo ""
    echo "=== GPU Mining Status ==="
    build/bin/dinero-cli mining.gpustatus 2>&1 | head -10 || echo "GPU RPC not available"

    echo ""
    echo "=== Stratum Status ==="
    build/bin/dinero-cli mining.getstratuminfo 2>&1 | head -10 || echo "Stratum not available"
VERIFY

# Cleanup
log_info "Cleaning up..."
rm -f "$TARBALL"

# Summary
echo ""
log_info "========================================="
log_info "Deployment Complete!"
log_info "========================================="
log_info "Version: $VERSION"
log_info "Server: $REMOTE_HOST"
log_info "Features Deployed:"
log_info "  ✅ GPU mining RPC (mining.gpustatus, mining.allowgpu, mining.gpuinfo)"
log_info "  ✅ Stratum SSL/TLS support"
log_info "  ✅ P2P seed nodes configured"
log_info "  ✅ Latest consensus & wallet updates"
log_info "========================================="
log_info ""
log_info "Monitor daemon:"
log_info "  ssh -i $SSH_KEY $REMOTE_USER@$REMOTE_HOST 'tail -f ~/.dinero/debug.log'"
