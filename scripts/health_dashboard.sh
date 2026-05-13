#!/bin/bash
# Dinero Health Dashboard
# Real-time monitoring of mining status and blockchain health

# Configuration
export RPC="http://127.0.0.1:20998"
export COOKIE="$(tr -d '\r\n' < /Users/haydarevich/Documents/DineroCoin/data/.cookie)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Clear screen and show header
clear
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║                    DINERO HEALTH DASHBOARD                   ║${NC}"
echo -e "${CYAN}║                    Real-time Monitoring                      ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Main monitoring loop
while true; do
    # Get current time
    current_time=$(date '+%Y-%m-%d %H:%M:%S')
    
    # Get mining info
    info=$(curl -s --user "$COOKIE" -H "content-type: application/json" --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' "$RPC")
    
    if [ $? -eq 0 ] && [ "$info" != "null" ]; then
        # Parse JSON response
        height=$(echo "$info" | jq -r .result.blocks)
        hashrate=$(echo "$info" | jq -r .result.hashrate_hps)
        bits=$(echo "$info" | jq -r .result.difficulty_bits)
        target=$(echo "$info" | jq -r .result.target)
        enabled=$(echo "$info" | jq -r .result.mining_enabled)
        phase=$(echo "$info" | jq -r .result.mining_phase)
        
        # Move cursor to top and clear screen
        printf "\033[H\033[2J"
        
        # Header
        echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
        echo -e "${CYAN}║                    DINERO HEALTH DASHBOARD                   ║${NC}"
        echo -e "${CYAN}║                    Real-time Monitoring                      ║${NC}"
        echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
        echo ""
        
        # Status indicators
        if [ "$enabled" = "true" ]; then
            echo -e "⛏️  ${GREEN}Mining: ENABLED${NC}"
        else
            echo -e "⛏️  ${RED}Mining: DISABLED${NC}"
        fi
        
        echo -e "📊 ${BLUE}Height:${NC} $height"
        echo -e "🚀 ${BLUE}Hashrate:${NC} $(printf "%.0f" $hashrate) H/s"
        echo -e "🎯 ${BLUE}Difficulty:${NC} 0x$(printf '%x' $bits)"
        echo -e "🎯 ${BLUE}Target:${NC} ${target:0:8}..."
        echo -e "📈 ${BLUE}Phase:${NC} $phase"
        echo -e "🕐 ${BLUE}Time:${NC} $current_time"
        echo ""
        
        # Health indicators
        echo -e "${PURPLE}Health Status:${NC}"
        
        # Check target format
        if [[ "$target" == "00002710"* ]]; then
            echo -e "  ✅ Target format: ${GREEN}Canonical (CPU-friendly)${NC}"
        else
            echo -e "  ⚠️  Target format: ${YELLOW}Non-canonical${NC}"
        fi
        
        # Check hashrate
        if (( $(echo "$hashrate > 100000" | bc -l) )); then
            echo -e "  ✅ Hashrate: ${GREEN}Healthy (>100k H/s)${NC}"
        else
            echo -e "  ⚠️  Hashrate: ${YELLOW}Low (<100k H/s)${NC}"
        fi
        
        # Check if mining is active
        if [ "$enabled" = "true" ]; then
            echo -e "  ✅ Mining: ${GREEN}Active${NC}"
        else
            echo -e "  ❌ Mining: ${RED}Inactive${NC}"
        fi
        
        echo ""
        echo -e "${CYAN}Press Ctrl+C to exit${NC}"
        
    else
        # Error state
        printf "\033[H\033[2J"
        echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
        echo -e "${CYAN}║                    DINERO HEALTH DASHBOARD                   ║${NC}"
        echo -e "${CYAN}║                    Real-time Monitoring                      ║${NC}"
        echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
        echo ""
        echo -e "${RED}❌ Daemon not responding${NC}"
        echo -e "${YELLOW}🕐 Time: $current_time${NC}"
        echo ""
        echo -e "${CYAN}Press Ctrl+C to exit${NC}"
    fi
    
    # Wait 2 seconds before next update
    sleep 2
done
