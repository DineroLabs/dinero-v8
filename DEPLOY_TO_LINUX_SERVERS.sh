#!/bin/bash
# Deploy Dinero with Headers-First Sync to Linux Servers
# Builds directly on server (required for Linux compatibility)

set -e

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Dinero Linux Server Deployment${NC}"
echo -e "${BLUE}  Headers-First Sync Update${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Check for saved credentials
if [ ! -f ~/.ssh/dinero_servers.conf ]; then
    echo -e "${RED}ERROR: Server credentials not found!${NC}"
    echo "Expected: ~/.ssh/dinero_servers.conf"
    echo ""
    echo "Create this file with:"
    echo "SERVER1=user@ip"
    echo "SERVER2=user@ip"
    echo "SSH_KEY=/path/to/key"
    exit 1
fi

# Load credentials
source ~/.ssh/dinero_servers.conf

echo -e "${GREEN}Loaded server credentials:${NC}"
echo "  Server 1: $SERVER1"
echo "  Server 2: $SERVER2"
echo "  SSH Key: $SSH_KEY"
echo ""

# Select target
echo -e "${YELLOW}Select deployment target:${NC}"
echo "  1) Server 1 ($SERVER1)"
echo "  2) Server 2 ($SERVER2)"
echo "  3) Both servers"
echo ""
read -p "Choice [1-3]: " choice

case $choice in
    1) SERVERS=("$SERVER1") ;;
    2) SERVERS=("$SERVER2") ;;
    3) SERVERS=("$SERVER1" "$SERVER2") ;;
    *) echo "Invalid choice"; exit 1 ;;
esac

# Create deployment package with SOURCE CODE
echo -e "${BLUE}Creating source deployment package...${NC}"
DEPLOY_DIR="dinero-source-deploy-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$DEPLOY_DIR"

# Copy essential source files
echo "Copying source files..."
rsync -av \
    --exclude='.git' \
    --exclude='build*' \
    --exclude='data' \
    --exclude='*.log' \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='third_party' \
    --exclude='x-gui' \
    --exclude='releases' \
    --exclude='packaging' \
    src/ "$DEPLOY_DIR/src/"

cp -r include/ "$DEPLOY_DIR/include/"
cp CMakeLists.txt "$DEPLOY_DIR/"
cp dinero.conf.sample "$DEPLOY_DIR/"

# Create build script for server
cat > "$DEPLOY_DIR/build_on_server.sh" <<'BUILDEOF'
#!/bin/bash
# Build Dinero on Linux Server
set -e

echo "================================"
echo "  Building Dinero on Linux"
echo "================================"
echo ""

# Install dependencies if needed
if ! command -v cmake &> /dev/null; then
    echo "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        libssl-dev \
        libsqlite3-dev \
        libjsoncpp-dev \
        qt6-base-dev \
        libsecp256k1-dev \
        pkg-config
fi

# Create build directory
mkdir -p build
cd build

# Configure
echo "Configuring build..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SANITIZERS=OFF \
    -DBUILD_SHARED_LIBS=OFF

# Build daemon only (not GUI or tests)
echo "Building daemon..."
make -j$(nproc) dinerod

echo ""
echo "✅ Build complete!"
echo "Binary: build/bin/dinerod"
ls -lh bin/dinerod
BUILDEOF

chmod +x "$DEPLOY_DIR/build_on_server.sh"

# Create deployment script for server
cat > "$DEPLOY_DIR/deploy_and_restart.sh" <<'DEPLOYEOF'
#!/bin/bash
# Deploy and Restart Dinero Daemon
set -e

INSTALL_DIR="/opt/dinero"
DATA_DIR="$INSTALL_DIR/data"

echo "================================"
echo "  Deploying Dinero Daemon"
echo "================================"
echo ""

# Stop existing daemon
if systemctl is-active --quiet dinerod; then
    echo "Stopping existing daemon..."
    sudo systemctl stop dinerod
    sleep 2
fi

# Backup old binary
if [ -f "$INSTALL_DIR/dinerod" ]; then
    echo "Backing up old binary..."
    sudo cp "$INSTALL_DIR/dinerod" "$INSTALL_DIR/dinerod.backup.$(date +%Y%m%d-%H%M%S)"
fi

