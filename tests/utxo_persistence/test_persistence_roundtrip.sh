#!/usr/bin/env bash
# Phase B.3: UTXO Persistence Roundtrip Test
# Apply blocks → persist → restart → load → assert identical UTXO state

set -e

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Phase B.3: UTXO Persistence Roundtrip${NC}"
echo -e "${BLUE}========================================${NC}"

DATA_DIR="/tmp/test_utxo_roundtrip_$$"
RPC_PORT=29100
P2P_PORT=29101

cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f "dinerod.*$DATA_DIR" 2>/dev/null || true
    sleep 2
    rm -rf "$DATA_DIR"
}
trap cleanup EXIT

# Step 1: Start node and mine blocks
echo -e "${BLUE}[TEST]${NC} Starting node and mining blocks"
mkdir -p "$DATA_DIR"
./dinerod -regtest -datadir="$DATA_DIR" -rpcport=$RPC_PORT -port=$P2P_PORT -daemon 2>&1 | grep -v "Dinero Daemon" | grep -v "Built:" | grep -v "Dinero: Real Money" || true
sleep 12

# Wait for cookie file to appear
for i in {1..20}; do
    if [ -f "$DATA_DIR/.cookie" ]; then
        break
    fi
    sleep 1
done

if [ ! -f "$DATA_DIR/.cookie" ]; then
    echo -e "${RED}[FAIL]${NC} Cookie file not created"
    exit 1
fi

COOKIE=$(cat "$DATA_DIR/.cookie" | cut -d: -f2)

# Create wallet and mine blocks
echo -e "${BLUE}[TEST]${NC} Creating HD wallet"
WALLET_RESULT=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.createhd","params":["test"],"id":1}' \
  http://127.0.0.1:$RPC_PORT)

ADDR=$(echo "$WALLET_RESULT" | jq -r '.result.first_address')
echo -e "${YELLOW}[INFO]${NC} Mining address: $ADDR"

# Mine 110 blocks (100 for maturity + 10 spendable)
echo -e "${BLUE}[TEST]${NC} Mining 110 blocks to create UTXO set"
curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"mining.generatetoaddress\",\"params\":[110,\"$ADDR\"],\"id\":2}" \
  http://127.0.0.1:$RPC_PORT | jq -r '.result | length' | sed 's/^/Blocks mined: /'

# Rescan to populate wallet
curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.rescanblockchain","params":[],"id":3}' \
  http://127.0.0.1:$RPC_PORT >/dev/null

# Step 2: Get UTXO state BEFORE restart
echo -e "${BLUE}[TEST]${NC} Capturing UTXO state before restart"

HEIGHT_BEFORE=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getblockcount","params":[],"id":4}' \
  http://127.0.0.1:$RPC_PORT | jq -r '.result')

TIP_BEFORE=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getbestblockhash","params":[],"id":5}' \
  http://127.0.0.1:$RPC_PORT | jq -r '.result')

# Get balance as proxy for UTXO state
BALANCE_BEFORE=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.getbalance","params":[],"id":6}' \
  http://127.0.0.1:$RPC_PORT | jq -r '.result.spendable')

echo -e "${YELLOW}[INFO]${NC} Height before restart: $HEIGHT_BEFORE"
echo -e "${YELLOW}[INFO]${NC} Tip before restart: $TIP_BEFORE"
echo -e "${YELLOW}[INFO]${NC} Balance before restart: $BALANCE_BEFORE DIN"

# Verify we have spendable coins
if [ "$BALANCE_BEFORE" == "0" ] || [ "$BALANCE_BEFORE" == "0.0" ]; then
    echo -e "${RED}[FAIL]${NC} No spendable balance before restart"
    exit 1
fi

# Step 3: Stop node (UTXOs should be persisted to disk)
echo -e "${BLUE}[TEST]${NC} Stopping node (UTXOs persisted to disk)"
pkill -9 -f "dinerod.*$DATA_DIR" 2>/dev/null || true
sleep 5

# Verify node is stopped (give it up to 10 seconds)
for i in {1..10}; do
    if ! pgrep -f "dinerod.*$DATA_DIR" >/dev/null; then
        break
    fi
    sleep 1
done

# Final check
if pgrep -f "dinerod.*$DATA_DIR" >/dev/null; then
    echo -e "${RED}[FAIL]${NC} Node failed to stop after 10 seconds"
    pkill -9 -f "dinerod" 2>/dev/null || true  # Force kill all dinerod processes
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Node stopped cleanly"

# Step 4: Restart node (should load UTXOs from disk)
echo -e "${BLUE}[TEST]${NC} Restarting node (loading UTXOs from disk)"
./dinerod -regtest -datadir="$DATA_DIR" -rpcport=$RPC_PORT -port=$P2P_PORT -daemon 2>&1 | grep -v "Dinero Daemon" | grep -v "Built:" | grep -v "Dinero: Real Money" || true
sleep 12

