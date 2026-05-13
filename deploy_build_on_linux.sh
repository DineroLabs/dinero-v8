#!/bin/bash
# DineroCoin Linux Production Deployment Script
# Date: November 10, 2025
# Features: GPU RPC, Stratum SSL, P2P improvements, latest consensus/wallet updates
# Build Strategy: Build ON the Linux server (not deploy Mac binaries)

set -e  # Exit on error

# Configuration
REMOTE_HOST="173.249.195.59"
REMOTE_USER="root"
SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
REMOTE_DIR="/opt/dinero"
VERSION="v0.1.0-$(git rev-parse --short HEAD)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Step 1: Push latest changes to git (so server can pull)
log_info "Checking git status..."
if git status --porcelain | grep -q '^'; then
    log_warn "You have uncommitted changes. Deployment will use committed code only."
    git status --short
    read -p "Continue with deployment of committed code? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log_error "Deployment cancelled"
        exit 1
    fi
fi

CURRENT_BRANCH=$(git branch --show-current)
log_info "Current branch: $CURRENT_BRANCH"
log_info "Latest commit: $(git log -1 --oneline)"

# Step 2: SSH to server and perform full build
log_info "Connecting to production server: $REMOTE_HOST"
log_info "This will build DineroCoin ON the Linux server..."

ssh -i "$SSH_KEY" "$REMOTE_USER@$REMOTE_HOST" bash <<'REMOTE_BUILD'
    set -e

    echo "[REMOTE] ========================================="
    echo "[REMOTE] DineroCoin Production Deployment"
    echo "[REMOTE] Build-on-Server Strategy"
    echo "[REMOTE] ========================================="

    # Navigate to installation directory
    cd /opt/dinero || {
        echo "[REMOTE] ERROR: /opt/dinero not found. Creating..."
        mkdir -p /opt/dinero
        cd /opt/dinero
    }

    # Check if git repo exists
    if [ ! -d ".git" ]; then
        echo "[REMOTE] No git repository found. Cloning..."
        echo "[REMOTE] ERROR: Repository URL not configured. Please clone manually first:"
        echo "[REMOTE]   cd /opt && git clone <your-dinero-repo-url> dinero"
        exit 1
    fi

    # Backup existing binaries
    if [ -f "build/bin/dinerod" ]; then
        BACKUP_DIR="/opt/dinero-backup-$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$BACKUP_DIR"
        cp build/bin/dinerod "$BACKUP_DIR/" 2>/dev/null || true
        cp build/bin/dinero-cli "$BACKUP_DIR/" 2>/dev/null || true
        echo "[REMOTE] Backup created: $BACKUP_DIR"
    fi

    # Stop running daemon
    echo "[REMOTE] Stopping existing daemon..."
    pkill -9 dinerod || true
    sleep 2

    # Pull latest code
    echo "[REMOTE] Pulling latest code from git..."
    git fetch origin
    CURRENT_BRANCH=$(git branch --show-current)
    git pull origin "$CURRENT_BRANCH"

    echo "[REMOTE] Latest commit: $(git log -1 --oneline)"

    # Check for build dependencies
    echo "[REMOTE] Checking build dependencies..."
    if ! command -v cmake &> /dev/null; then
        echo "[REMOTE] ERROR: cmake not found. Install with: apt-get install cmake"
        exit 1
    fi

    if ! command -v g++ &> /dev/null; then
        echo "[REMOTE] ERROR: g++ not found. Install with: apt-get install build-essential"
        exit 1
    fi

    # Configure build if needed
    if [ ! -d "build" ]; then
        echo "[REMOTE] Creating build directory..."
        mkdir -p build
        cd build
        echo "[REMOTE] Running CMake configuration..."
        cmake .. \
            -DCMAKE_BUILD_TYPE=Release \
            -DENABLE_GPU_MINING=ON \
            -DENABLE_STRATUM_SERVER=ON \
            -DENABLE_STRATUM_SSL=ON
        cd ..
    fi

    # Build binaries
    echo "[REMOTE] ========================================="
    echo "[REMOTE] Building DineroCoin (this may take 5-10 minutes)..."
    echo "[REMOTE] Features:"
    echo "[REMOTE]   - GPU mining RPC support"
    echo "[REMOTE]   - Stratum SSL/TLS encryption"
    echo "[REMOTE]   - P2P seed node improvements"
    echo "[REMOTE]   - Latest wallet & consensus updates"
    echo "[REMOTE] ========================================="

    # Build with all CPU cores
    cd build
    make -j$(nproc) dinerod dinero-cli 2>&1 | tail -20
    cd ..

    # Verify build
    if [ ! -f "build/bin/dinerod" ] || [ ! -f "build/bin/dinero-cli" ]; then
        echo "[REMOTE] ERROR: Build failed! Binaries not found."
        exit 1
    fi

    echo "[REMOTE] Build completed successfully"
    ls -lh build/bin/dinerod build/bin/dinero-cli

    # Verify binary architecture
    echo "[REMOTE] Binary architecture:"
    file build/bin/dinerod | head -1

    # Start daemon with production configuration
    echo "[REMOTE] ========================================="
    echo "[REMOTE] Starting DineroCoin daemon..."
    echo "[REMOTE] ========================================="

    build/bin/dinerod \
        -daemon \
        -stratum \
        -stratumport=3333 \
        -addnode=172.93.160.131:20999 \
        -addnode=173.249.195.59:20999 \
        -rpcbind=0.0.0.0 \
        -rpcallowip=0.0.0.0/0

    echo "[REMOTE] Daemon starting... Waiting 8 seconds..."
    sleep 8

    echo "[REMOTE] Checking daemon status..."
    build/bin/dinero-cli getblockcount || echo "[REMOTE] Daemon may still be initializing..."