# Install new binary
echo "Installing new binary..."
sudo cp build/bin/dinerod "$INSTALL_DIR/dinerod"
sudo chmod +x "$INSTALL_DIR/dinerod"
sudo chown dinero:dinero "$INSTALL_DIR/dinerod" 2>/dev/null || true

# Start daemon
echo "Starting daemon..."
sudo systemctl start dinerod
sleep 3

# Check status
echo ""
echo "Checking daemon status..."
sudo systemctl status dinerod --no-pager || true

echo ""
echo "✅ Deployment complete!"
echo ""
echo "Monitor logs:"
echo "  sudo journalctl -u dinerod -f"
echo ""
echo "Check sync:"
echo "  $INSTALL_DIR/dinerod getblockchaininfo"
DEPLOYEOF

chmod +x "$DEPLOY_DIR/deploy_and_restart.sh"

# Create README
cat > "$DEPLOY_DIR/README.txt" <<'READMEEOF'
DEPLOYMENT INSTRUCTIONS
========================

This package contains the Dinero source code with headers-first sync.

AUTOMATIC DEPLOYMENT:
  ./build_on_server.sh && ./deploy_and_restart.sh

MANUAL STEPS:
  1. Build:
     ./build_on_server.sh

  2. Deploy:
     ./deploy_and_restart.sh

  3. Monitor:
     sudo journalctl -u dinerod -f

WHAT'S NEW:
  - Headers-first sync (5x faster blockchain sync)
  - Full P2P message parsing
  - Better error handling
  - Timeout management

FILES CHANGED:
  - src/daemon/p2p/peer_manager.cpp (131 new lines)
  - src/daemon/p2p/headers_first_sync.cpp
  - include/p2p/headers_first_sync.h

ROLLBACK:
  If issues occur:
    sudo systemctl stop dinerod
    sudo cp dinerod.backup.* dinerod
    sudo systemctl start dinerod
READMEEOF

# Package it
echo "Creating tarball..."
tar -czf "${DEPLOY_DIR}.tar.gz" "$DEPLOY_DIR"
echo -e "${GREEN}✅ Package created: ${DEPLOY_DIR}.tar.gz${NC}"
echo ""

# Deploy to each server
for SERVER in "${SERVERS[@]}"; do
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Deploying to $SERVER${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    
    # Upload package
    echo -e "${YELLOW}[1/4] Uploading source package...${NC}"
    scp -i "$SSH_KEY" "${DEPLOY_DIR}.tar.gz" "$SERVER:/tmp/" || {
        echo -e "${RED}Failed to upload to $SERVER${NC}"
        continue
    }
    
    # Extract and build
    echo -e "${YELLOW}[2/4] Building on server...${NC}"
    ssh -i "$SSH_KEY" "$SERVER" <<SSHEOF
set -e
cd /tmp
tar -xzf ${DEPLOY_DIR}.tar.gz
cd $DEPLOY_DIR
./build_on_server.sh
SSHEOF
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}Build failed on $SERVER${NC}"
        continue
    fi
    
    # Deploy
    echo -e "${YELLOW}[3/4] Deploying binary...${NC}"
    ssh -i "$SSH_KEY" "$SERVER" <<SSHEOF
set -e
cd /tmp/$DEPLOY_DIR
./deploy_and_restart.sh
SSHEOF
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}Deployment failed on $SERVER${NC}"
        continue
    fi
    
    # Verify
    echo -e "${YELLOW}[4/4] Verifying deployment...${NC}"
    ssh -i "$SSH_KEY" "$SERVER" <<SSHEOF
/opt/dinero/dinerod --version
systemctl is-active dinerod && echo "✅ Daemon running" || echo "❌ Daemon not running"
SSHEOF
    
    echo ""
    echo -e "${GREEN}✅ Deployment to $SERVER complete!${NC}"
    echo ""
done

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}  All deployments complete!${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo "Next steps:"
echo "  1. Monitor logs on each server:"
echo "     ssh $SERVER1 'sudo journalctl -u dinerod -f | grep HeadersSync'"
echo ""
echo "  2. Check blockchain sync:"
echo "     ssh $SERVER1 '/opt/dinero/dinerod getblockchaininfo'"
echo ""
echo "  3. Verify P2P connections:"
echo "     ssh $SERVER1 '/opt/dinero/dinerod getpeerinfo | grep addr'"
echo ""

# Cleanup
rm -rf "$DEPLOY_DIR"
echo "Cleaned up temporary files."

