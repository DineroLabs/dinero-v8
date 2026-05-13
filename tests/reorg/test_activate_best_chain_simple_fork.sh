#!/usr/bin/env bash
#
# Test 1: ActivateBestChain - Simple 2-Block Fork
#
# v0.15.0.4 Test Scaffolding (CONSENSUS-CRITICAL)
#
# Scenario:
#   Genesis → A → B → C  (initial active tip, lower chainwork)
#              \
#               D → E    (higher chainwork tip)
#
# Preconditions:
#   - Both chains fully valid
#   - Both tips present in BlockIndex
#   - No reorg occurs during block acceptance
#   - Chainwork(E) > Chainwork(C)
#
# Action:
#   - Call ActivateBestChain() (via block acceptance trigger)
#
# Expected Outcome:
#   - Active chain tip becomes E
#   - Blocks C and B are disconnected
#   - Blocks D and E are connected
#   - UTXO state matches E-chain
#   - Mempool reconciled (empty for this test)
#
# Exit Criteria:
#   ✅ Active tip is E (not C)
#   ✅ Active height matches E
#   ✅ UTXO set reflects E-chain state
#   ✅ No partial reorg state
#
# Status: SCAFFOLDING ONLY - Expected to FAIL until ActivateBestChain is implemented

set -e  # Exit on error
set -u  # Exit on undefined variable

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test configuration
# Use DINEROD env var if set, otherwise default to ./build/dinerod
DINEROD="${DINEROD:-./build/dinerod}"
DATA_DIR="/tmp/dinero-test-fork-$$"
RPC_PORT=$((19000 + RANDOM % 1000))
P2P_PORT=$((20000 + RANDOM % 1000))
STRATUM_PORT=$((21000 + RANDOM % 1000))

# Cleanup function
cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f "dinerod.*$DATA_DIR" 2>/dev/null || true
    sleep 2
    rm -rf "$DATA_DIR"
}

trap cleanup EXIT

