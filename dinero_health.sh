#!/bin/bash

# Dinero Health Dashboard - Simple & Reliable
# No Qt, no complex dependencies - just curl and jq

RPC_URL="http://127.0.0.1:20998"
COOKIE_FILE="/Users/haydarevich/Documents/DineroCoin/data/mainnet/.cookie"
INTERVAL=2

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if daemon is running
check_daemon() {
    if ! pgrep -f dinerod > /dev/null; then
        echo -e "${RED}❌ Dinero daemon is not running${NC}"
        echo "Start it with: ./build/bin/dinerod -datadir=/Users/haydarevich/Documents/DineroCoin/data/mainnet -rpcport=20998 -port=20999 &"
        exit 1
    fi
}

# Check if cookie file exists
check_cookie() {
    if [[ ! -f "$COOKIE_FILE" ]]; then
        echo -e "${RED}❌ Cookie file not found: $COOKIE_FILE${NC}"
        exit 1
    fi
}

# Get mining info
get_mining_info() {
    local cookie=$(cat "$COOKIE_FILE")
    curl -s -u "$cookie" \
         -H 'content-type: application/json' \
         --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' \
         "$RPC_URL" 2>/dev/null
}

# Display health dashboard
display_health() {
    local response=$(get_mining_info)
    
    if [[ -z "$response" ]]; then
        echo -e "${RED}❌ Connection failed${NC}"
        return 1
    fi
    
    local error=$(echo "$response" | jq -r '.error // empty' 2>/dev/null)
    if [[ -n "$error" ]]; then
        echo -e "${RED}❌ RPC Error: $error${NC}"
        return 1
    fi
    
    local blocks=$(echo "$response" | jq -r '.result.blocks // 0')
    local difficulty=$(echo "$response" | jq -r '.result.difficulty // 0')
    local hashrate=$(echo "$response" | jq -r '.result.hashrate_hps // 0')
    local target=$(echo "$response" | jq -r '.result.target // "unknown"')
    local mining_enabled=$(echo "$response" | jq -r '.result.mining_enabled // false')
    local mining_address=$(echo "$response" | jq -r '.result.mining_address // "none"')
    local testnet=$(echo "$response" | jq -r '.result.testnet // false')
    
    # Clear screen
    clear
    
    # Display dashboard
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║                    DINERO HEALTH DASHBOARD                   ║"
    echo "╠══════════════════════════════════════════════════════════════╣"
    printf "║ Height: %-8s │ Difficulty: %-12.2f ║\n" "$blocks" "$difficulty"
    printf "║ Hashrate: %-8.1f H/s │ Target: %-8s... ║\n" "$hashrate" "${target:0:8}"
    
    local mining_status="DISABLED"
    local mining_color="$RED"
    if [[ "$mining_enabled" == "true" ]]; then
        mining_status="ENABLED "
        mining_color="$GREEN"
    fi
    
    local network="TESTNET"
    local network_color="$YELLOW"
    if [[ "$testnet" == "false" ]]; then
        network="MAINNET"
        network_color="$GREEN"
    fi
    
    printf "║ Mining: ${mining_color}%-8s${NC} │ Network: ${network_color}%-8s${NC} ║\n" "$mining_status" "$network"
    printf "║ Address: %-20s... ║\n" "${mining_address:0:20}"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo "Updated: $(date '+%H:%M:%S') | Press Ctrl+C to stop"
}

# Main monitoring loop
monitor() {
    echo -e "${GREEN}🚀 Starting Dinero Health Monitor...${NC}"
    echo "RPC: $RPC_URL"
    echo "Cookie: $COOKIE_FILE"
    echo "Interval: ${INTERVAL}s"
    echo ""
    
    while true; do
        display_health
        sleep "$INTERVAL"
    done
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --rpc)
            RPC_URL="$2"
            shift 2
            ;;
        --cookie)
            COOKIE_FILE="$2"
            shift 2
            ;;
        --interval)
            INTERVAL="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --rpc URL        RPC URL (default: http://127.0.0.1:20998)"
            echo "  --cookie PATH    Cookie file path"
            echo "  --interval SEC   Update interval (default: 2)"
            echo "  --help           Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Check prerequisites
check_daemon
check_cookie

# Check if jq is available
if ! command -v jq &> /dev/null; then
    echo -e "${RED}❌ jq is required but not installed${NC}"
    echo "Install with: brew install jq"
    exit 1
fi

# Start monitoring
monitor
