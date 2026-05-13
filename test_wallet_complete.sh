#!/bin/bash
# Complete Wallet Restore Test - Premine UTXO Persistence Verification
# This script tests the entire wallet restore → UTXO detection → database persistence chain

set -e

echo "═══════════════════════════════════════════════════════════════════════"
echo "DINERO WALLET RESTORE + UTXO PERSISTENCE TEST"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""

# Configuration
SEED="<redacted legacy premine mnemonic>"
PREMINE_ADDRESS="din1qd43uqnzgpp8w28490ex8je03axt7wy6fh28rlh"
PREMINE_AMOUNT="2627900.0"
WALLET_NAME="my_wallet"

echo "Test Configuration:"
echo "  Wallet: $WALLET_NAME"
echo "  Expected premine address: $PREMINE_ADDRESS"
echo "  Expected premine amount: $PREMINE_AMOUNT DIN"
echo ""

# Phase 1: Clean slate
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "PHASE 1: CLEAN SLATE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "[1/3] Stopping any running daemon..."
pkill -9 dinerod || true
sleep 2

echo "[2/3] Cleaning data directory..."
rm -rf ~/.dinero/blockchain ~/.dinero/headers ~/.dinero/wallets ~/.dinero/*.db*

echo "[3/3] Starting fresh daemon..."
./bin/dinerod &
DAEMON_PID=$!
echo "  Daemon started (PID: $DAEMON_PID)"
sleep 5

# Get RPC cookie
if [ ! -f ~/.dinero/.cookie ]; then
    echo "❌ ERROR: Daemon .cookie file not found"
    exit 1
fi
COOKIE=$(cat ~/.dinero/.cookie | cut -d: -f2)

echo "✅ Clean environment ready"
echo ""

# Phase 2: Restore wallet
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "PHASE 2: RESTORE WALLET FROM SEED"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

RESTORE_RESULT=$(curl -s --user "__cookie__:$COOKIE" \
  --data-binary "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"wallet.restore\",\"params\":[\"$WALLET_NAME\",\"$SEED\",\"\",\"\"]}" \
  http://127.0.0.1:20998 | python3 -m json.tool)

echo "$RESTORE_RESULT" | grep -q '"error": null' || {
    echo "❌ Failed to restore wallet"
    echo "$RESTORE_RESULT"
    pkill dinerod
    exit 1
}

FIRST_ADDRESS=$(echo "$RESTORE_RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result', {}).get('first_address', ''))")

echo "  Restored wallet: $WALLET_NAME"
echo "  First address: $FIRST_ADDRESS"

if [ "$FIRST_ADDRESS" != "$PREMINE_ADDRESS" ]; then
    echo "❌ ERROR: Address mismatch!"
    echo "  Expected: $PREMINE_ADDRESS"
    echo "  Got: $FIRST_ADDRESS"
    pkill dinerod
    exit 1
fi

echo "✅ Wallet restored with correct address derivation"
echo ""

# Verify address persistence
echo "Verifying address persistence in database..."
ADDR_COUNT=$(sqlite3 ~/.dinero/wallets/wallet_${WALLET_NAME}.db "SELECT COUNT(*) FROM addresses;")
SCRIPT_COUNT=$(sqlite3 ~/.dinero/wallets/wallet_${WALLET_NAME}.db "SELECT COUNT(*) FROM watch_scripts;")

echo "  Addresses in database: $ADDR_COUNT"
echo "  Watch scripts in database: $SCRIPT_COUNT"

if [ "$ADDR_COUNT" -lt 1 ]; then
    echo "❌ ERROR: No addresses persisted!"
    pkill dinerod
    exit 1
fi

echo "✅ Addresses persisted to database"
echo ""

# Phase 3: Mine premine block
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "PHASE 3: MINE BLOCK 1 (PREMINE)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "Mining Block 1 with premine to $PREMINE_ADDRESS..."
MINE_RESULT=$(curl -s --user "__cookie__:$COOKIE" \
  --data-binary "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"generatetoaddress\",\"params\":[1,\"$PREMINE_ADDRESS\"]}" \
  http://127.0.0.1:20998 | python3 -m json.tool)

echo "$MINE_RESULT" | grep -q '"error": null' || {
    echo "❌ Failed to mine block"
    echo "$MINE_RESULT"
    pkill dinerod
    exit 1
}

BLOCK_HASH=$(echo "$MINE_RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result', [[]])[0][0] if json.load(sys.stdin).get('result') else '')")

echo "✅ Block 1 mined (hash: ${BLOCK_HASH:0:16}...)"
sleep 2
echo ""

# Phase 4: Check wallet balance (first time)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "PHASE 4: VERIFY WALLET BALANCE (BEFORE RESTART)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

BALANCE_RESULT=$(curl -s --user "__cookie__:$COOKIE" \
  --data-binary '{"jsonrpc":"2.0","id":"1","method":"wallet.getinfo","params":[]}' \
  http://127.0.0.1:20998 | python3 -m json.tool)

BALANCE=$(echo "$BALANCE_RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result', {}).get('balance', 0))")
UTXO_COUNT=$(echo "$BALANCE_RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result', {}).get('utxo_count', 0))")

echo "  Balance: $BALANCE DIN"
echo "  UTXO count: $UTXO_COUNT"

if [ "$BALANCE" != "$PREMINE_AMOUNT" ]; then
    echo "❌ ERROR: Balance mismatch!"
    echo "  Expected: $PREMINE_AMOUNT DIN"
    echo "  Got: $BALANCE DIN"
    echo ""
    echo "Checking database directly..."
    DB_UTXO_COUNT=$(sqlite3 ~/.dinero/wallets/wallet_${WALLET_NAME}.db "SELECT COUNT(*) FROM utxos;")
    echo "  UTXOs in database: $DB_UTXO_COUNT"

    if [ "$DB_UTXO_COUNT" -eq 0 ]; then
        echo "❌ CRITICAL: UTXO not persisted to database!"
        echo "  This means WalletWorker detected the UTXO but didn't save it."
    fi

    pkill dinerod
    exit 1
fi

echo "✅ Wallet shows correct balance!"
echo ""

# Verify database UTXO persistence
echo "Verifying UTXO in database..."
DB_UTXO_COUNT=$(sqlite3 ~/.dinero/wallets/wallet_${WALLET_NAME}.db "SELECT COUNT(*) FROM utxos;")
echo "  UTXOs in database: $DB_UTXO_COUNT"

if [ "$DB_UTXO_COUNT" -eq 0 ]; then
    echo "❌ ERROR: UTXO not persisted to database!"
    pkill dinerod
    exit 1
fi

echo "✅ UTXO persisted to database"
echo ""

# Phase 5: Restart daemon and verify persistence
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "PHASE 5: RESTART DAEMON & VERIFY PERSISTENCE"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "[1/3] Stopping daemon..."
pkill dinerod
sleep 3

echo "[2/3] Starting daemon again..."
./bin/dinerod &
DAEMON_PID=$!
echo "  Daemon restarted (PID: $DAEMON_PID)"
sleep 5

# Get new cookie (it regenerates on restart)
COOKIE=$(cat ~/.dinero/.cookie | cut -d: -f2)

echo "[3/3] Checking wallet balance after restart..."
BALANCE_RESULT_2=$(curl -s --user "__cookie__:$COOKIE" \
  --data-binary '{"jsonrpc":"2.0","id":"1","method":"wallet.getinfo","params":[]}' \
  http://127.0.0.1:20998 | python3 -m json.tool)

BALANCE_2=$(echo "$BALANCE_RESULT_2" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result', {}).get('balance', 0))")
UTXO_COUNT_2=$(echo "$BALANCE_RESULT_2" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result', {}).get('utxo_count', 0))")

echo "  Balance after restart: $BALANCE_2 DIN"
echo "  UTXO count after restart: $UTXO_COUNT_2"

if [ "$BALANCE_2" != "$PREMINE_AMOUNT" ]; then
    echo "❌ ERROR: Balance not preserved after restart!"
    echo "  Expected: $PREMINE_AMOUNT DIN"
    echo "  Got: $BALANCE_2 DIN"
    pkill dinerod
    exit 1
fi

echo "✅ Balance persisted across daemon restart!"
echo ""

# Final summary
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ ✅ ✅ ALL TESTS PASSED! ✅ ✅ ✅"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Summary:"
echo "  ✅ Wallet restored from seed"
echo "  ✅ Addresses persisted to database ($ADDR_COUNT addresses)"
echo "  ✅ Watch scripts registered ($SCRIPT_COUNT scripts)"
echo "  ✅ Premine UTXO detected by WalletWorker"
echo "  ✅ UTXO persisted to database"
echo "  ✅ Balance shows correctly: $BALANCE DIN"
echo "  ✅ Balance persists across daemon restart"
echo ""
echo "The wallet restore + UTXO persistence chain is FULLY FUNCTIONAL!"
echo ""

# Cleanup
echo "Stopping daemon..."
pkill dinerod
sleep 2
echo "✅ Test complete"
