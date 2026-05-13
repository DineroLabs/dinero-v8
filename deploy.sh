#!/usr/bin/env bash
# DineroCoin Incremental Deployment Script
# Syncs only changed files to servers and triggers incremental rebuild

set -e

# Server configuration
CA_SERVER="172.93.160.131"
VA_SERVER="173.249.195.59"
SSH_KEY="$HOME/.ssh/dinero_deployment_2025"
REMOTE_PATH="/root/DineroCoin"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🚀 DineroCoin Incremental Deployment"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Function to deploy to a server
deploy_to_server() {
    local SERVER=$1
    local NAME=$2

    echo -e "${GREEN}📡 Deploying to ${NAME} (${SERVER})...${NC}"

    # Sync only changed source files (exclude build artifacts and data)
    echo "  → Syncing changed files..."
    rsync -avz --delete \
        --exclude='build*/' \
        --exclude='.git/' \
        --exclude='data/' \
        --exclude='*.o' \
        --exclude='*.a' \
        --exclude='*.dylib' \
        --exclude='*.so' \
        --exclude='__pycache__/' \
        --exclude='.DS_Store' \
        -e "ssh -i $SSH_KEY" \
        . root@${SERVER}:${REMOTE_PATH}/ | grep -E "^\.|sent|received" || true

    echo -e "  ${GREEN}✅ Files synced${NC}"

    # Trigger incremental build on server
    echo "  → Triggering incremental build..."
    ssh -i "$SSH_KEY" root@${SERVER} "
        cd ${REMOTE_PATH} &&
        echo '  → Running incremental build...' &&
        cd build-linux 2>/dev/null || (echo '  → No build dir, running full build...' && ./build-server.sh && exit 0) &&
        echo '  → Incremental build with make...' &&
        make -j\$(nproc) 2>&1 | tail -20
    " || {
        echo -e "  ${YELLOW}⚠️  Build failed or first time, trying full build...${NC}"
        ssh -i "$SSH_KEY" root@${SERVER} "cd ${REMOTE_PATH} && ./build-server.sh"
    }

    echo -e "  ${GREEN}✅ Build complete on ${NAME}${NC}"
    echo ""
}

# Check if we're in the right directory
if [ ! -f "build-server.sh" ]; then
    echo -e "${RED}❌ Error: Must run from DineroCoin directory${NC}"
    exit 1
fi

# Deploy to both servers
deploy_to_server "$CA_SERVER" "California"
deploy_to_server "$VA_SERVER" "Virginia"

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}✅ Deployment complete!${NC}"
echo ""
echo "Next steps:"
echo "  1. Restart daemons: ./deploy.sh restart"
echo "  2. Check status: ./deploy.sh status"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
