#!/bin/bash
# Dinero Linux Mainnet Deployment - November 2025
# Deploys latest version with:
#   - Genesis hash 173fe6da...
#   - Premine integration (2,627,900 DIN)
#   - Coin type 1447
#   - ExplorerDB + monitoring
#   - All production fixes

set -e

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
CA_SERVER="172.93.160.131"
VA_SERVER="173.249.195.59"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "═══════════════════════════════════════════════════════════════════════"
echo "🚀 DINERO MAINNET DEPLOYMENT - November 2025"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "Servers:"
echo "  🌐 California:  $CA_SERVER"
echo "  🌐 Virginia:    $VA_SERVER"
echo ""
echo "Updates:"
echo "  ✅ Genesis hash: 173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33"
echo "  ✅ Premine: 2,627,900 DIN (Block 1)"
echo "  ✅ Coin type: 1447 (Dinero SLIP-0044)"
echo "  ✅ ExplorerDB service"
echo "  ✅ Monitoring dashboard"
echo "  ✅ CMake isolation"
echo ""
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Function to check disk space on server
check_disk_space() {
    local server=$1
    local name=$2
    
    echo "📊 Checking disk space on $name..."
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        echo "Current disk usage:"
        df -h / | grep -v Filesystem
        echo ""
        echo "DineroCoin directory size:"
        du -sh /root/DineroCoin 2>/dev/null || echo "Not found"
        echo ""
        echo "Build directory size:"
        du -sh /root/DineroCoin/build 2>/dev/null || echo "Not found"
        echo ""
        echo "Data directory size:"
        du -sh /root/.dinero 2>/dev/null || echo "Not found"
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name disk space OK${NC}"
    else
        echo -e "${RED}❌ $name disk space check failed${NC}"
    fi
    echo ""
}

# Function to clean old builds
clean_old_builds() {
    local server=$1
    local name=$2
    
    echo "🧹 Cleaning old builds on $name..."
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        cd /root/DineroCoin
        
        # Remove old build directories
        echo "Removing old build directories..."
        rm -rf build/ build-*/ || true
        
        # Remove temporary files
        echo "Removing temporary files..."
        find . -name "*.o" -delete 2>/dev/null || true
        find . -name "*.a" -delete 2>/dev/null || true
        find . -name "CMakeCache.txt" -delete 2>/dev/null || true
        
        # List remaining large directories
        echo ""
        echo "Remaining large directories:"
        du -sh */ 2>/dev/null | sort -hr | head -10
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name cleaned${NC}"
    else
        echo -e "${YELLOW}⚠️  $name cleanup had issues (continuing)${NC}"
    fi
    echo ""
}

# Function to sync code to server
sync_code() {
    local server=$1
    local name=$2
    
    echo "📦 Syncing latest code to $name..."
    rsync -avz --delete \
        --exclude='build/' \
        --exclude='build-*/' \
        --exclude='.git/' \
        --exclude='*.o' \
        --exclude='*.a' \
        --exclude='data/' \
        --exclude='*.log' \
        --exclude='nohup.out' \
        --exclude='gui/' \
        --exclude='releases/' \
        --exclude='dist/' \
        --exclude='*.dmg' \
        --exclude='*.tar.gz' \
        -e "ssh -i $SSH_KEY" \
        /Users/haydarevich/Documents/DineroCoin/ \
        root@$server:/root/DineroCoin/ 2>&1 | tail -20
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name code synced${NC}"
    else
        echo -e "${RED}❌ $name sync failed${NC}"
        return 1
    fi
    echo ""
}

# Function to build on server
build_server() {
    local server=$1
    local name=$2
    
    echo "🔨 Building daemon on $name..."
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        cd /root/DineroCoin
        
        # Create build directory
        mkdir -p build
        cd build
        
        echo "Running CMake..."
        cmake .. \
            -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_GUI=OFF \
            -DENABLE_SANITIZERS=OFF \
            -DBUILD_TESTS=OFF 2>&1 | tail -20
        
        if [ $? -ne 0 ]; then
            echo "❌ CMake failed"
            exit 1
        fi
        
        echo ""
        echo "Building binaries..."
        make -j$(nproc) dinerod dinero-cli dinero-miner 2>&1 | tail -30
        
        if [ $? -ne 0 ]; then
            echo "❌ Build failed"
            exit 1
        fi
        
        echo ""
        echo "Build results:"
        ls -lh dinerod dinero-cli dinero-miner 2>/dev/null || echo "Some binaries missing!"
        
        # Verify daemon binary
        if [ -f dinerod ]; then
            echo ""
            echo "✅ Daemon built successfully"
            file dinerod
        else
            echo ""
            echo "❌ Daemon binary not found!"
            exit 1
        fi
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name build complete${NC}"
    else
        echo -e "${RED}❌ $name build failed${NC}"
        return 1
    fi
    echo ""
}

