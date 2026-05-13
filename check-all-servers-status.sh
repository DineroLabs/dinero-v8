#!/usr/bin/env bash
#
# Quick Status Check for All DineroCoin Servers
#

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  DineroCoin Servers Status (LA + VA)${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
echo ""

# SSH Key
SSH_KEY="~/.ssh/dinero_deployment_2025"

# DineroVA (173.249.195.59)
echo -e "${YELLOW}DineroVA (173.249.195.59):${NC}"
if ssh -i $SSH_KEY -o ConnectTimeout=5 root@173.249.195.59 "pgrep -f dinerod" 2>/dev/null; then
    PID=$(ssh -i $SSH_KEY root@173.249.195.59 "pgrep -f dinerod")
    VERSION=$(ssh -i $SSH_KEY root@173.249.195.59 "/root/dinerod --version 2>&1 | head -1" 2>/dev/null || echo "Unknown")
    echo -e "  ${GREEN}✓ Running${NC} (PID: $PID)"
    echo -e "  Version: $VERSION"
else
    echo -e "  ${RED}✗ Not running${NC}"
fi
echo ""

# DineroLA (172.93.160.131)
echo -e "${YELLOW}DineroLA (172.93.160.131):${NC}"
if ssh -i $SSH_KEY -o ConnectTimeout=5 root@172.93.160.131 "pgrep -f dinerod" 2>/dev/null; then
    PID=$(ssh -i $SSH_KEY root@172.93.160.131 "pgrep -f dinerod")
    VERSION=$(ssh -i $SSH_KEY root@172.93.160.131 "/root/dinerod --version 2>&1 | head -1" 2>/dev/null || echo "Unknown")
    echo -e "  ${GREEN}✓ Running${NC} (PID: $PID)"
    echo -e "  Version: $VERSION"
else
    echo -e "  ${RED}✗ Not running${NC}"
fi
echo ""

echo -e "${GREEN}════════════════════════════════════════════════════${NC}"
