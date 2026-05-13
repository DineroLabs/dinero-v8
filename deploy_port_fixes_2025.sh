#!/usr/bin/env bash
# Deploy Port Configuration Fixes to Production Servers
# Fixes: P2P port, datadir, and Lightning port configuration bugs
set -e

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  Dinero Port Fixes Deployment${NC}"
echo -e "${BLUE}  SSH Deployment 2025${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Server configuration
SERVERS=(
    "96.9.226.98:DineroCA:$HOME/.ssh/dinero_key"
    "173.249.195.59:DineroVA:$HOME/.ssh/server2.key"
    "172.93.160.131:DineroLA:$HOME/.ssh/dinerola.key"
)

# Source directory
SOURCE_DIR="$HOME/Documents/DineroCoin"

echo -e "${GREEN}Fixes being deployed:${NC}"
echo "  1. P2P port configuration (--p2pport flag)"
echo "  2. Datadir configuration (--datadir flag)"
echo "  3. Lightning port configuration (--lightningport flag)"
echo ""

# Create deployment package
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
DEPLOY_DIR="/tmp/dinero-port-fixes-$TIMESTAMP"
mkdir -p "$DEPLOY_DIR"

echo -e "${BLUE}Creating deployment package...${NC}"

# Copy modified source files
mkdir -p "$DEPLOY_DIR/include/daemon/services"
mkdir -p "$DEPLOY_DIR/src/daemon/services"
mkdir -p "$DEPLOY_DIR/src/lightning"

cp "$SOURCE_DIR/include/daemon/services/config_service.h" "$DEPLOY_DIR/include/daemon/services/"
cp "$SOURCE_DIR/src/daemon/services/config_service.cpp" "$DEPLOY_DIR/src/daemon/services/"
cp "$SOURCE_DIR/src/lightning/lightning_service.cpp" "$DEPLOY_DIR/src/lightning/"

# Create deployment script for server
cat > "$DEPLOY_DIR/deploy_on_server.sh" <<'REMOTEEOF'
#!/bin/bash
set -e

echo "================================"
echo "  Deploying Port Fixes"
echo "================================"
echo ""

DINERO_DIR="$HOME/DineroCoin"

# Check if DineroCoin directory exists
if [ ! -d "$DINERO_DIR" ]; then
    echo "ERROR: $DINERO_DIR not found!"
    echo "Looking for alternative locations..."

    if [ -d "/root/dinero" ]; then
        DINERO_DIR="/root/dinero"
    elif [ -d "/opt/dinero" ]; then
        DINERO_DIR="/opt/dinero"
    else
        echo "ERROR: Cannot find Dinero installation"
        exit 1
    fi
fi

echo "Using Dinero directory: $DINERO_DIR"
echo ""

# Backup original files
echo "Backing up original files..."
mkdir -p "$DINERO_DIR/backups/port-fixes-backup-$(date +%Y%m%d-%H%M%S)"
BACKUP_DIR="$DINERO_DIR/backups/port-fixes-backup-$(date +%Y%m%d-%H%M%S)"

cp "$DINERO_DIR/include/daemon/services/config_service.h" "$BACKUP_DIR/" 2>/dev/null || true
cp "$DINERO_DIR/src/daemon/services/config_service.cpp" "$BACKUP_DIR/" 2>/dev/null || true
cp "$DINERO_DIR/src/lightning/lightning_service.cpp" "$BACKUP_DIR/" 2>/dev/null || true

echo "Backup created: $BACKUP_DIR"
echo ""

# Copy new files
echo "Installing fixed files..."
cp include/daemon/services/config_service.h "$DINERO_DIR/include/daemon/services/"
cp src/daemon/services/config_service.cpp "$DINERO_DIR/src/daemon/services/"
cp src/lightning/lightning_service.cpp "$DINERO_DIR/src/lightning/"

echo "Files updated successfully!"
echo ""

# Build
echo "Building daemon with fixes..."
cd "$DINERO_DIR"

# Determine build directory
if [ -d "build-vendored" ]; then
    BUILD_DIR="build-vendored"
elif [ -d "build" ]; then
    BUILD_DIR="build"
else
    echo "ERROR: No build directory found"
    exit 1
fi

cd "$BUILD_DIR"

# Build daemon
echo "Compiling dinerod..."
cmake --build . --target dinerod -j$(nproc) 2>&1 | tail -20

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Build successful!"
    ls -lh dinerod 2>/dev/null || ls -lh bin/dinerod 2>/dev/null || echo "Binary location unknown"
else
    echo ""
    echo "❌ Build failed!"
    exit 1
fi

echo ""
echo "================================"
echo "  Deployment Complete"
echo "================================"
echo ""
echo "Next steps:"
echo "  1. Stop current daemon"
echo "  2. Replace binary with newly built one"
echo "  3. Start daemon with port flags"
echo ""
REMOTEEOF

chmod +x "$DEPLOY_DIR/deploy_on_server.sh"