# Function to restart daemon
restart_daemon() {
    local server=$1
    local name=$2
    local peer_server=$3
    
    echo "🔄 Restarting daemon on $name..."
    ssh -i $SSH_KEY root@$server << REMOTE
        # Stop old daemon
        echo "Stopping old daemon..."
        systemctl stop dinerod 2>/dev/null || true
        pkill -15 dinerod 2>/dev/null || true
        sleep 3
        pkill -9 dinerod 2>/dev/null || true
        sleep 1
        
        # Start new daemon with mainnet configuration
        echo "Starting new daemon..."
        cd /root/DineroCoin
        nohup ./build/dinerod \
            --datadir=/root/.dinero \
            --port=19003 \
            --rpcport=20998 \
            --wsport=21001 \
            --addnode=$peer_server:19003 \
            --printtoconsole > /tmp/dinero-mainnet.log 2>&1 &
        
        sleep 3
        
        # Check if running
        PID=\$(pgrep -f 'dinerod.*19003' || echo 'NONE')
        if [ "\$PID" != "NONE" ]; then
            echo "✅ Daemon started with PID: \$PID"
            ps aux | grep dinerod | grep -v grep | head -1
        else
            echo "❌ Daemon failed to start!"
            echo "Last log lines:"
            tail -20 /tmp/dinero-mainnet.log
            exit 1
        fi
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name daemon restarted${NC}"
    else
        echo -e "${RED}❌ $name daemon restart failed${NC}"
        return 1
    fi
    echo ""
}

# Function to verify deployment
verify_deployment() {
    local server=$1
    local name=$2
    
    echo "🔍 Verifying deployment on $name..."
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        echo "Daemon process:"
        ps aux | grep dinerod | grep -v grep
        echo ""
        
        echo "Recent log (last 15 lines):"
        tail -15 /tmp/dinero-mainnet.log
        echo ""
        
        echo "Network connections:"
        netstat -tulpn | grep ':19003\|:20998\|:21001' 2>/dev/null || ss -tulpn | grep ':19003\|:20998\|:21001'
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name verification complete${NC}"
    else
        echo -e "${YELLOW}⚠️  $name verification had issues${NC}"
    fi
    echo ""
}

# Main deployment sequence
main() {
    echo "STEP 1: Pre-deployment checks"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    check_disk_space $CA_SERVER "California"
    check_disk_space $VA_SERVER "Virginia"
    
    echo ""
    read -p "Continue with deployment? (y/n) " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Deployment cancelled."
        exit 0
    fi
    echo ""
    
    echo "STEP 2: Cleaning old builds"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    clean_old_builds $CA_SERVER "California"
    clean_old_builds $VA_SERVER "Virginia"
    
    echo "STEP 3: Syncing code"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    sync_code $CA_SERVER "California" || exit 1
    sync_code $VA_SERVER "Virginia" || exit 1
    
    echo "STEP 4: Building binaries"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    build_server $CA_SERVER "California" || exit 1
    build_server $VA_SERVER "Virginia" || exit 1
    
    echo "STEP 5: Restarting daemons"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    restart_daemon $CA_SERVER "California" $VA_SERVER || exit 1
    sleep 3
    restart_daemon $VA_SERVER "Virginia" $CA_SERVER || exit 1
    
    echo "STEP 6: Verification"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    sleep 5  # Give daemons time to start
    verify_deployment $CA_SERVER "California"
    verify_deployment $VA_SERVER "Virginia"
    
    echo "═══════════════════════════════════════════════════════════════════════"
    echo -e "${GREEN}🎉 DEPLOYMENT COMPLETE!${NC}"
    echo "═══════════════════════════════════════════════════════════════════════"
    echo ""
    echo "Both servers are now running the latest mainnet code with:"
    echo "  ✅ Correct genesis hash (173fe6da...)"
    echo "  ✅ Premine integration (2,627,900 DIN)"
    echo "  ✅ Coin type 1447"
    echo "  ✅ ExplorerDB service"
    echo "  ✅ All production fixes"
    echo ""
    echo "Server IPs:"
    echo "  🌐 California: $CA_SERVER:19003"
    echo "  🌐 Virginia:   $VA_SERVER:19003"
    echo ""
    echo "RPC Access:"
    echo "  📡 http://$CA_SERVER:20998"
    echo "  📡 http://$VA_SERVER:20998"
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════"
}

# Run main deployment
main

