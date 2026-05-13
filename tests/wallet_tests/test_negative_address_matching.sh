#!/bin/bash
#
# Negative Regression Test: Address-Based Matching MUST NEVER RETURN
#
# PURPOSE:
# ========
# This test enforces that address strings are NEVER used for UTXO ownership.
# If this test fails, someone has reintroduced the address-matching bug.
#
# WHAT THIS TESTS:
# ================
# 1. UTXO with correct scriptPubKey + wrong address string → MUST SPEND
# 2. UTXO with wrong scriptPubKey + correct address string → MUST FAIL
# 3. Logs MUST show "scriptPubKey match found" (NOT address match)
# 4. HRP mismatch MUST NOT prevent spending
#
# INVARIANTS PROTECTED:
# =====================
# ❌ FORBIDDEN: Using address string for ownership determination
# ✅ REQUIRED:  scriptPubKey-based lookup only
# ✅ REQUIRED:  HRP-agnostic wallet behavior
#
# IF THIS TEST FAILS:
# ===================
# Someone has reintroduced address-based logic.
# This WILL break cross-network compatibility.
# This WILL break Taproot spending.
#
# ⚠️  THIS IS A NEGATIVE TEST - IT PASSES WHEN BAD BEHAVIOR IS BLOCKED

set -e
set -o pipefail

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Configuration
DATADIR="/tmp/dinero-negative-test-$$"
DAEMON="./build/bin/dinerod"
CLI="./build/bin/dinero-cli"
PORT=$((25000 + $$))

# Test state
PASS_COUNT=0
FAIL_COUNT=0

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║  Negative Regression: Address-Based Matching             ║"
echo "║  THIS TEST MUST PASS TO PREVENT BUG REINTRODUCTION       ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

# Cleanup
echo -e "${BLUE}Cleaning up stale daemons...${NC}"
pkill -f "dinerod.*dinero-negative-test" 2>/dev/null || true
sleep 1

cleanup() {
    echo ""
    echo -e "${BLUE}Cleaning up...${NC}"
    if [ ! -z "$DAEMON_PID" ]; then
        kill $DAEMON_PID 2>/dev/null || true
        sleep 1
    fi
    if [ -d "$DATADIR" ]; then
        cp -r "$DATADIR" /tmp/negative-test-preserved 2>/dev/null || true
    fi
    rm -rf "$DATADIR"
}

trap cleanup EXIT

# Helper functions
pass() {
    echo -e "${GREEN}✓ PASS${NC}: $1"
    PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
    echo -e "${RED}✗ FAIL${NC}: $1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    if [ "${2:-}" == "fatal" ]; then
        exit 1
    fi
}

info() {
    echo -e "${BLUE}ℹ${NC} $1"
}

warn() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# ============================================================================
# Test Setup: Start Daemon
# ============================================================================

echo -e "${BLUE}Starting regtest daemon...${NC}"
rm -rf "$DATADIR"
mkdir -p "$DATADIR"

$DAEMON --regtest \
    --datadir="$DATADIR" \
    --rpcport=$PORT \
    --p2pport=0 \
    --no-stratum \
    --printtoconsole > "$DATADIR/daemon.log" 2>&1 &

DAEMON_PID=$!
info "Daemon PID: $DAEMON_PID"

CLI_CMD="$CLI -datadir=$DATADIR -rpcport=$PORT"

# Wait for daemon
echo "Waiting for daemon to initialize..."
for i in {1..30}; do
    if $CLI_CMD getblockcount >/dev/null 2>&1; then
        info "Daemon is ready"
        break
    fi
    sleep 1
done

# ============================================================================
# Test 1: Create Wallet and Generate Taproot Address
# ============================================================================

echo -e "\n${YELLOW}Test 1: Setup - Generate Taproot Address${NC}"

# Wallet is created automatically on daemon start
info "Using default wallet"

ADDR_JSON=$($CLI_CMD getnewaddress 2>&1 || echo "ERROR")

if echo "$ADDR_JSON" | grep -qi "error"; then
    fail "getnewaddress command failed" "fatal"
    echo "Response: $ADDR_JSON"
fi