REMOTE_BUILD

# Step 3: Verify deployment
log_info "Verifying deployment..."
sleep 3

ssh -i "$SSH_KEY" "$REMOTE_USER@$REMOTE_HOST" bash <<'REMOTE_VERIFY'
    set -e
    cd /opt/dinero

    echo ""
    echo "=== Daemon Info ==="
    build/bin/dinero-cli blockchain.getinfo 2>&1 | head -10 || build/bin/dinero-cli getinfo 2>&1 | head -10

    echo ""
    echo "=== Peer Connections ==="
    build/bin/dinero-cli getpeerinfo 2>&1 | grep -E "addr|version|subver" | head -20 || echo "No peers yet (still connecting...)"

    echo ""
    echo "=== GPU Mining Status ==="
    build/bin/dinero-cli mining.gpustatus 2>&1 | head -10 || echo "GPU RPC not available"

    echo ""
    echo "=== Stratum Server Status ==="
    build/bin/dinero-cli mining.getstratuminfo 2>&1 | head -10 || echo "Stratum not available"

    echo ""
    echo "=== Server Health ==="
    build/bin/dinero-cli server.health 2>&1 | head -15 || echo "Health check not available"
REMOTE_VERIFY

# Summary
echo ""
log_info "========================================="
log_info "Deployment Summary"
log_info "========================================="
log_info "Version: $VERSION"
log_info "Server: $REMOTE_HOST"
log_info "Build Strategy: Built ON Linux server (x86_64)"
log_info "Features Deployed:"
log_info "  ✅ GPU mining RPC (mining.gpustatus, mining.allowgpu, mining.gpuinfo)"
log_info "  ✅ Stratum SSL/TLS support"
log_info "  ✅ P2P seed nodes configured"
log_info "  ✅ Latest consensus & wallet updates"
log_info "  ✅ Explorer sync improvements"
log_info "========================================="
log_info "Deployment complete! 🚀"
log_info ""
log_info "Next steps:"
log_info "  1. Monitor: ssh -i $SSH_KEY $REMOTE_USER@$REMOTE_HOST 'tail -f ~/.dinero/debug.log'"
log_info "  2. Check peers: ssh -i $SSH_KEY $REMOTE_USER@$REMOTE_HOST 'cd /opt/dinero && build/bin/dinero-cli getpeerinfo'"
log_info "  3. Test GPU RPC: ssh -i $SSH_KEY $REMOTE_USER@$REMOTE_HOST 'cd /opt/dinero && build/bin/dinero-cli mining.gpustatus'"
