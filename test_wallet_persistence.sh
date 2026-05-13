#!/bin/bash
# Test: Wallet Restore + UTXO Persistence + Daemon Restart
# This test verifies that UTXOs are properly persisted to the database

set -e

echo "═══════════════════════════════════════════════════════════════"
echo "  Dinero Wallet UTXO Persistence Test"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
DATADIR="$HOME/.dinero_test"
RPC_PORT="20998"
COOKIE_FILE="$DATADIR/.cookie"
PREMINE_MNEMONIC="<redacted legacy premine mnemonic>"
PREMINE_ADDRESS="din1qd43uqnzgpp8w28490ex8je03axt7wy6fh28rlh"
PREMINE_AMOUNT="2627900.0"

# Helper functions
rpc_call() {
    local method=$1
    local params=$2

    # Read cookie for auth
    if [ ! -f "$COOKIE_FILE" ]; then
        echo -e "${RED}ERROR: Cookie file not found at $COOKIE_FILE${NC}"
        return 1
    fi

    local cookie=$(cat "$COOKIE_FILE")

    curl -s --user "__cookie__:$cookie" \
        --data-binary "{\"jsonrpc\":\"2.0\",\"id\":\"test\",\"method\":\"$method\",\"params\":$params}" \
        http://127.0.0.1:$RPC_PORT
}

wait_for_daemon() {
    echo -e "${BLUE}⏳ Waiting for daemon to start...${NC}"
    for i in {1..30}; do
        if rpc_call "getblockchaininfo" "[]" > /dev/null 2>&1; then
            echo -e "${GREEN}✅ Daemon is ready${NC}"
            return 0
        fi
        sleep 1
    done
    echo -e "${RED}❌ Daemon failed to start within 30 seconds${NC}"
    return 1
}

check_database() {
    local wallet_db="$DATADIR/wallets/wallet_my_wallet.db"

    if [ ! -f "$wallet_db" ]; then
        echo -e "${RED}❌ Wallet database not found: $wallet_db${NC}"
        return 1
    fi

    echo -e "${BLUE}📊 Checking database contents...${NC}"

    # Check addresses
    local addr_count=$(sqlite3 "$wallet_db" "SELECT COUNT(*) FROM addresses;" 2>/dev/null || echo "0")
    echo -e "   Addresses in DB: $addr_count"

    # Check watch_scripts
    local script_count=$(sqlite3 "$wallet_db" "SELECT COUNT(*) FROM watch_scripts;" 2>/dev/null || echo "0")
    echo -e "   Scripts in DB: $script_count"

    # Check UTXOs
    local utxo_count=$(sqlite3 "$wallet_db" "SELECT COUNT(*) FROM utxos;" 2>/dev/null || echo "0")
    echo -e "   UTXOs in DB: $utxo_count"

    if [ "$utxo_count" -gt 0 ]; then
        echo -e "${BLUE}   UTXO details:${NC}"
        sqlite3 "$wallet_db" "SELECT txid, vout, value_sats, height, is_coinbase FROM utxos;" 2>/dev/null | \
            while IFS='|' read -r txid vout value height coinbase; do
                echo -e "     - $txid:$vout = $(echo "scale=2; $value / 100000000" | bc) DIN (height $height, coinbase=$coinbase)"
            done
    fi

    return 0
}

# ═══════════════════════════════════════════════════════════════
# TEST START
# ═══════════════════════════════════════════════════════════════

echo -e "${YELLOW}Step 1: Clean slate${NC}"
pkill -9 dinerod 2>/dev/null || true
sleep 2
rm -rf "$DATADIR"
mkdir -p "$DATADIR"
echo -e "${GREEN}✅ Data directory cleaned${NC}"
echo ""

echo -e "${YELLOW}Step 2: Start daemon${NC}"
./bin/dinerod -datadir="$DATADIR" -daemon -regtest
if ! wait_for_daemon; then
    echo -e "${RED}❌ Test failed: Daemon did not start${NC}"
    exit 1
fi
echo ""

echo -e "${YELLOW}Step 3: Restore wallet with premine seed${NC}"
RESTORE_RESULT=$(rpc_call "wallet.restore" "[\"my_wallet\",\"$PREMINE_MNEMONIC\",\"\",\"\"]")
echo "$RESTORE_RESULT" | python3 -m json.tool
if echo "$RESTORE_RESULT" | grep -q "error"; then
    echo -e "${RED}❌ Test failed: wallet.restore returned error${NC}"
    pkill -9 dinerod
    exit 1
fi
echo -e "${GREEN}✅ Wallet restored${NC}"
echo ""

echo -e "${YELLOW}Step 4: Check addresses were persisted${NC}"
check_database
echo ""

echo -e "${YELLOW}Step 5: Mine Block 1 (contains premine)${NC}"
MINE_RESULT=$(rpc_call "generatetoaddress" "[1,\"$PREMINE_ADDRESS\"]")
echo "$MINE_RESULT" | python3 -m json.tool
echo -e "${GREEN}✅ Mined block 1${NC}"
echo ""

sleep 3  # Wait for block processing

echo -e "${YELLOW}Step 6: Check balance (before restart)${NC}"
BALANCE_BEFORE=$(rpc_call "wallet.getinfo" "[]")
echo "$BALANCE_BEFORE" | python3 -m json.tool

# Extract balance value
BALANCE_VALUE=$(echo "$BALANCE_BEFORE" | python3 -c "import sys, json; print(json.load(sys.stdin)['result']['balance'])")
echo -e "${BLUE}Balance: $BALANCE_VALUE DIN${NC}"

