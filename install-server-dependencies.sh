#!/usr/bin/env bash
#
# Install build dependencies on DineroCoin servers
# Run before Lightning deployment
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Server configurations
SERVERS=(
    "DineroVA|173.249.195.59|~/.ssh/dinero_deployment_2025"
    "DineroLA|172.93.160.131|~/.ssh/dinero_deployment_2025"
)

echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Installing Build Dependencies on Servers${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo ""

for server_entry in "${SERVERS[@]}"; do
    IFS='|' read -r server_name server_ip server_key <<< "$server_entry"
    
    echo -e "${YELLOW}Installing on $server_name ($server_ip)...${NC}"
    
    ssh -i "$server_key" root@$server_ip << 'EOF'
        echo "Updating package lists..."
        apt-get update -qq
        
        echo "Installing build dependencies..."
        apt-get install -y \
            build-essential \
            cmake \
            perl \
            autoconf \
            automake \
            libtool \
            git \
            wget \
            curl
        
        echo "✓ Dependencies installed"
        
        # Verify critical tools
        echo ""
        echo "Installed versions:"
        gcc --version | head -1
        g++ --version | head -1
        cmake --version | head -1
        libtool --version | head -1
EOF
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ $server_name ready for Lightning deployment${NC}"
    else
        echo -e "${RED}✗ Failed to install dependencies on $server_name${NC}"
    fi
    echo ""
done

echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Dependencies Installation Complete${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo ""
echo "Both servers are now ready for Lightning deployment."
echo ""
echo "Run: ./deploy-lightning-to-all-servers.sh"

