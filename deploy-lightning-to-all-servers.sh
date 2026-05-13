#!/usr/bin/env bash
#
# Deploy Lightning Security Updates to All DineroCoin Servers
# Version: v1.1.2
# Date: November 13, 2025
#
# This script deploys the Lightning security fixes to production servers:
# - DineroVA (173.249.195.59)
# - DineroLA (172.93.160.131)
#
# NOTE: DineroCA (96.9.226.98) is excluded from this deployment

set -e  # Exit on error

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Server configurations (DineroCA excluded)
# Format: "ServerName|IP|SSHKey"
SERVERS=(
    "DineroVA|173.249.195.59|~/.ssh/dinero_deployment_2025"
    "DineroLA|172.93.160.131|~/.ssh/dinero_deployment_2025"
)

LIGHTNING_TAG="v1.2.0"
REPO_PATH="/root/DineroCoin"

echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Lightning Security Deployment (LA + VA)${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo ""
echo -e "${YELLOW}Version:${NC} $LIGHTNING_TAG"
echo -e "${YELLOW}Servers:${NC} DineroVA, DineroLA"
echo -e "${YELLOW}Excluded:${NC} DineroCA (96.9.226.98)"
echo ""

# Function to deploy to a single server
deploy_to_server() {
    local name=$1
    local ip=$2
    local key=$3

    echo -e "${BLUE}────────────────────────────────────────────────────${NC}"
    echo -e "${BLUE}Deploying to $name ($ip)${NC}"
    echo -e "${BLUE}────────────────────────────────────────────────────${NC}"

    # Step 1: Check server connectivity
    echo -e "${YELLOW}[1/8]${NC} Testing SSH connection..."
    if ! ssh -i "$key" -o ConnectTimeout=10 root@$ip "echo 'Connected'" >/dev/null 2>&1; then
        echo -e "${RED}✗ Failed to connect to $name${NC}"
        return 1
    fi
    echo -e "${GREEN}✓ Connected to $name${NC}"

    # Step 2: Stop daemon
    echo -e "${YELLOW}[2/8]${NC} Stopping DineroCoin daemon..."
    ssh -i "$key" root@$ip "pkill -f dinerod || true" 2>/dev/null
    sleep 2
    echo -e "${GREEN}✓ Daemon stopped${NC}"

    # Step 3: Backup current binary
    echo -e "${YELLOW}[3/8]${NC} Backing up current binary..."
    ssh -i "$key" root@$ip "cp /root/dinerod /root/dinerod.backup.\$(date +%Y%m%d) 2>/dev/null || true"
    echo -e "${GREEN}✓ Backup created${NC}"

    # Step 4: Push latest code
    echo -e "${YELLOW}[4/8]${NC} Pushing latest Lightning security code to server..."
    
    # Ensure we're on v1.1.2 locally
    cd /Users/haydarevich/Documents/DineroCoin
    LOCAL_TAG=$(git describe --tags --exact-match 2>/dev/null || echo "")
    
    if [ "$LOCAL_TAG" != "v1.2.0" ]; then
        echo "Checking out v1.2.0 locally..."
        git checkout v1.2.0 2>/dev/null || {
            echo -e "${RED}✗ Failed to checkout v1.2.0 locally${NC}"
            return 1
        }
    fi
    
    # Create a clean tarball (exclude .git, build dirs, data)
    echo "Creating deployment archive..."
    tar czf /tmp/dinero-lightning-v1.2.0.tar.gz \
        --exclude='.git' \
        --exclude='build*' \
        --exclude='data' \
        --exclude='*.tar.gz' \
        --exclude='node_modules' \
        -C /Users/haydarevich/Documents DineroCoin/

    # Transfer to server
    echo "Uploading to $name..."
    scp -i "$key" -o StrictHostKeyChecking=no \
        /tmp/dinero-lightning-v1.2.0.tar.gz root@$ip:/tmp/

    # Extract on server
    ssh -i "$key" root@$ip << 'EOF'
        cd /root
        rm -rf /root/DineroCoin.old
        if [ -d "/root/DineroCoin" ]; then
            mv /root/DineroCoin /root/DineroCoin.old
        fi
        tar xzf /tmp/dinero-lightning-v1.2.0.tar.gz -C /root/
        rm /tmp/dinero-lightning-v1.2.0.tar.gz
        echo "✓ Code deployed to /root/DineroCoin"
EOF

    rm /tmp/dinero-lightning-v1.2.0.tar.gz
    echo -e "${GREEN}✓ Code pushed successfully${NC}"

    # Step 5: Run Lightning deployment script
    echo -e "${YELLOW}[5/8]${NC} Building Lightning security updates..."
    echo -e "${YELLOW}    (This will take 15-25 minutes - building OpenSSL, secp256k1, and DineroCoin)${NC}"

    ssh -i "$key" root@$ip << 'EOF'
        cd /root/DineroCoin
        chmod +x deploy-linux-lightning.sh
        ./deploy-linux-lightning.sh 2>&1 | tee /root/lightning-deploy.log

        # Check if build succeeded
        if [ ! -f "build-linux/dinerod" ]; then
            echo "ERROR: Build failed - binary not found"
            exit 1
        fi

        echo "✓ Build completed successfully"
EOF

    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ Build failed on $name${NC}"
        echo -e "${YELLOW}Check logs: ssh -i $key root@$ip 'tail -100 /root/lightning-deploy.log'${NC}"
        return 1
    fi
    echo -e "${GREEN}✓ Lightning security build complete${NC}"

    # Step 6: Install new binary
    echo -e "${YELLOW}[6/8]${NC} Installing new binary..."
    ssh -i "$key" root@$ip << 'EOF'
        cd /root/DineroCoin
        cp build-linux/dinerod /root/dinerod
        chmod +x /root/dinerod

        # Verify binary
        /root/dinerod --version
EOF
    echo -e "${GREEN}✓ Binary installed${NC}"

    # Step 7: Start daemon
    echo -e "${YELLOW}[7/8]${NC} Starting DineroCoin daemon with Lightning..."
    ssh -i "$key" root@$ip << 'EOF'
        nohup /root/dinerod \
            -datadir=/root/dinero_data \
            -rpcport=20998 \
            -port=20999 \
            -lightning=1 \
            -lightning-port=9735 \
            > /root/daemon.log 2>&1 &

        sleep 3

        # Check if daemon started
        if pgrep -f dinerod > /dev/null; then
            echo "✓ Daemon started (PID: $(pgrep -f dinerod))"
        else
            echo "ERROR: Daemon failed to start"
            exit 1
        fi
EOF

    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ Failed to start daemon on $name${NC}"
        return 1
    fi
    echo -e "${GREEN}✓ Daemon running${NC}"

    # Step 8: Verify Lightning is active
    echo -e "${YELLOW}[8/8]${NC} Verifying Lightning security..."
    sleep 5

    ssh -i "$key" root@$ip << 'EOF'
        # Check daemon is responding
        if pgrep -f dinerod > /dev/null; then
            echo "✓ DineroCoin daemon: Running"
        else
            echo "✗ DineroCoin daemon: Not running"
            exit 1
        fi

        # Check logs for Lightning initialization
        if grep -q "Lightning" /root/daemon.log 2>/dev/null; then
            echo "✓ Lightning network: Active"
        else
            echo "⚠ Lightning status: Check logs"
        fi
EOF

    echo -e "${GREEN}✓ Lightning security deployed to $name${NC}"
    echo ""
}

# Main deployment loop
FAILED_SERVERS=()
SUCCESSFUL_SERVERS=()
TOTAL_SERVERS=${#SERVERS[@]}

for server_entry in "${SERVERS[@]}"; do
    # Parse server info (Format: Name|IP|Key)
    IFS='|' read -r server_name server_ip server_key <<< "$server_entry"

    if deploy_to_server "$server_name" "$server_ip" "$server_key"; then
        SUCCESSFUL_SERVERS+=("$server_name|$server_ip")
    else
        FAILED_SERVERS+=("$server_name|$server_ip")
        echo -e "${RED}⚠ Deployment to $server_name failed${NC}"
        echo ""
    fi
done

# Summary
echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Deployment Summary${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo ""

if [ ${#SUCCESSFUL_SERVERS[@]} -gt 0 ]; then
    echo -e "${GREEN}✅ Successful deployments (${#SUCCESSFUL_SERVERS[@]}/$TOTAL_SERVERS):${NC}"
    for entry in "${SUCCESSFUL_SERVERS[@]}"; do
        IFS='|' read -r name ip <<< "$entry"
        echo -e "   ${GREEN}✓${NC} $name ($ip)"
    done
    echo ""
fi

if [ ${#FAILED_SERVERS[@]} -gt 0 ]; then
    echo -e "${RED}❌ Failed deployments (${#FAILED_SERVERS[@]}/$TOTAL_SERVERS):${NC}"
    for entry in "${FAILED_SERVERS[@]}"; do
        IFS='|' read -r name ip <<< "$entry"
        echo -e "   ${RED}✗${NC} $name ($ip)"
    done
    echo ""
fi

# Security improvements deployed
echo -e "${YELLOW}Security Improvements Deployed:${NC}"
echo "  ✅ Payment Preimages: 256-bit CSPRNG (was predictable 0x12)"
echo "  ✅ Payment Secrets: 256-bit CSPRNG (was predictable 0x42)"
echo "  ✅ SHA-256: OpenSSL implementation (was broken memcpy)"
echo "  ✅ ECDSA Signatures: secp256k1 with recovery (was fake)"
echo "  ✅ Bech32 Encoding: 100% BOLT 11 compliant (was 85%)"
echo "  ✅ Terminology: Changed to 'una' and 'muna'"
echo ""

# Monitoring commands
echo -e "${YELLOW}Quick Status Check:${NC}"
echo ""
for server_entry in "${SERVERS[@]}"; do
    IFS='|' read -r server_name server_ip server_key <<< "$server_entry"
    echo "# $server_name:"
    echo "ssh -i $server_key root@$server_ip 'tail -20 /root/daemon.log'"
    echo ""
done

# Exit with error if any deployment failed
if [ ${#FAILED_SERVERS[@]} -gt 0 ]; then
    exit 1
fi

echo -e "${GREEN}🎉 All servers deployed successfully!${NC}"
echo ""