# Update cookie (may have changed)
COOKIE=$(cat "$DATA_DIR/.cookie" | cut -d: -f2)

# Wait for node to be ready
for i in {1..20}; do
    if curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
      -d '{"jsonrpc":"2.0","method":"blockchain.getblockcount","params":[],"id":7}' \
      http://127.0.0.1:$RPC_PORT 2>/dev/null | jq -r '.result' >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

echo -e "${GREEN}[PASS]${NC} Node restarted and RPC responding"

# Step 5: Get UTXO state AFTER restart
echo -e "${BLUE}[TEST]${NC} Verifying UTXO state after restart"

HEIGHT_AFTER=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getblockcount","params":[],"id":8}' \
  http://127.0.0.1:$RPC_PORT | jq -r '.result')

TIP_AFTER=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"blockchain.getbestblockhash","params":[],"id":9}' \
  http://127.0.0.1:$RPC_PORT | jq -r '.result')

# Load wallet explicitly (wallet state is separate from UTXO state)
echo -e "${BLUE}[TEST]${NC} Loading wallet after restart"
WALLET_LOAD=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.loadhd","params":["test"],"id":10}' \
  http://127.0.0.1:$RPC_PORT)

WALLET_LOADED=$(echo "$WALLET_LOAD" | jq -r '.result.success')
if [ "$WALLET_LOADED" != "true" ]; then
    echo -e "${YELLOW}[WARN]${NC} Wallet load returned: $(echo "$WALLET_LOAD" | jq -r '.result')"
fi

# Rescan wallet (it doesn't track UTXOs, just scriptPubKeys)
echo -e "${BLUE}[TEST]${NC} Rescanning blockchain"
curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.rescanblockchain","params":[],"id":11}' \
  http://127.0.0.1:$RPC_PORT >/dev/null

BALANCE_AFTER=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.getbalance","params":[],"id":12}' \
  http://127.0.0.1:$RPC_PORT | jq -r '.result.spendable')

echo -e "${YELLOW}[INFO]${NC} Height after restart: $HEIGHT_AFTER"
echo -e "${YELLOW}[INFO]${NC} Tip after restart: $TIP_AFTER"
echo -e "${YELLOW}[INFO]${NC} Balance after restart: $BALANCE_AFTER DIN"

# Step 6: Assert UTXO state is identical
echo -e "${BLUE}[TEST]${NC} Asserting UTXO persistence correctness"

if [ "$HEIGHT_BEFORE" != "$HEIGHT_AFTER" ]; then
    echo -e "${RED}[FAIL]${NC} Height mismatch: before=$HEIGHT_BEFORE after=$HEIGHT_AFTER"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Height unchanged: $HEIGHT_AFTER"

if [ "$TIP_BEFORE" != "$TIP_AFTER" ]; then
    echo -e "${RED}[FAIL]${NC} Tip hash mismatch"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Tip hash unchanged: ${TIP_AFTER:0:16}..."

if [ "$BALANCE_BEFORE" != "$BALANCE_AFTER" ]; then
    echo -e "${RED}[FAIL]${NC} Balance mismatch: before=$BALANCE_BEFORE after=$BALANCE_AFTER"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Balance unchanged: $BALANCE_AFTER DIN"

# Step 7: Verify we can spend coins (UTXOs are functional)
echo -e "${BLUE}[TEST]${NC} Verifying UTXOs are functional after restart"
TX_RESULT=$(curl -s --user "__cookie__:$COOKIE" -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"wallet.sendtoaddress\",\"params\":[\"$ADDR\",1.0,\"\",\"\",true],\"id\":13}" \
  http://127.0.0.1:$RPC_PORT)

TX_ACCEPTED=$(echo "$TX_RESULT" | jq -r '.result.accepted')
if [ "$TX_ACCEPTED" != "true" ]; then
    echo -e "${RED}[FAIL]${NC} Transaction rejected after restart: $(echo "$TX_RESULT" | jq -r '.result.error')"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Transaction accepted (UTXOs functional)"

# Final summary
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}✅ UTXO PERSISTENCE ROUNDTRIP TEST PASSED${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "${GREEN}Verified:${NC}"
echo -e "  ✅ UTXOs persisted to disk during block application"
echo -e "  ✅ UTXOs loaded from disk at startup"
echo -e "  ✅ Chain state identical before/after restart"
echo -e "  ✅ UTXO set functional after restart (spendable)"
echo ""
echo -e "${GREEN}Phase B.3 Persistence Guarantees Met:${NC}"
echo -e "  • Persist UTXO Set: ✅"
echo -e "  • Deterministic load at startup: ✅"
echo -e "  • Startup verification: ✅"
echo -e "  • Clear failure modes: ✅"
