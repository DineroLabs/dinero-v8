#!/bin/bash
#
# Phase N.1: Launch Two Nodes for Manual Testing
#
# This script launches two Dinero nodes with separate datadirs
# for manual P2P testing and verification.
#
# Usage:
#   ./launch_two_nodes.sh         # Start nodes
#   ./launch_two_nodes.sh stop    # Stop nodes
#   ./launch_two_nodes.sh clean   # Clean datadirs

set -e

# Configuration
NODE_A_DATADIR="/tmp/dinero-manual-node-a"
NODE_B_DATADIR="/tmp/dinero-manual-node-b"
NODE_A_RPC_PORT=40001
NODE_B_RPC_PORT=40002
NODE_A_P2P_PORT=40003
NODE_B_P2P_PORT=40004
BUILD_DIR="$(cd "$(dirname "$0")/../../build" && pwd)"
DINEROD="$BUILD_DIR/dinerod"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "════════════════════════════════════════════════════════════"
echo "  Phase N.1: Two-Node Network Test"
echo "════════════════════════════════════════════════════════════"

# Check if dinerod exists
if [ ! -f "$DINEROD" ]; then
    echo -e "${RED}ERROR: dinerod not found at $DINEROD${NC}"
    echo "Please build dinerod first: cd build && cmake --build . --target dinerod"
    exit 1
fi

# Function: Stop nodes
stop_nodes() {
    echo -e "\n${YELLOW}Stopping nodes...${NC}"
    pkill -f "dinerod.*$NODE_A_DATADIR" 2>/dev/null || true
    pkill -f "dinerod.*$NODE_B_DATADIR" 2>/dev/null || true
    sleep 2
    echo -e "${GREEN}✅ Nodes stopped${NC}"
}

# Function: Clean datadirs
clean_datadirs() {
    echo -e "\n${YELLOW}Cleaning datadirs...${NC}"
    rm -rf "$NODE_A_DATADIR"
    rm -rf "$NODE_B_DATADIR"
    echo -e "${GREEN}✅ Datadirs cleaned${NC}"
}

# Function: Start nodes
start_nodes() {
    echo -e "\n${YELLOW}Creating datadirs...${NC}"
    mkdir -p "$NODE_A_DATADIR"
    mkdir -p "$NODE_B_DATADIR"

    echo -e "\n${YELLOW}Starting Node A...${NC}"
    "$DINEROD" \
        --datadir="$NODE_A_DATADIR" \
        --rpcport=$NODE_A_RPC_PORT \
        --p2pport=$NODE_A_P2P_PORT \
        --regtest \
        -daemon > /dev/null 2>&1 &

    sleep 2

    echo -e "\n${YELLOW}Starting Node B (connected to Node A)...${NC}"
    "$DINEROD" \
        --datadir="$NODE_B_DATADIR" \
        --rpcport=$NODE_B_RPC_PORT \
        --p2pport=$NODE_B_P2P_PORT \
        --connect=127.0.0.1:$NODE_A_P2P_PORT \
        --regtest \
        -daemon > /dev/null 2>&1 &

    echo -e "\n${YELLOW}Waiting for nodes to start...${NC}"
    sleep 3

    # Verify nodes are running
    if curl -s http://127.0.0.1:$NODE_A_RPC_PORT -d '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' > /dev/null 2>&1; then
        echo -e "${GREEN}✅ Node A is running${NC}"
    else
        echo -e "${RED}❌ Node A failed to start${NC}"
        exit 1
    fi

    if curl -s http://127.0.0.1:$NODE_B_RPC_PORT -d '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' > /dev/null 2>&1; then
        echo -e "${GREEN}✅ Node B is running${NC}"
    else
        echo -e "${RED}❌ Node B failed to start${NC}"
        exit 1
    fi

    echo -e "\n${GREEN}════════════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}  ✅ Two Nodes Running${NC}"
    echo -e "${GREEN}════════════════════════════════════════════════════════════${NC}"
    echo ""
    echo "Node A:"
    echo "  Datadir:  $NODE_A_DATADIR"
    echo "  RPC Port: $NODE_A_RPC_PORT"
    echo "  P2P Port: $NODE_A_P2P_PORT"
    echo ""
    echo "Node B:"
    echo "  Datadir:  $NODE_B_DATADIR"
    echo "  RPC Port: $NODE_B_RPC_PORT"
    echo "  P2P Port: $NODE_B_P2P_PORT"
    echo ""
    echo "Test commands:"
    echo "  # Check block count on Node A"
    echo "  curl -s http://127.0.0.1:$NODE_A_RPC_PORT -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockcount\",\"params\":[]}' | jq"
    echo ""
    echo "  # Check block count on Node B"
    echo "  curl -s http://127.0.0.1:$NODE_B_RPC_PORT -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockcount\",\"params\":[]}' | jq"
    echo ""
    echo "  # Mine a block on Node A"
    echo "  curl -s http://127.0.0.1:$NODE_A_RPC_PORT -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"generate\",\"params\":[1]}' | jq"
    echo ""
    echo "  # Stop nodes"
    echo "  ./launch_two_nodes.sh stop"
    echo ""
}

# Main
case "${1:-start}" in
    start)
        stop_nodes
        clean_datadirs
        start_nodes
        ;;
    stop)
        stop_nodes
        ;;
    clean)
        stop_nodes
        clean_datadirs
        ;;
    restart)
        stop_nodes
        clean_datadirs
        start_nodes
        ;;
    *)
        echo "Usage: $0 {start|stop|clean|restart}"
        exit 1
        ;;
esac
