#!/bin/bash

# Dinero Health Monitoring Script
# Cross-platform health dashboard with standardized paths and auth

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration with environment variable overrides. Keep these aligned with
# daemon startup defaults in src/daemon/main.cpp and chainparams_impl.cpp.
NETWORK="${DINERO_NETWORK:-mainnet}"
apply_network_defaults() {
    case "$NETWORK" in
        "mainnet")
            DEFAULT_RPC_URL="http://127.0.0.1:20998"
            DEFAULT_DATA_DIR="$HOME/.dinero"
            ;;
        "testnet")
            DEFAULT_RPC_URL="http://127.0.0.1:20998"
            DEFAULT_DATA_DIR="$HOME/.dinero/testnet"
            ;;
        "regtest")
            DEFAULT_RPC_URL="http://127.0.0.1:20996"
            DEFAULT_DATA_DIR="$HOME/.dinero/regtest"
            ;;
        *)
            echo -e "${RED}Error: Invalid network '$NETWORK'${NC}"
            echo "Valid networks: mainnet, testnet, regtest"
            exit 1
            ;;
    esac

    RPC_URL="${DINERO_RPC_URL:-$DEFAULT_RPC_URL}"
    DATA_DIR="${DINERO_DATADIR:-$DEFAULT_DATA_DIR}"
    COOKIE_FILE="${DINERO_COOKIE_FILE:-$DATA_DIR/.cookie}"
}
apply_network_defaults

# Exit codes
EXIT_HEALTHY=0
EXIT_UNHEALTHY=1
EXIT_DAEMON_DOWN=2

# Check dependencies
check_dependencies() {
    local missing_deps=()
    
    if ! command -v curl &> /dev/null; then
        missing_deps+=("curl")
    fi
    
    if ! command -v jq &> /dev/null; then
        missing_deps+=("jq")
    fi
    
    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        echo -e "${RED}Error: Missing dependencies: ${missing_deps[*]}${NC}"
        echo "Please install missing dependencies and try again."
        exit $EXIT_DAEMON_DOWN
    fi
}

# Get authentication header from cookie file
get_auth_header() {
    if [[ -f "$COOKIE_FILE" ]]; then
        local cookie=$(cat "$COOKIE_FILE" | tr -d '\n\r')
        if [[ -n "$cookie" ]]; then
            # Cookie format: "username:password"
            if [[ "$cookie" == *":"* ]]; then
                echo "Authorization: Basic $(echo -n "$cookie" | base64)"
            else
                # Assume it's just the password, use default username
                echo "Authorization: Basic $(echo -n "__cookie__:$cookie" | base64)"
            fi
        fi
    fi
}

# Make RPC call
rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    local auth_header=$(get_auth_header)
    
    local curl_args=(
        -s
        -X POST
        "$RPC_URL"
        -H "Content-Type: application/json"
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}"
    )
    
    if [[ -n "$auth_header" ]]; then
        curl_args+=(-H "$auth_header")
    fi
    
    curl "${curl_args[@]}"
}

# Check if daemon is responding
check_daemon_health() {
    local response=$(rpc_call "gethealth" 2>/dev/null)
    local http_code=$?
    
    if [[ $http_code -ne 0 ]]; then
        return $EXIT_DAEMON_DOWN
    fi
    
    if [[ -z "$response" ]]; then
        return $EXIT_DAEMON_DOWN
    fi
    
    # Check if response contains error
    if echo "$response" | jq -e '.error' > /dev/null 2>&1; then
        return $EXIT_DAEMON_DOWN
    fi
    
    return $EXIT_HEALTHY
}

# Get health information
get_health_info() {
    local response=$(rpc_call "gethealth")
    echo "$response" | jq -r '.result // empty'
}

# Get mining information
get_mining_info() {
    local response=$(rpc_call "getmininginfo")
    echo "$response" | jq -r '.result // empty'
}

# Get blockchain information
get_blockchain_info() {
    local response=$(rpc_call "getblockchaininfo")
    echo "$response" | jq -r '.result // empty'
}

# Get network information
get_network_info() {
    local response=$(rpc_call "getnetworkinfo")
    echo "$response" | jq -r '.result // empty'
}

