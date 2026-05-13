#!/bin/bash
# Dinero Linux Deployment - Genesis Fix + Latest Updates
# November 7, 2025
#
# What this deploys:
#   ✅ Genesis hash fix (173fe6da... hardcoded)
#   ✅ Premine integration (2,627,900 DIN)
#   ✅ std::bad_alloc fix (hardcoded values)
#   ✅ ASERT DAA from block 2
#   ✅ Coin type 1447
#   ✅ All production fixes

set -e

SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
CA_SERVER="172.93.160.131"
VA_SERVER="173.249.195.59"
LOCAL_REPO="/Users/haydarevich/Documents/DineroCoin"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "═══════════════════════════════════════════════════════════════════════"
echo "🚀 DINERO LINUX DEPLOYMENT - GENESIS FIX + LATEST UPDATES"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "Servers:"
echo "  🌐 California:  $CA_SERVER"
echo "  🌐 Virginia:    $VA_SERVER"
echo ""
echo "Critical Updates Being Deployed:"
echo "  ✅ Genesis hash: 173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33"
echo "  ✅ Premine: 2,627,900 DIN (Block 1)"
echo "  ✅ std::bad_alloc fix (hardcoded mainnet values)"
echo "  ✅ Network mismatch fix (mainnet everywhere)"
echo "  ✅ ASERT DAA from block 2 (no Bitcoin DAA)"
echo "  ✅ Coin type: 1447 (Dinero SLIP-0044)"
echo "  ✅ ExplorerDB + monitoring"
echo ""
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Function to check if server is reachable
check_server() {
    local server=$1
    local name=$2
    
    echo -e "${BLUE}🔍 Checking connection to $name...${NC}"
    if ssh -i $SSH_KEY -o ConnectTimeout=5 root@$server "echo 'OK'" &>/dev/null; then
        echo -e "${GREEN}✅ $name is reachable${NC}"
        return 0
    else
        echo -e "${RED}❌ $name is NOT reachable${NC}"
        return 1
    fi
}

# Function to check disk space
check_disk_space() {
    local server=$1
    local name=$2
    
    echo -e "${BLUE}📊 Checking disk space on $name...${NC}"
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        echo "Current disk usage:"
        df -h / | grep -v Filesystem
        echo ""
        echo "DineroCoin directory size:"
        du -sh /root/DineroCoin 2>/dev/null || echo "Not found"
REMOTE
    echo ""
}

# Function to stop daemon
stop_daemon() {
    local server=$1
    local name=$2
    
    echo -e "${YELLOW}🛑 Stopping old daemon on $name...${NC}"
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        # Try multiple ways to stop the daemon
        pkill -9 dinerod || true
        killall -9 dinerod || true
        systemctl stop dinerod || true
        
        # Wait for process to die
        sleep 2
        
        if pgrep dinerod > /dev/null; then
            echo "⚠️  Daemon still running, forcing kill..."
            pkill -9 dinerod || true
            sleep 1
        fi
        
        if ! pgrep dinerod > /dev/null; then
            echo "✅ Daemon stopped"
        else
            echo "❌ Failed to stop daemon"
            exit 1
        fi
REMOTE
}

# Function to sync code from Mac to server
sync_code() {
    local server=$1
    local name=$2
    
    echo -e "${BLUE}📦 Syncing latest code to $name via rsync...${NC}"
    
    # Create remote directory if it doesn't exist
    ssh -i $SSH_KEY root@$server "mkdir -p /root/DineroCoin"
    
    # Sync code (exclude build artifacts and data)
    rsync -avz --delete \
        --exclude 'build/' \
        --exclude 'build-*/' \
        --exclude '.git/' \
        --exclude 'data/' \
        --exclude '.dinero/' \
        --exclude '*.o' \
        --exclude '*.a' \
        --exclude 'node_modules/' \
        --exclude '.DS_Store' \
        -e "ssh -i $SSH_KEY" \
        "$LOCAL_REPO/" \
        root@$server:/root/DineroCoin/
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ Code synced to $name${NC}"
    else
        echo -e "${RED}❌ Failed to sync code to $name${NC}"
        exit 1
    fi
    echo ""
}

# Function to build on server
build_on_server() {
    local server=$1
    local name=$2
    
    echo -e "${BLUE}🔨 Building daemon on $name (Linux)...${NC}"
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        cd /root/DineroCoin
        
        # Clean old build
        echo "Cleaning old build artifacts..."
        rm -rf build/ CMakeCache.txt CMakeFiles/
        
        # Create build directory
        mkdir -p build
        cd build
        
        # Configure with CMake
        echo ""
        echo "Running CMake..."
        cmake -DCMAKE_BUILD_TYPE=Release \
              -DENABLE_SANITIZERS=OFF \
              -DDIN_EXPERIMENTAL_FEATURES=OFF \
              .. || exit 1
        
        # Build
        echo ""
        echo "Building dinerod..."
        make -j$(nproc) dinerod || exit 1
        
        # Verify binary exists
        if [ -f "dinerod" ]; then
            echo ""
            echo "✅ Build successful!"
            ls -lh dinerod
        else
            echo ""
            echo "❌ Build failed - binary not found"
            exit 1
        fi
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ Build successful on $name${NC}"
    else
        echo -e "${RED}❌ Build failed on $name${NC}"
        exit 1
    fi
    echo ""
}

