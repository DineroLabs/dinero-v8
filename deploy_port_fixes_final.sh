#!/usr/bin/env bash
# Deploy Port Configuration Fixes - SSH Deployment 2025
set -e

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}  Port Fixes - SSH Deployment 2025${NC}"
echo -e "${BLUE}======================================${NC}"
echo ""

# Use SSH Deployment 2025 key
SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
SOURCE_DIR="$HOME/Documents/DineroCoin"

# Correct server mapping
SERVERS=(
    "172.93.160.131:California"
    "173.249.195.59:Virginia"
)

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
DEPLOY_DIR="/tmp/dinero-port-fixes-$TIMESTAMP"
mkdir -p "$DEPLOY_DIR"

echo -e "${BLUE}Creating deployment package...${NC}"

# Copy fixed files
mkdir -p "$DEPLOY_DIR/include/daemon/services"
mkdir -p "$DEPLOY_DIR/src/daemon/services"
mkdir -p "$DEPLOY_DIR/src/lightning"

cp "$SOURCE_DIR/include/daemon/services/config_service.h" "$DEPLOY_DIR/include/daemon/services/"
cp "$SOURCE_DIR/src/daemon/services/config_service.cpp" "$DEPLOY_DIR/src/daemon/services/"
cp "$SOURCE_DIR/src/lightning/lightning_service.cpp" "$DEPLOY_DIR/src/lightning/"

# Create deployment script
cat > "$DEPLOY_DIR/deploy_on_server.sh" <<'REMOTEEOF'
#!/bin/bash
set -e

echo "================================"
echo "  Deploying Port Fixes"
echo "================================"

# Find DineroCoin directory
for dir in "$HOME/DineroCoin" "/root/DineroCoin" "/root/dinero" "/opt/dinero"; do
    if [ -d "$dir" ]; then
        DINERO_DIR="$dir"
        break
    fi
done

if [ -z "$DINERO_DIR" ]; then
    echo "ERROR: Cannot find Dinero installation"
    exit 1
fi

echo "Using: $DINERO_DIR"

# Backup
BACKUP_DIR="$DINERO_DIR/backups/port-fixes-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP_DIR"

cp "$DINERO_DIR/include/daemon/services/config_service.h" "$BACKUP_DIR/" 2>/dev/null || true
cp "$DINERO_DIR/src/daemon/services/config_service.cpp" "$BACKUP_DIR/" 2>/dev/null || true
cp "$DINERO_DIR/src/lightning/lightning_service.cpp" "$BACKUP_DIR/" 2>/dev/null || true

echo "Backup: $BACKUP_DIR"

# Install fixes
cp include/daemon/services/config_service.h "$DINERO_DIR/include/daemon/services/"
cp src/daemon/services/config_service.cpp "$DINERO_DIR/src/daemon/services/"
cp src/lightning/lightning_service.cpp "$DINERO_DIR/src/lightning/"

echo "Files updated!"

# Build
cd "$DINERO_DIR"

if [ -d "build-vendored" ]; then
    BUILD_DIR="build-vendored"
elif [ -d "build" ]; then
    BUILD_DIR="build"
else
    echo "ERROR: No build directory found"
    exit 1
fi

echo "Building in $BUILD_DIR..."
cd "$BUILD_DIR"

cmake --build . --target dinerod -j$(nproc) 2>&1 | tail -20

if [ -f "dinerod" ]; then
    ls -lh dinerod
elif [ -f "bin/dinerod" ]; then
    ls -lh bin/dinerod
else
    echo "ERROR: Binary not found after build"
    exit 1
fi

echo ""
echo "✅ Deployment complete!"
REMOTEEOF

chmod +x "$DEPLOY_DIR/deploy_on_server.sh"

echo -e "${GREEN}Package created${NC}"
echo ""

# Deploy to servers
for server in "${SERVERS[@]}"; do
    IFS=':' read -r IP NAME <<< "$server"

    echo -e "${BLUE}======================================${NC}"
    echo -e "${BLUE}  $NAME ($IP)${NC}"
    echo -e "${BLUE}======================================${NC}"
    echo ""

    echo -e "${YELLOW}[1/3] Uploading...${NC}"
    scp -i "$SSH_KEY" -o StrictHostKeyChecking=no -r "$DEPLOY_DIR" root@$IP:/tmp/
    echo "✅ Uploaded"
    echo ""

    echo -e "${YELLOW}[2/3] Building...${NC}"
    ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no root@$IP << SSHEOF
cd /tmp/$(basename $DEPLOY_DIR)
bash deploy_on_server.sh
SSHEOF

    echo ""
    echo -e "${YELLOW}[3/3] Verifying...${NC}"
    ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no root@$IP << SSHEOF
echo "Dinero directory:"
ls -d ~/DineroCoin /root/DineroCoin /root/dinero /opt/dinero 2>/dev/null | head -1 || echo "Not found"
echo ""
echo "Latest backup:"
ls -td ~/DineroCoin/backups/port-fixes-* /root/DineroCoin/backups/port-fixes-* 2>/dev/null | head -1 || echo "No backup"
SSHEOF

    echo ""
    echo -e "${GREEN}✅ $NAME complete!${NC}"
    echo ""
done

rm -rf "$DEPLOY_DIR"

echo -e "${BLUE}======================================${NC}"
echo -e "${GREEN}  Deployment Complete!${NC}"
echo -e "${BLUE}======================================${NC}"
echo ""
echo "Fixes deployed:"
echo "  ✅ P2P port configuration (--p2pport)"
echo "  ✅ Datadir configuration (--datadir)"
echo "  ✅ Lightning port configuration (--lightningport)"
echo ""
echo "Next: Restart daemons on servers"
echo ""