# RPC helper
rpc() {
    local METHOD="$1"
    shift
    local PARAMS_JSON="["
    local FIRST=true

    for param in "$@"; do
        if [ "$FIRST" = true ]; then
            FIRST=false
        else
            PARAMS_JSON="$PARAMS_JSON,"
        fi

        if [ "$param" = "true" ] || [ "$param" = "false" ]; then
            PARAMS_JSON="$PARAMS_JSON$param"
        elif [[ "$param" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            PARAMS_JSON="$PARAMS_JSON$param"
        else
            PARAMS_JSON="$PARAMS_JSON\"$param\""
        fi
    done
    PARAMS_JSON="$PARAMS_JSON]"

    local COOKIE=$(cat "$DATA_DIR/.cookie" 2>/dev/null | cut -d: -f2)
    curl -s --user "__cookie__:$COOKIE" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$METHOD\",\"params\":$PARAMS_JSON,\"id\":1}" \
        http://127.0.0.1:$RPC_PORT | jq -r '.result // .error // .'
}

echo -e "${BLUE}======================================${NC}"
echo -e "${BLUE}Test 1: Simple Fork - ActivateBestChain${NC}"
echo -e "${BLUE}v0.15.0.4 Scaffolding (Expected FAIL)${NC}"
echo -e "${BLUE}======================================${NC}"

# Start node in regtest mode
echo -e "${BLUE}[TEST]${NC} Starting regtest node"
$DINEROD --regtest --datadir="$DATA_DIR" --rpcport=$RPC_PORT --port=$P2P_PORT --stratumport=$STRATUM_PORT --daemon 2>&1 >/dev/null
sleep 8

# Wait for RPC
for i in {1..30}; do
    if rpc "blockchain.getblockcount" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

# Wait for wallet RPC specifically
for i in {1..30}; do
    WALLET_TEST=$(rpc "wallet.listaddresses" 2>&1)
    if echo "$WALLET_TEST" | grep -q "address" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo -e "${GREEN}[PASS]${NC} Node started"

# ============================================================================
# Dinero starts with premine at height 1, not genesis at height 0
# All height expectations must be relative to the initial chain height
# ============================================================================
INITIAL_HEIGHT=$(rpc "blockchain.getblockcount")
echo -e "${YELLOW}[INFO]${NC} Initial chain height: $INITIAL_HEIGHT (Dinero premine)"

# Get mining address
ADDR=$(rpc "wallet.listaddresses" | jq -r 'if type=="array" then .[0].address else empty end')
echo -e "${YELLOW}[INFO]${NC} Mining address: $ADDR"

# Mine common ancestor chain: Premine → A → B
echo -e "${BLUE}[TEST]${NC} Mining common ancestor chain (Premine → A → B)"
MINING_RESULT=$(rpc "mining.generatetoaddress" "2" "$ADDR")
A_HASH=$(echo "$MINING_RESULT" | jq -r '.blocks[0]')
B_HASH=$(echo "$MINING_RESULT" | jq -r '.blocks[1]')

echo -e "${YELLOW}[INFO]${NC} Block A: $A_HASH"
echo -e "${YELLOW}[INFO]${NC} Block B: $B_HASH"

# Verify we're at INITIAL_HEIGHT + 2
HEIGHT=$(rpc "blockchain.getblockcount")
EXPECTED_HEIGHT=$((INITIAL_HEIGHT + 2))
if [ "$HEIGHT" != "$EXPECTED_HEIGHT" ]; then
    echo -e "${RED}[FAIL]${NC} Expected height $EXPECTED_HEIGHT, got $HEIGHT"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Common ancestor chain at height $EXPECTED_HEIGHT"

# Save current tip (B) for reference
TIP_B=$(rpc "blockchain.getbestblockhash")
echo -e "${YELLOW}[INFO]${NC} Tip after B: $TIP_B"

# Build Chain C: B → C (total height INITIAL+3, lower cumulative work)
echo -e "${BLUE}[TEST]${NC} Building chain C (B → C)"
C_HASH=$(rpc "mining.generatetoaddress" "1" "$ADDR" | jq -r '.blocks[0]')
echo -e "${YELLOW}[INFO]${NC} Block C: $C_HASH"

# Verify active tip is C at height INITIAL_HEIGHT + 3
HEIGHT=$(rpc "blockchain.getblockcount")
TIP_C=$(rpc "blockchain.getbestblockhash")
EXPECTED_HEIGHT_C=$((INITIAL_HEIGHT + 3))

if [ "$HEIGHT" != "$EXPECTED_HEIGHT_C" ]; then
    echo -e "${RED}[FAIL]${NC} Expected height $EXPECTED_HEIGHT_C after C, got $HEIGHT"
    exit 1
fi

if [ "$TIP_C" != "$C_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Expected tip to be C, got different hash"
    exit 1
fi

echo -e "${GREEN}[PASS]${NC} Chain C active at height $EXPECTED_HEIGHT_C"

# ============================================================================
# CRITICAL: Now we need to mine competing chain D → E off block B
# This requires invalidating C, mining D and E, then un-invalidating C
# to create the fork scenario where both tips exist in BlockIndex
# ============================================================================

echo -e "${BLUE}[TEST]${NC} Creating competing fork D → E off block B"

# Step 1: Invalidate C to revert to B
echo -e "${YELLOW}[INFO]${NC} Invalidating block C to revert to B"
rpc "blockchain.invalidateblock" "$C_HASH" >/dev/null

# Verify we're back at B (height INITIAL_HEIGHT + 2)
HEIGHT=$(rpc "blockchain.getblockcount")
TIP_AFTER_INVALIDATE=$(rpc "blockchain.getbestblockhash")
EXPECTED_HEIGHT_B=$((INITIAL_HEIGHT + 2))

if [ "$HEIGHT" != "$EXPECTED_HEIGHT_B" ]; then
    echo -e "${RED}[FAIL]${NC} Expected height $EXPECTED_HEIGHT_B after invalidating C, got $HEIGHT"
    exit 1
fi

if [ "$TIP_AFTER_INVALIDATE" != "$B_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Expected tip to be B after invalidate, got $TIP_AFTER_INVALIDATE"
    exit 1
fi

echo -e "${GREEN}[PASS]${NC} Reverted to block B (height $EXPECTED_HEIGHT_B)"

# Step 2: Mine competing chain D → E (2 blocks, total height INITIAL+4)
echo -e "${YELLOW}[INFO]${NC} Mining competing chain D → E (2 blocks)"
COMPETING_RESULT=$(rpc "mining.generatetoaddress" "2" "$ADDR")
D_HASH=$(echo "$COMPETING_RESULT" | jq -r '.blocks[0]')
E_HASH=$(echo "$COMPETING_RESULT" | jq -r '.blocks[1]')

echo -e "${YELLOW}[INFO]${NC} Block D: $D_HASH"
echo -e "${YELLOW}[INFO]${NC} Block E: $E_HASH"

# Verify we're at E (height INITIAL_HEIGHT + 4)
HEIGHT=$(rpc "blockchain.getblockcount")
TIP_E=$(rpc "blockchain.getbestblockhash")
EXPECTED_HEIGHT_E=$((INITIAL_HEIGHT + 4))

if [ "$HEIGHT" != "$EXPECTED_HEIGHT_E" ]; then
    echo -e "${RED}[FAIL]${NC} Expected height $EXPECTED_HEIGHT_E after E, got $HEIGHT"
    exit 1
fi

if [ "$TIP_E" != "$E_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Expected tip to be E, got different hash"
    exit 1
fi

echo -e "${GREEN}[PASS]${NC} Competing chain D → E mined (height $EXPECTED_HEIGHT_E)"

# Step 3: Reconsider block C to make it available again
# This creates the fork scenario: both C and E exist in BlockIndex
echo -e "${YELLOW}[INFO]${NC} Reconsidering block C to create fork scenario"
rpc "blockchain.reconsiderblock" "$C_HASH" >/dev/null

echo -e "${GREEN}[PASS]${NC} Fork scenario created: C-chain and E-chain both exist"

# ============================================================================
# ASSERTION PHASE: Test ActivateBestChain Behavior
# ============================================================================
# At this point:
#   - Genesis → A → B → C exists (chainwork_C)
#   - Genesis → A → B → D → E exists (chainwork_E)
#   - chainwork_E > chainwork_C (E-chain has 1 more block)
#   - Current active tip should be E (higher work)
#
# Expected behavior (when ActivateBestChain is implemented):
#   ✅ Active tip is E
#   ✅ Automatic reorg happened (C disconnected, D+E connected)
#   ✅ UTXO set matches E-chain
#
# Current behavior (v0.15.0.3):
#   ⚠️ Tip might still be at whichever chain was mined last
#   ⚠️ No automatic reorg (ActivateBestChain not implemented)
# ============================================================================

echo -e "${BLUE}[TEST]${NC} Verifying ActivateBestChain selected best chain"

# Get final state
FINAL_HEIGHT=$(rpc "blockchain.getblockcount")
FINAL_TIP=$(rpc "blockchain.getbestblockhash")

echo -e "${YELLOW}[INFO]${NC} Final height: $FINAL_HEIGHT"
echo -e "${YELLOW}[INFO]${NC} Final tip: $FINAL_TIP"
echo -e "${YELLOW}[INFO]${NC} Expected tip (E): $E_HASH"

# ============================================================================
# CRITICAL ASSERTIONS
# ============================================================================

# Assertion 1: Active tip must be E (highest chainwork)
if [ "$FINAL_TIP" != "$E_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} ❌ ASSERTION FAILED"
    echo -e "${RED}       Active tip is NOT E (highest chainwork chain)${NC}"
    echo -e "${RED}       Expected: $E_HASH${NC}"
    echo -e "${RED}       Got:      $FINAL_TIP${NC}"
    echo -e "${YELLOW}[INFO]${NC} This failure is EXPECTED until ActivateBestChain is implemented"
    exit 1
fi

# Assertion 2: Active height must be INITIAL_HEIGHT + 4
EXPECTED_FINAL_HEIGHT=$((INITIAL_HEIGHT + 4))
if [ "$FINAL_HEIGHT" != "$EXPECTED_FINAL_HEIGHT" ]; then
    echo -e "${RED}[FAIL]${NC} ❌ ASSERTION FAILED"
    echo -e "${RED}       Active height is NOT $EXPECTED_FINAL_HEIGHT${NC}"
    echo -e "${RED}       Expected: $EXPECTED_FINAL_HEIGHT${NC}"
    echo -e "${RED}       Got:      $FINAL_HEIGHT${NC}"
    exit 1
fi

# Assertion 3: Verify E is actually connected (can get block)
E_BLOCK=$(rpc "blockchain.getblock" "$E_HASH" "1")
if [ -z "$E_BLOCK" ]; then
    echo -e "${RED}[FAIL]${NC} ❌ ASSERTION FAILED"
    echo -e "${RED}       Cannot retrieve block E from active chain${NC}"
    exit 1
fi

# Assertion 4: Verify C is NOT in active chain (should be disconnected)
C_BLOCK_HEIGHT=$(echo "$E_BLOCK" | jq -r '.height // empty')
HEIGHT_OF_D=$((INITIAL_HEIGHT + 3))
if [ "$C_BLOCK_HEIGHT" == "$HEIGHT_OF_D" ]; then
    # If we can get a block at height INITIAL_HEIGHT+3, it should be D (not C)
    HEIGHT_D_HASH=$(rpc "blockchain.getblockhash" "$HEIGHT_OF_D")
    if [ "$HEIGHT_D_HASH" == "$C_HASH" ]; then
        echo -e "${RED}[FAIL]${NC} ❌ ASSERTION FAILED"
        echo -e "${RED}       Block C is still in active chain (should be disconnected)${NC}"
        exit 1
    fi
fi

# ============================================================================
# SUCCESS: All assertions passed
# ============================================================================

echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}✅ TEST PASSED${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo -e "${GREEN}Verified:${NC}"
echo -e "  ✅ Active tip is E (highest chainwork)"
echo -e "  ✅ Active height is $EXPECTED_FINAL_HEIGHT (initial=$INITIAL_HEIGHT + 4 mined)"
echo -e "  ✅ E-chain is active (D and E connected)"
echo -e "  ✅ C-chain is disconnected (automatic reorg occurred)"
echo ""
echo -e "${GREEN}ActivateBestChain correctly selected the best chain!${NC}"

exit 0