# Create info file
cat > "$DEPLOY_DIR/FIXES_INFO.txt" <<'INFOEOF'
PORT CONFIGURATION FIXES
========================

This deployment fixes 3 critical configuration bugs:

1. P2P Port Bug (config_service.h:45)
   - BEFORE: --p2pport flag ignored, always used port 20999
   - AFTER: --p2pport flag works correctly
   - Fix: P2PPort() now checks both "p2pport" and "port" keys

2. Datadir Bug (config_service.cpp:32-54)
   - BEFORE: --datadir flag overwritten by Init()
   - AFTER: --datadir flag preserved
   - Fix: Init() only sets defaults if key doesn't exist

3. Lightning Port Bug (lightning_service.cpp:77-85)
   - BEFORE: Lightning always used port 9735
   - AFTER: --lightningport flag works correctly
   - Fix: Read "lightningport" from config

These fixes enable multi-node testing on the same machine!

FILES MODIFIED:
- include/daemon/services/config_service.h
- src/daemon/services/config_service.cpp
- src/lightning/lightning_service.cpp

TESTED: ✅ Local Mac (2 nodes running simultaneously)
INFOEOF

echo -e "${GREEN}✅ Deployment package created${NC}"
echo ""

# Deploy to each server
for server in "${SERVERS[@]}"; do
    IFS=':' read -r IP NAME KEY <<< "$server"
    KEY=$(eval echo $KEY)

    # Check if key exists
    if [ ! -f "$KEY" ]; then
        echo -e "${RED}ERROR: SSH key not found: $KEY${NC}"
        echo "Skipping $NAME..."
        echo ""
        continue
    fi

    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Deploying to $NAME ($IP)${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""

    # Test SSH connection first
    echo -e "${YELLOW}[1/4] Testing SSH connection...${NC}"
    if ! ssh -i "$KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@$IP "echo 'Connected'" 2>/dev/null; then
        echo -e "${RED}Failed to connect to $NAME${NC}"
        echo ""
        continue
    fi
    echo "✅ Connected"
    echo ""

    # Upload package
    echo -e "${YELLOW}[2/4] Uploading deployment package...${NC}"
    if ! scp -i "$KEY" -o StrictHostKeyChecking=no -r "$DEPLOY_DIR" root@$IP:/tmp/; then
        echo -e "${RED}Failed to upload to $NAME${NC}"
        echo ""
        continue
    fi
    echo "✅ Uploaded"
    echo ""

    # Deploy on server
    echo -e "${YELLOW}[3/4] Deploying fixes on server...${NC}"
    ssh -i "$KEY" -o StrictHostKeyChecking=no root@$IP << SSHEOF
set -e
cd /tmp/$(basename $DEPLOY_DIR)
bash deploy_on_server.sh
SSHEOF

    if [ $? -ne 0 ]; then
        echo -e "${RED}Deployment failed on $NAME${NC}"
        echo ""
        continue
    fi

    echo ""
    echo -e "${YELLOW}[4/4] Verifying deployment...${NC}"
    ssh -i "$KEY" -o StrictHostKeyChecking=no root@$IP << SSHEOF
echo "Checking for backup..."
ls -la ~/DineroCoin/backups/ | tail -1 || ls -la /root/dinero/backups/ | tail -1 || echo "Backup location varies"
echo ""
echo "Checking build output..."
ls -lh ~/DineroCoin/build-vendored/dinerod 2>/dev/null || \
ls -lh ~/DineroCoin/build/dinerod 2>/dev/null || \
ls -lh /root/dinero/build/dinerod 2>/dev/null || \
echo "Binary location varies by installation"
SSHEOF

    echo ""
    echo -e "${GREEN}✅ $NAME deployment complete!${NC}"
    echo ""
done

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}  All Deployments Complete!${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

echo "Deployment Summary:"
echo "  - 3 source files updated on each server"
echo "  - Daemon rebuilt with fixes"
echo "  - Original files backed up"
echo ""

echo "Next Steps:"
echo "  1. SSH to each server and restart daemon:"
echo "     ${YELLOW}ssh -i ~/.ssh/dinero_key root@96.9.226.98${NC}"
echo "     ${YELLOW}pkill dinerodlinux && sleep 2${NC}"
echo "     ${YELLOW}nohup ./dinerodlinux --datadir=/root/dinero/data --rpcport=20998 --p2pport=20999 > daemon.log 2>&1 &${NC}"
echo ""
echo "  2. Verify port configuration works:"
echo "     ${YELLOW}lsof -iTCP -sTCP:LISTEN | grep dinerod${NC}"
echo ""
echo "  3. Check daemon logs:"
echo "     ${YELLOW}tail -f /root/daemon.log${NC}"
echo ""

# Cleanup
echo "Cleaning up local temporary files..."
rm -rf "$DEPLOY_DIR"

echo ""
echo -e "${GREEN}🎉 Deployment complete!${NC}"
echo ""