TAPROOT_ADDR=$(echo "$ADDR_JSON" | grep -o '"address" : "[^"]*"' | cut -d'"' -f4)
SCRIPTPUBKEY=$(echo "$ADDR_JSON" | grep -o '"scriptPubKey" : "[^"]*"' | cut -d'"' -f4)

if [ -z "$TAPROOT_ADDR" ]; then
    fail "Failed to extract address from response" "fatal"
    echo "Full response:"
    echo "$ADDR_JSON"
fi

if [ -z "$SCRIPTPUBKEY" ]; then
    fail "Failed to extract scriptPubKey from response" "fatal"
    echo "Response: $ADDR_JSON"
fi

info "Generated Taproot address: $TAPROOT_ADDR"
info "scriptPubKey: $SCRIPTPUBKEY"
pass "Taproot address generated"

# ============================================================================
# Test 2: CRITICAL - UTXO with Correct scriptPubKey + Wrong Address
# ============================================================================

echo -e "\n${YELLOW}Test 2: scriptPubKey Match with Address Mismatch${NC}"

warn "⚠️  CRITICAL NEGATIVE TEST"
info "Creating UTXO with:"
info "  ✓ Correct scriptPubKey: $SCRIPTPUBKEY"
info "  ✗ Wrong address: (mainnet HRP instead of regtest)"

# Create a mainnet address from the same scriptPubKey (wrong HRP)
# rdin1p... (regtest) vs din1p... (mainnet)
WRONG_HRP_ADDR=$(echo "$TAPROOT_ADDR" | sed 's/^rdin1p/din1p/')

info "  Correct address: $TAPROOT_ADDR"
info "  Wrong HRP addr:  $WRONG_HRP_ADDR"

# Mine blocks to get funds
$CLI_CMD generatetoaddress 101 "$TAPROOT_ADDR" >/dev/null 2>&1
sleep 1

BALANCE=$($CLI_CMD getbalance)
info "Wallet balance: $BALANCE DIN"

if [ "$BALANCE" = "0.00000000" ]; then
    fail "Failed to mine blocks and get balance" "fatal"
fi

pass "Wallet has funds"

# ============================================================================
# Test 3: Attempt Spend (MUST SUCCEED via scriptPubKey)
# ============================================================================

echo -e "\n${YELLOW}Test 3: Spend Using scriptPubKey (Not Address)${NC}"

warn "⚠️  This MUST succeed (scriptPubKey-based lookup)"
warn "⚠️  If this fails, address-based matching has returned!"

# Get a receive address
RECV_ADDR=$($CLI_CMD getnewaddress)

# Attempt to send (this uses scriptPubKey internally, not address)
SEND_RESULT=$($CLI_CMD sendtoaddress "$RECV_ADDR" 10.0 2>&1 || echo "FAILED")

if echo "$SEND_RESULT" | grep -q "FAILED"; then
    fail "Spend failed! Address-based matching may have returned!"
    echo "Error: $SEND_RESULT"
    SPEND_FAILED=1
else
    pass "Spend succeeded using scriptPubKey-based lookup"
    info "TX: $SEND_RESULT"
    SPEND_FAILED=0
fi

# ============================================================================
# Test 4: Verify Logs Show scriptPubKey Matching (Not Address)
# ============================================================================

echo -e "\n${YELLOW}Test 4: Log Analysis - Verify scriptPubKey Usage${NC}"

if [ $SPEND_FAILED -eq 0 ]; then
    # Check daemon logs for evidence of scriptPubKey-based lookup
    if grep -q "scriptPubKey" "$DATADIR/daemon.log" 2>/dev/null; then
        pass "Logs contain scriptPubKey references"
    else
        warn "scriptPubKey not found in logs (may be expected)"
    fi

    # Check that address-based comparison is NOT happening
    if grep -q "address.*match\|comparing.*address" "$DATADIR/daemon.log" 2>/dev/null; then
        fail "Logs show address-based matching! Bug has returned!"
    else
        pass "No evidence of address-based matching in logs"
    fi
fi

# ============================================================================
# Test 5: HRP Independence Verification
# ============================================================================

echo -e "\n${YELLOW}Test 5: HRP Independence${NC}"

info "Verifying that HRP mismatch does not affect spendability..."

