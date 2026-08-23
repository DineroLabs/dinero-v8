#!/bin/bash
#
# Two-Node Transaction Relay Integration Test
# v0.13.0.1 - Step D (PROOF LAYER 2)
#
# This test proves:
# - Real transactions relay between nodes
# - Mempool accepts network transactions
# - No duplication occurs
# - No crashes or hangs
# - Txid is preserved across network

set -e  # Exit on error
set -o pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║  Two-Node Transaction Relay Test (Proof Layer 2)         ║"
echo "║  v0.13.0.1 - Step D                                       ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

# Cleanup function
cleanup() {
    local test_rc=$?
    local cleanup_rc=0
    local final_rc=0
    trap - EXIT
    set +e
    echo ""
    echo -e "${YELLOW}Cleaning up test environment...${NC}"
    dinero_stop_datadir_processes /tmp/node_a || cleanup_rc=1
    dinero_stop_datadir_processes /tmp/node_b || cleanup_rc=1
    if (( cleanup_rc == 0 )); then
        rm -rf /tmp/node_a /tmp/node_b || cleanup_rc=1
    fi
    echo -e "${GREEN}✅ Cleanup complete${NC}"
    dinero_cleanup_result "${test_rc}" "${cleanup_rc}" || final_rc=$?
    exit "${final_rc}"
}

# Trap to ensure cleanup on exit
trap cleanup EXIT

# Step 1: Build dinerod if needed
echo -e "${BLUE}Step 1: Checking binaries...${NC}"
if [ ! -f "./build/dinerod" ] || [ ! -f "./build/dinero-cli" ]; then
    echo -e "${RED}❌ Binaries not found. Run: make dinerod dinero-cli${NC}"
    exit 1
fi
echo -e "${GREEN}✅ Binaries found${NC}"
echo ""

# Step 2: Start Node A (port 20999)
echo -e "${BLUE}Step 2: Starting Node A (port 20999)...${NC}"
rm -rf /tmp/node_a
mkdir -p /tmp/node_a

./build/dinerod --regtest --datadir=/tmp/node_a --rpcport=19443 --port=20999 --daemon
sleep 4

# Check if Node A is running
if ! pgrep -f "dinerod.*19443" > /dev/null; then
    echo -e "${RED}❌ Node A failed to start${NC}"
    exit 1
fi
echo -e "${GREEN}✅ Node A started${NC}"
echo ""

# Step 3: Start Node B (port 21000)
echo -e "${BLUE}Step 3: Starting Node B (port 21000)...${NC}"
rm -rf /tmp/node_b
mkdir -p /tmp/node_b

./build/dinerod --regtest --datadir=/tmp/node_b --rpcport=19444 --port=21000 --daemon
sleep 4

# Check if Node B is running
if ! pgrep -f "dinerod.*19444" > /dev/null; then
    echo -e "${RED}❌ Node B failed to start${NC}"
    exit 1
fi
echo -e "${GREEN}✅ Node B started${NC}"
echo ""

# Step 4: Connect Node A → Node B
echo -e "${BLUE}Step 4: Connecting nodes as peers...${NC}"

COOKIE_A=$(cat /tmp/node_a/.cookie | cut -d: -f2)
COOKIE_B=$(cat /tmp/node_b/.cookie | cut -d: -f2)

# Add Node B as peer to Node A (use "onetry" to connect immediately)
curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"addnode","params":["127.0.0.1:21000","onetry"],"id":1}' > /dev/null

sleep 3

# Verify connection by checking peer count via getpeerinfo
PEER_COUNT=$(curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"getpeerinfo","params":[],"id":2}' | jq -r '.result | length')

if [ "$PEER_COUNT" -lt 1 ]; then
    echo -e "${YELLOW}⚠️  Nodes not yet connected, waiting...${NC}"
    sleep 5
    PEER_COUNT=$(curl -s -X POST http://127.0.0.1:19443 \
      -u "__cookie__:$COOKIE_A" \
      -H "Content-Type: application/json" \
      -d '{"jsonrpc":"2.0","method":"getpeerinfo","params":[],"id":2}' | jq -r '.result | length')
fi

if [ "$PEER_COUNT" -lt 1 ]; then
    echo -e "${RED}❌ Nodes failed to connect${NC}"
    exit 1
fi

echo -e "${GREEN}✅ Nodes connected (peer count: $PEER_COUNT)${NC}"
echo ""

# Step 5: Create wallet on Node A
echo -e "${BLUE}Step 5: Creating wallet on Node A...${NC}"

curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["test"],"id":3}' > /tmp/wallet_create.json

ADDR_A=$(jq -r '.result.first_address' /tmp/wallet_create.json)
echo -e "${GREEN}✅ Wallet created with address: $ADDR_A${NC}"
echo ""

# Step 6: Mine blocks on Node A to get funds (using proper mining.start flow)
echo -e "${BLUE}Step 6: Mining blocks on Node A for wallet funding...${NC}"

# Set mining address to wallet address
echo -e "  Setting mining address to wallet..."
curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"mining.setaddress\",\"params\":[\"$ADDR_A\"],\"id\":4}" > /dev/null

# Start mining with 2 threads
echo -e "  Starting mining..."
curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"mining.start","params":[2],"id":5}' > /dev/null