# Display health dashboard
display_dashboard() {
    echo -e "${BLUE}=== Dinero Health Dashboard ===${NC}"
    echo -e "Network: ${YELLOW}$NETWORK${NC}"
    echo -e "RPC URL: ${YELLOW}$RPC_URL${NC}"
    echo -e "Data Dir: ${YELLOW}$DATA_DIR${NC}"
    echo
    
    # Get all information
    local health_info=$(get_health_info)
    local mining_info=$(get_mining_info)
    local blockchain_info=$(get_blockchain_info)
    local network_info=$(get_network_info)
    
    if [[ -z "$health_info" ]]; then
        echo -e "${RED}❌ Failed to get health information${NC}"
        return $EXIT_UNHEALTHY
    fi
    
    # Parse and display health info
    local status=$(echo "$health_info" | jq -r '.status // "unknown"')
    local chain=$(echo "$health_info" | jq -r '.chain // "unknown"')
    local height=$(echo "$health_info" | jq -r '.height // 0')
    local hashrate=$(echo "$health_info" | jq -r '.hashrate // 0')
    local mining_enabled=$(echo "$health_info" | jq -r '.mining_enabled // false')
    local mempool_size=$(echo "$health_info" | jq -r '.mempool_size // 0')
    local connections=$(echo "$health_info" | jq -r '.connections // 0')
    local uptime=$(echo "$health_info" | jq -r '.uptime // 0')
    
    # Status indicator
    if [[ "$status" == "healthy" ]]; then
        echo -e "Status: ${GREEN}✅ $status${NC}"
    else
        echo -e "Status: ${RED}❌ $status${NC}"
    fi
    
    echo -e "Chain: ${YELLOW}$chain${NC}"
    echo -e "Height: ${YELLOW}$height${NC}"
    echo -e "Hashrate: ${YELLOW}$(printf "%.2f" "$hashrate") H/s${NC}"
    
    # Mining status
    if [[ "$mining_enabled" == "true" ]]; then
        echo -e "Mining: ${GREEN}✅ Enabled${NC}"
    else
        echo -e "Mining: ${YELLOW}⏸️  Disabled${NC}"
    fi
    
    echo -e "Mempool: ${YELLOW}$mempool_size transactions${NC}"
    echo -e "Connections: ${YELLOW}$connections peers${NC}"
    
    if [[ $uptime -gt 0 ]]; then
        local uptime_hours=$((uptime / 3600))
        local uptime_mins=$(((uptime % 3600) / 60))
        echo -e "Uptime: ${YELLOW}${uptime_hours}h ${uptime_mins}m${NC}"
    fi
    
    echo
    
    # Additional mining details if available
    if [[ -n "$mining_info" ]]; then
        local difficulty=$(echo "$mining_info" | jq -r '.difficulty // 0')
        local network_hashrate=$(echo "$mining_info" | jq -r '.networkhashps // 0')
        local mining_address=$(echo "$mining_info" | jq -r '.mining_address // "none"')
        
        echo -e "${BLUE}=== Mining Details ===${NC}"
        echo -e "Difficulty: ${YELLOW}$(printf "%.2f" "$difficulty")${NC}"
        echo -e "Network Hashrate: ${YELLOW}$(printf "%.2f" "$network_hashrate") H/s${NC}"
        echo -e "Mining Address: ${YELLOW}$mining_address${NC}"
        echo
    fi
    
    # Network details if available
    if [[ -n "$network_info" ]]; then
        local version=$(echo "$network_info" | jq -r '.version // "unknown"')
        local subversion=$(echo "$network_info" | jq -r '.subversion // "unknown"')
        
        echo -e "${BLUE}=== Network Details ===${NC}"
        echo -e "Version: ${YELLOW}$version${NC}"
        echo -e "Subversion: ${YELLOW}$subversion${NC}"
        echo
    fi
    
    # Return appropriate exit code
    if [[ "$status" == "healthy" ]]; then
        return $EXIT_HEALTHY
    else
        return $EXIT_UNHEALTHY
    fi
}

# Main function
main() {
    # Check dependencies
    check_dependencies
    
    # Check if daemon is responding
    if ! check_daemon_health; then
        echo -e "${RED}❌ Daemon is not responding${NC}"
        echo -e "RPC URL: ${YELLOW}$RPC_URL${NC}"
        echo -e "Data Dir: ${YELLOW}$DATA_DIR${NC}"
        echo -e "Cookie File: ${YELLOW}$COOKIE_FILE${NC}"
        echo
        echo "Troubleshooting:"
        echo "  1. Check if dinerod is running"
        echo "  2. Verify RPC URL and port"
        echo "  3. Check cookie file exists and is readable"
        echo "  4. Check firewall settings"
        exit $EXIT_DAEMON_DOWN
    fi
    
    # Display dashboard
    display_dashboard
}

# Handle command line arguments
case "${1:-}" in
    "--help" | "-h")
        echo "Dinero Health Monitoring Script"
        echo
        echo "Usage: $0 [options]"
        echo
        echo "Options:"
        echo "  --help, -h     Show this help message"
        echo "  --json         Output health info as JSON"
        echo "  --network N    Set network (mainnet, testnet, regtest)"
        echo
        echo "Environment Variables:"
        echo "  DINERO_RPC_URL      RPC endpoint URL"
        echo "  DINERO_DATADIR      Data directory path"
        echo "  DINERO_COOKIE_FILE  Cookie file path"
        echo "  DINERO_NETWORK      Network name"
        echo
        echo "Exit Codes:"
        echo "  0  Healthy"
        echo "  1  Unhealthy"
        echo "  2  Daemon down"
        exit 0
        ;;
    "--json")
        get_health_info | jq .
        exit $?
        ;;
    "--network")
        if [[ -n "${2:-}" ]]; then
            NETWORK="$2"
            apply_network_defaults
        fi
        main
        ;;
    "")
        main
        ;;
    *)
        echo -e "${RED}Error: Unknown option '$1'${NC}"
        echo "Use --help for usage information"
        exit 1
        ;;
esac