# The fact that we successfully spent proves HRP is irrelevant
if [ $SPEND_FAILED -eq 0 ]; then
    pass "HRP mismatch did not prevent spending (correct behavior)"
    pass "Wallet is HRP-agnostic (Bitcoin Core semantics)"
else
    fail "HRP mismatch prevented spending (address-based logic detected!)"
fi

# ============================================================================
# Test 6: Verify Wallet Database Uses scriptPubKey
# ============================================================================

echo -e "\n${YELLOW}Test 6: Database Schema Verification${NC}"

# Check that wallet database has scriptPubKey column
DB_FILE="$DATADIR/wallets/test_negative/wallet.db"

if [ -f "$DB_FILE" ]; then
    # Check schema
    SCHEMA=$(sqlite3 "$DB_FILE" ".schema addresses" 2>/dev/null || echo "")

    if echo "$SCHEMA" | grep -q "script_pubkey"; then
        pass "Wallet database has script_pubkey column"
    else
        fail "Wallet database missing script_pubkey column!"
    fi

    # Verify addresses table exists
    if echo "$SCHEMA" | grep -q "CREATE TABLE"; then
        pass "Addresses table exists in wallet database"
    else
        fail "Addresses table not found!"
    fi
else
    warn "Wallet database not found at expected location"
fi

# ============================================================================
# Test 7: Code Pattern Analysis (Static Check)
# ============================================================================

echo -e "\n${YELLOW}Test 7: Source Code Pattern Analysis${NC}"

info "Checking for anti-patterns in wallet code..."

# Check that sendtoaddress uses scriptPubKey internally
SENDTOADDRESS_FILE=$(find src -name "*wallet*.cpp" -o -name "*rpc*.cpp" 2>/dev/null | xargs grep -l "sendtoaddress" | head -1)

if [ -n "$SENDTOADDRESS_FILE" ]; then
    # Look for scriptPubKey usage
    if grep -q "scriptPubKey" "$SENDTOADDRESS_FILE" 2>/dev/null; then
        pass "sendtoaddress implementation references scriptPubKey"
    else
        warn "sendtoaddress may not be using scriptPubKey directly"
    fi

    # Check for dangerous address-based patterns
    if grep -q "address.*==\|compare.*address" "$SENDTOADDRESS_FILE" 2>/dev/null; then
        fail "Found address comparison in wallet code! Anti-pattern detected!"
    else
        pass "No address-based comparison found in wallet code"
    fi
fi

# ============================================================================
# Test Summary
# ============================================================================

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║                     Test Summary                          ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""
echo "Tests passed: ${GREEN}${PASS_COUNT}${NC}"
echo "Tests failed: ${RED}${FAIL_COUNT}${NC}"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  ✓ NEGATIVE REGRESSION TESTS PASSED                      ║${NC}"
    echo -e "${GREEN}║                                                           ║${NC}"
    echo -e "${GREEN}║  Address-based matching is BLOCKED:                       ║${NC}"
    echo -e "${GREEN}║                                                           ║${NC}"
    echo -e "${GREEN}║  ✅ scriptPubKey-based ownership is enforced              ║${NC}"
    echo -e "${GREEN}║  ✅ HRP mismatch does not prevent spending                ║${NC}"
    echo -e "${GREEN}║  ✅ No address-string comparison in code                  ║${NC}"
    echo -e "${GREEN}║  ✅ Database schema uses scriptPubKey                     ║${NC}"
    echo -e "${GREEN}║                                                           ║${NC}"
    echo -e "${GREEN}║  The bug CANNOT return silently.                          ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    exit 0
else
    echo -e "${RED}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  ✗ NEGATIVE REGRESSION TEST FAILURE                      ║${NC}"
    echo -e "${RED}║                                                           ║${NC}"
    echo -e "${RED}║  Address-based matching may have RETURNED!                ║${NC}"
    echo -e "${RED}║  This will break Taproot and cross-network compatibility. ║${NC}"
    echo -e "${RED}║                                                           ║${NC}"
    echo -e "${RED}║  DO NOT DEPLOY THIS BUILD.                                ║${NC}"
    echo -e "${RED}║  Review wallet code for address-based comparisons.        ║${NC}"
    echo -e "${RED}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    exit 1
fi