# Function to start daemon
start_daemon() {
    local server=$1
    local name=$2
    
    echo -e "${BLUE}🚀 Starting daemon on $name with mainnet config...${NC}"
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        cd /root/DineroCoin
        
        # Create data directory
        mkdir -p /root/.dinero/mainnet
        
        # Start daemon in background with nohup
        nohup ./build/dinerod \
            -datadir=/root/.dinero \
            -rpcport=20998 \
            -port=20999 \
            -rpcbind=0.0.0.0 \
            -rpcallowip=0.0.0.0/0 \
            > /root/.dinero/mainnet/dinerod.log 2>&1 &
        
        # Wait for startup
        sleep 3
        
        # Check if running
        if pgrep dinerod > /dev/null; then
            echo "✅ Daemon started successfully"
            echo "PID: $(pgrep dinerod)"
        else
            echo "❌ Failed to start daemon"
            echo "Last 20 lines of log:"
            tail -20 /root/.dinero/mainnet/dinerod.log
            exit 1
        fi
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ Daemon started on $name${NC}"
    else
        echo -e "${RED}❌ Failed to start daemon on $name${NC}"
        exit 1
    fi
    echo ""
}

# Function to verify deployment
verify_deployment() {
    local server=$1
    local name=$2
    
    echo -e "${BLUE}🔍 Verifying deployment on $name...${NC}"
    ssh -i $SSH_KEY root@$server << 'REMOTE'
        echo "Waiting 10 seconds for daemon initialization..."
        sleep 10
        
        echo ""
        echo "Checking daemon process:"
        if pgrep dinerod > /dev/null; then
            echo "✅ Process running (PID: $(pgrep dinerod))"
        else
            echo "❌ Process NOT running"
            exit 1
        fi
        
        echo ""
        echo "Last 30 lines of daemon log:"
        echo "═══════════════════════════════════════════════════════════════════════"
        tail -30 /root/.dinero/mainnet/dinerod.log
        echo "═══════════════════════════════════════════════════════════════════════"
        
        echo ""
        echo "Checking for critical issues:"
        if grep -i "std::bad_alloc" /root/.dinero/mainnet/dinerod.log; then
            echo "❌ CRITICAL: std::bad_alloc still present!"
            exit 1
        fi
        
        if grep -i "Network mismatch" /root/.dinero/mainnet/dinerod.log; then
            echo "❌ CRITICAL: Network mismatch still present!"
            exit 1
        fi
        
        if grep -i "Regtest genesis" /root/.dinero/mainnet/dinerod.log; then
            echo "❌ CRITICAL: Regtest genesis warning still present!"
            exit 1
        fi
        
        if grep -i "Genesis hash mismatch" /root/.dinero/mainnet/dinerod.log; then
            echo "❌ CRITICAL: Genesis hash mismatch still present!"
            exit 1
        fi
        
        echo ""
        echo "✅ No critical errors found in log"
        
        echo ""
        echo "Checking for successful initialization:"
        if grep -i "Genesis stored" /root/.dinero/mainnet/dinerod.log || \
           grep -i "Genesis block" /root/.dinero/mainnet/dinerod.log; then
            echo "✅ Genesis initialization successful"
        fi
        
        if grep -i "Premine stored" /root/.dinero/mainnet/dinerod.log || \
           grep -i "Premine block" /root/.dinero/mainnet/dinerod.log; then
            echo "✅ Premine initialization successful"
        fi
REMOTE
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✅ $name deployment verified${NC}"
    else
        echo -e "${RED}❌ $name deployment has issues${NC}"
    fi
    echo ""
}

# Main deployment function
deploy_to_server() {
    local server=$1
    local name=$2
    
    echo ""
    echo "═══════════════════════════════════════════════════════════════════════"
    echo "🌐 DEPLOYING TO $name ($server)"
    echo "═══════════════════════════════════════════════════════════════════════"
    echo ""
    
    check_server $server "$name" || return 1
    check_disk_space $server "$name"
    stop_daemon $server "$name"
    sync_code $server "$name"
    build_on_server $server "$name"
    start_daemon $server "$name"
    verify_deployment $server "$name"
    
    echo -e "${GREEN}✅ $name deployment complete!${NC}"
    echo ""
}

# Ask which servers to deploy to
echo "Which servers do you want to deploy to?"
echo "  1) California only"
echo "  2) Virginia only"
echo "  3) Both servers"
echo ""
read -p "Enter choice (1-3): " choice

case $choice in
    1)
        deploy_to_server $CA_SERVER "California"
        ;;
    2)
        deploy_to_server $VA_SERVER "Virginia"
        ;;
    3)
        deploy_to_server $CA_SERVER "California"
        deploy_to_server $VA_SERVER "Virginia"
        ;;
    *)
        echo "Invalid choice"
        exit 1
        ;;
esac

echo ""
echo "═══════════════════════════════════════════════════════════════════════"
echo "🎉 DEPLOYMENT COMPLETE!"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "What was deployed:"
echo "  ✅ Genesis hash: 173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33"
echo "  ✅ Premine: 2,627,900 DIN (Block 1)"
echo "  ✅ std::bad_alloc fix (hardcoded mainnet values)"
echo "  ✅ ASERT DAA from block 2"
echo "  ✅ All experimental features disabled"
echo ""
echo "Next steps:"
echo "  1. Monitor logs: ssh -i $SSH_KEY root@<server> tail -f /root/.dinero/mainnet/dinerod.log"
echo "  2. Check RPC: curl --user \$(cat /root/.dinero/mainnet/.cookie) --data-binary '{\"jsonrpc\":\"1.0\",\"method\":\"getblockchaininfo\",\"params\":[]}' http://localhost:20998"
echo "  3. Verify peers are connecting"
echo ""