if [ "$BALANCE_VALUE" == "0" ] || [ "$BALANCE_VALUE" == "0.0" ]; then
    echo -e "${RED}❌ WARNING: Balance is 0 before restart!${NC}"
    echo -e "${YELLOW}   This might indicate the UTXO wasn't detected${NC}"
else
    echo -e "${GREEN}✅ Balance shows premine: $BALANCE_VALUE DIN${NC}"
fi
echo ""

echo -e "${YELLOW}Step 7: Check database BEFORE restart${NC}"
check_database
UTXO_COUNT_BEFORE=$(sqlite3 "$DATADIR/wallets/wallet_my_wallet.db" "SELECT COUNT(*) FROM utxos;" 2>/dev/null || echo "0")
echo ""

if [ "$UTXO_COUNT_BEFORE" -eq 0 ]; then
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${RED}❌ CRITICAL: No UTXOs in database before restart!${NC}"
    echo -e "${RED}   This means the persistence fix is not working.${NC}"
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "${YELLOW}Continuing with test to confirm failure after restart...${NC}"
    echo ""
fi

echo -e "${YELLOW}Step 8: Stop daemon${NC}"
rpc_call "stop" "[]"
sleep 5
echo -e "${GREEN}✅ Daemon stopped${NC}"
echo ""

echo -e "${YELLOW}Step 9: Restart daemon${NC}"
./bin/dinerod -datadir="$DATADIR" -daemon -regtest
if ! wait_for_daemon; then
    echo -e "${RED}❌ Test failed: Daemon did not restart${NC}"
    exit 1
fi
echo ""

sleep 3  # Wait for wallet initialization

echo -e "${YELLOW}Step 10: Check balance (after restart)${NC}"
BALANCE_AFTER=$(rpc_call "wallet.getinfo" "[]")
echo "$BALANCE_AFTER" | python3 -m json.tool

# Extract balance value
BALANCE_VALUE_AFTER=$(echo "$BALANCE_AFTER" | python3 -c "import sys, json; print(json.load(sys.stdin)['result']['balance'])")
echo -e "${BLUE}Balance after restart: $BALANCE_VALUE_AFTER DIN${NC}"
echo ""

echo -e "${YELLOW}Step 11: Check database AFTER restart${NC}"
check_database
UTXO_COUNT_AFTER=$(sqlite3 "$DATADIR/wallets/wallet_my_wallet.db" "SELECT COUNT(*) FROM utxos;" 2>/dev/null || echo "0")
echo ""

# ═══════════════════════════════════════════════════════════════
# TEST RESULTS
# ═══════════════════════════════════════════════════════════════

echo "═══════════════════════════════════════════════════════════════"
echo "  TEST RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo ""

echo "Before restart:"
echo "  - Balance: $BALANCE_VALUE DIN"
echo "  - UTXOs in DB: $UTXO_COUNT_BEFORE"
echo ""

echo "After restart:"
echo "  - Balance: $BALANCE_VALUE_AFTER DIN"
echo "  - UTXOs in DB: $UTXO_COUNT_AFTER"
echo ""

# Check if test passed
if [ "$BALANCE_VALUE" != "0" ] && [ "$BALANCE_VALUE" != "0.0" ] && \
   [ "$BALANCE_VALUE_AFTER" == "$BALANCE_VALUE" ] && \
   [ "$UTXO_COUNT_BEFORE" -gt 0 ] && \
   [ "$UTXO_COUNT_AFTER" -gt 0 ]; then
    echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}✅ ✅ ✅  TEST PASSED  ✅ ✅ ✅${NC}"
    echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "${GREEN}✅ Balance persists across restart${NC}"
    echo -e "${GREEN}✅ UTXOs are properly stored in database${NC}"
    echo -e "${GREEN}✅ Wallet restore + persistence is WORKING!${NC}"
    echo ""

    # Cleanup
    echo -e "${BLUE}Cleaning up...${NC}"
    rpc_call "stop" "[]"
    sleep 2

    exit 0
else
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${RED}❌ ❌ ❌  TEST FAILED  ❌ ❌ ❌${NC}"
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo ""

    if [ "$UTXO_COUNT_BEFORE" -eq 0 ]; then
        echo -e "${RED}❌ UTXOs were NOT persisted to database${NC}"
        echo -e "${YELLOW}   Check that:${NC}"
        echo -e "${YELLOW}   1. WalletManager pointer is passed to WalletWorker${NC}"
        echo -e "${YELLOW}   2. wallet_manager_->addUTXO() is called in ProcessConnect${NC}"
        echo -e "${YELLOW}   3. WalletManager::addUTXO() method exists and works${NC}"
    elif [ "$BALANCE_VALUE_AFTER" != "$BALANCE_VALUE" ]; then
        echo -e "${RED}❌ Balance changed after restart${NC}"
        echo -e "${YELLOW}   Before: $BALANCE_VALUE${NC}"
        echo -e "${YELLOW}   After:  $BALANCE_VALUE_AFTER${NC}"
    elif [ "$UTXO_COUNT_AFTER" -eq 0 ]; then
        echo -e "${RED}❌ UTXOs disappeared from database after restart${NC}"
    fi

    echo ""
    echo -e "${YELLOW}Daemon left running for debugging. Stop with:${NC}"
    echo -e "${YELLOW}  curl --user \"__cookie__:\$(cat $COOKIE_FILE)\" \\${NC}"
    echo -e "${YELLOW}    --data-binary '{\"method\":\"stop\"}' http://127.0.0.1:$RPC_PORT${NC}"
    echo ""

    exit 1
fi