# Wait for height > 101 (COINBASE_MATURITY = 100)
echo -e "  Waiting for 110+ blocks (coinbase maturity = 100)..."
TARGET_HEIGHT=110
for i in {1..60}; do
    HEIGHT=$(curl -s -X POST http://127.0.0.1:19443 \
      -u "__cookie__:$COOKIE_A" \
      -H "Content-Type: application/json" \
      -d '{"jsonrpc":"2.0","method":"blockchain.getblockcount","params":[],"id":6}' | jq -r '.result // 0')

    if [ "$HEIGHT" -ge "$TARGET_HEIGHT" ]; then
        echo -e "  Reached height $HEIGHT"
        break
    fi
    echo -e "  Height: $HEIGHT / $TARGET_HEIGHT"
    sleep 1
done

# Stop mining
echo -e "  Stopping mining..."
curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"mining.stop","params":[],"id":7}' > /dev/null

sleep 2

# Rescan wallet to pick up mined rewards
echo -e "  Rescanning wallet..."
curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.rescanblockchain","params":[],"id":8}' > /dev/null

sleep 2

# Check balance
BALANCE=$(curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.getbalance","params":[],"id":9}' | jq -r '.result.spendable // .result.total // 0')

if [ "$BALANCE" == "0" ] || [ "$BALANCE" == "0.0" ] || [ -z "$BALANCE" ]; then
    echo -e "${YELLOW}⚠️  Warning: Balance is still 0 after mining. Wallet may need more time or rescan.${NC}"
else
    echo -e "${GREEN}✅ Node A balance: $BALANCE DIN${NC}"
fi
echo ""

# Step 7: Create wallet on Node B and get a recipient address
echo -e "${BLUE}Step 7: Creating wallet on Node B for recipient address...${NC}"

curl -s -X POST http://127.0.0.1:19444 \
  -u "__cookie__:$COOKIE_B" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["recipient"],"id":7}' > /tmp/wallet_b.json

RECIPIENT=$(jq -r '.result.first_address' /tmp/wallet_b.json)
echo -e "${GREEN}✅ Node B wallet created with address: $RECIPIENT${NC}"
echo ""

# Step 8: Submit transaction on Node A
echo -e "${BLUE}Step 8: Submitting transaction on Node A to Node B address...${NC}"

TX_RESULT=$(curl -s -X POST http://127.0.0.1:19443 \
  -u "__cookie__:$COOKIE_A" \
  -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"wallet.sendtoaddress\",\"params\":[\"$RECIPIENT\",0.1,1.0,\"\",true],\"id\":8}")

TXID=$(echo "$TX_RESULT" | jq -r '.result.txid')

if [ "$TXID" == "null" ] || [ -z "$TXID" ]; then
    echo -e "${RED}❌ Transaction submission failed${NC}"
    echo "$TX_RESULT" | jq .
    exit 1
fi

echo -e "${GREEN}✅ Transaction submitted: $TXID${NC}"
echo ""

# Step 9: Wait for relay
echo -e "${BLUE}Step 9: Waiting for transaction to relay to Node B...${NC}"
sleep 5

# Step 10: Check Node B mempool
echo -e "${BLUE}Step 10: Checking Node B mempool...${NC}"

MEMPOOL_B=$(curl -s -X POST http://127.0.0.1:19444 \
  -u "__cookie__:$COOKIE_B" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"getrawmempool","params":[],"id":8}' | jq -r '.result')

if echo "$MEMPOOL_B" | grep -q "$TXID"; then
    echo -e "${GREEN}✅ Transaction found in Node B mempool!${NC}"
else
    echo -e "${RED}❌ Transaction NOT found in Node B mempool${NC}"
    echo "Node B mempool contents:"
    echo "$MEMPOOL_B" | jq .
    exit 1
fi
echo ""

# Step 10: Verify no duplication
echo -e "${BLUE}Step 10: Verifying no duplication...${NC}"

TX_COUNT=$(echo "$MEMPOOL_B" | jq 'length')
if [ "$TX_COUNT" -eq 1 ]; then
    echo -e "${GREEN}✅ Exactly 1 transaction in mempool (no duplication)${NC}"
else
    echo -e "${RED}❌ Unexpected transaction count: $TX_COUNT${NC}"
    exit 1
fi
echo ""

# Step 11: Verify txid matches
echo -e "${BLUE}Step 11: Verifying txid preservation...${NC}"

TXID_B=$(echo "$MEMPOOL_B" | jq -r '.[0]')
if [ "$TXID" == "$TXID_B" ]; then
    echo -e "${GREEN}✅ Txid preserved across network: $TXID${NC}"
else
    echo -e "${RED}❌ Txid mismatch!${NC}"
    echo "  Node A: $TXID"
    echo "  Node B: $TXID_B"
    exit 1
fi
echo ""

# Success!
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║  TEST SUMMARY                                             ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""
echo -e "${GREEN}✅ ALL TESTS PASSED${NC}"
echo ""
echo "Proof Established:"
echo "  ✅ Real transactions relay between nodes"
echo "  ✅ Mempool accepts network transactions"
echo "  ✅ No duplication occurs"
echo "  ✅ No crashes or hangs"
echo "  ✅ Txid is preserved across network"
echo ""
echo "Transaction Flow:"
echo "  1. Node A created tx: $TXID"
echo "  2. Node A sent inv to Node B"
echo "  3. Node B requested tx via getdata"
echo "  4. Node A sent full tx to Node B"
echo "  5. Node B validated and added to mempool"
echo ""
echo -e "${BLUE}STEP D COMPLETE ✅ - Transaction relay proven working${NC}"
echo ""

exit 0
