#!/usr/bin/env bash
set -euo pipefail

# Dinero Self-Starting Wrapper
# This script automatically starts dinerod if needed, waits for RPC readiness,
# then executes your commands. Zero-friction UX!

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
DAEMON="$APP_DIR/build/bin/dinerod"
CLI="$APP_DIR/build/bin/dinero-cli"

# Configuration
NETWORK="${NETWORK:-mainnet}"
DATADIR="${DATADIR:-/Users/haydarevich/Documents/DineroCoin/data/mainnet}"
RPC_HOST="${RPC_HOST:-127.0.0.1}"
RPC_PORT="${RPC_PORT:-20998}"
LOGDIR="$DATADIR/logs"
COOKIE_FILE="$DATADIR/.cookie"
PID_FILE="$DATADIR/dinerod.pid"

# Create necessary directories
mkdir -p "$LOGDIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}ℹ️  $1${NC}"; }
log_success() { echo -e "${GREEN}✅ $1${NC}"; }
log_warning() { echo -e "${YELLOW}⚠️  $1${NC}"; }
log_error() { echo -e "${RED}❌ $1${NC}"; }

# Check if daemon is already running and RPC is ready
is_ready() {
    [[ -f "$COOKIE_FILE" ]] || return 1
    
    local AUTH
    AUTH="$(tr -d '\r\n' < "$COOKIE_FILE")" || return 1
    
    # Test RPC with getblockcount
    curl -sS --connect-timeout 1 --max-time 2 \
        --user "$AUTH" \
        -H 'content-type: application/json' \
        --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
        "http://$RPC_HOST:$RPC_PORT/" >/dev/null 2>&1
}

# Start daemon if not already running
start_daemon_if_needed() {
    if is_ready; then
        log_success "Daemon is already running and RPC is ready."
        return
    fi
    
    # Check if daemon process exists
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid=$(cat "$PID_FILE" 2>/dev/null || echo "")
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            log_warning "Daemon process exists but RPC not ready. Waiting..."
        else
            # Remove stale PID file
            rm -f "$PID_FILE"
        fi
    fi
    
    log_info "Starting dinerod..."
    "$DAEMON" \
        -server=1 \
        -listen=1 \
        -rpcbind="$RPC_HOST" \
        -rpcallowip="$RPC_HOST" \
        -datadir="$DATADIR" \
        -rpcport="$RPC_PORT" \
        -printtoconsole=0 \
        -debuglogfile="$LOGDIR/daemon.log" \
        -daemon=1
    
    log_info "Waiting for RPC to become ready..."
    local retry_count=0
    local max_retries=90
    
    while [[ $retry_count -lt $max_retries ]]; do
        if is_ready; then
            log_success "Daemon RPC ready on port $RPC_PORT"
            return
        fi
        
        retry_count=$((retry_count + 1))
        echo -n "."
        sleep 1
        
        # Show progress every 10 seconds
        if [[ $((retry_count % 10)) -eq 0 ]]; then
            echo " ($retry_count/$max_retries)"
        fi
    done
    
    echo
    log_error "Daemon did not become ready after $max_retries seconds."
    log_error "Check logs: $LOGDIR/daemon.log"
    exit 1
}

# Stop daemon gracefully
stop_daemon() {
    if [[ -f "$COOKIE_FILE" ]]; then
        local AUTH
        AUTH="$(tr -d '\r\n' < "$COOKIE_FILE")" || return 1
        
        log_info "Stopping daemon..."
        "$CLI" \
            -datadir="$DATADIR" \
            -rpcport="$RPC_PORT" \
            -rpcconnect="$RPC_HOST" \
            -rpcuserpass="$AUTH" \
            stop || true
    fi
}

# Cleanup on exit
cleanup() {
    log_info "Cleaning up..."
    # Uncomment the next line if you want to stop daemon when wrapper exits
    # stop_daemon
}

# Set up cleanup trap
trap cleanup EXIT

# Main execution
main() {
    log_info "Dinero Self-Starting Wrapper"
    log_info "Network: $NETWORK"
    log_info "Data directory: $DATADIR"
    log_info "RPC endpoint: $RPC_HOST:$RPC_PORT"
    echo
    
    # Ensure daemon is running
    start_daemon_if_needed
    
    # Get authentication from cookie
    local AUTH
    AUTH="$(tr -d '\r\n' < "$COOKIE_FILE")"
    
    log_success "Ready to execute commands!"
    echo
    
    # Execute the command passed as arguments
    if [[ $# -gt 0 ]]; then
        log_info "Executing: $*"
        "$CLI" \
            -datadir="$DATADIR" \
            -rpcport="$RPC_PORT" \
            -rpcconnect="$RPC_HOST" \
            -rpcuserpass="$AUTH" \
            "$@"
    else
        # Interactive mode
        log_info "Entering interactive mode. Type 'help' for available commands."
        log_info "Type 'exit' to quit."
        echo
        
        while true; do
            echo -n "dinero> "
            read -r cmd
            
            if [[ "$cmd" == "exit" || "$cmd" == "quit" ]]; then
                break
            elif [[ "$cmd" == "help" ]]; then
                echo "Available commands:"
                echo "  getblockchaininfo  - Get blockchain status"
                echo "  getwalletinfo      - Get wallet status"
                echo "  getnewaddress      - Generate new address"
                echo "  getbalance         - Get wallet balance"
                echo "  getmininginfo      - Get mining status"
                echo "  help               - Show this help"
                echo "  exit/quit          - Exit interactive mode"
                echo
            elif [[ -n "$cmd" ]]; then
                "$CLI" \
                    -datadir="$DATADIR" \
                    -rpcport="$RPC_PORT" \
                    -rpcconnect="$RPC_HOST" \
                    -rpcuserpass="$AUTH" \
                    $cmd
                echo
            fi
        done
    fi
}

# Run main function with all arguments
main "$@"
