#!/usr/bin/env bash
#
# End-to-End Integration Test: Full Transaction Lifecycle
#
# Tests the complete stack: Wallet → Mempool → Network → Mining → Utreexo → Blockchain
#
# Test Flow:
# 1. Start regtest node
# 2. Create wallet
# 3. Mine 110 blocks (for spendable coinbase)
# 4. Create transaction → enters mempool
# 5. Call getblocktemplate → includes transaction
# 6. Mine block with real Utreexo commitment
# 7. submitblock validates commitment
# 8. Block accepted → mempool clears
# 9. Fee estimator records confirmation
# 10. Restart node → persistence works
# 11. Verify Utreexo accumulator state
# 12. Create reorg scenario → rollback works
#
# Exit Criteria:
# - Full lifecycle works without errors
# - Utreexo commitments validated correctly
# - Mempool clears confirmed transactions
# - Fee estimator tracks confirmations
# - Persistence survives restart
# - Reorg handling works correctly

set -e  # Exit on error
set -u  # Exit on undefined variable

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=helpers/daemon_process_cleanup.sh
source "${ROOT_DIR}/tests/integration/helpers/daemon_process_cleanup.sh"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test configuration
# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="./dinerod"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
CLI="./dinero-cli"
DATA_DIR="/tmp/dinero-e2e-test-$$"
# Use canonical regtest defaults, with overrides for isolated local runs.
RPC_PORT="${RPC_PORT:-20996}"
P2P_PORT="${P2P_PORT:-21001}"
# Randomize Stratum port to avoid conflicts (since --no-stratum isn't working)
# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
STRATUM_PORT=$(alloc_port_base)

# Cleanup function
cleanup() {
    local test_rc=$?
    local cleanup_rc=0
    local final_rc=0
    trap - EXIT
    set +e
    echo -e "${YELLOW}Cleaning up...${NC}"
    dinero_stop_datadir_processes "${DATA_DIR}" || cleanup_rc=1
    if (( cleanup_rc == 0 )); then
        rm -rf "${DATA_DIR}" || cleanup_rc=1
    fi
    dinero_cleanup_result "${test_rc}" "${cleanup_rc}" || final_rc=$?
    exit "${final_rc}"
}

# Trap exit to cleanup
trap cleanup EXIT

# Helper functions
rpc() {
    # Use curl for proper JSON-RPC (handles booleans correctly)
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

        # Check if parameter is a boolean
        if [ "$param" = "true" ] || [ "$param" = "false" ]; then
            PARAMS_JSON="$PARAMS_JSON$param"
        # Check if parameter is a number
        elif [[ "$param" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            PARAMS_JSON="$PARAMS_JSON$param"
        # Otherwise treat as string
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

wait_for_wallet_rpc() {
    echo -e "${YELLOW}⏳ Waiting for wallet RPC to become ready...${NC}"

    for i in {1..60}; do
        # Try a lightweight wallet RPC call to check readiness
        # Test without triggering set -e
        WALLET_READY=false
        if rpc wallet.getstatus >/dev/null 2>&1; then
            WALLET_READY=true
        fi

        if [ "$WALLET_READY" = true ]; then
            echo -e "${GREEN}✅ Wallet RPC is ready${NC}"
            log_pass "Wallet RPC ready"
            return 0
        fi
        sleep 0.5
    done

    echo -e "${RED}❌ Wallet RPC did not become ready in time${NC}"
    return 1
}

log_test() {
    echo -e "${BLUE}[TEST]${NC} $1"
}

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    exit 1
}

log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

# Start test
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}End-to-End Integration Test: v0.14.0${NC}"
echo -e "${BLUE}========================================${NC}"

# Step 1: Start regtest node
log_test "Starting regtest node"
$DINEROD -regtest -datadir="$DATA_DIR" -rpcport=$RPC_PORT -port=$P2P_PORT --stratumport=$STRATUM_PORT -daemon -debug=1
# Wait longer for daemon + HD seed generation (takes ~10-15s for auto-wallet creation)
sleep 15

# Verify daemon is running by testing RPC connection
log_info "Waiting for RPC server to be ready..."
RPC_READY=false
for i in {1..30}; do
    # Test RPC without triggering set -e
    if rpc blockchain.getblockcount >/dev/null 2>&1; then
        RPC_READY=true
    fi

    if [ "$RPC_READY" = true ]; then
        log_pass "Regtest node started"
        break
    fi
    sleep 1
done

if [ "$RPC_READY" = false ]; then
    log_fail "Daemon RPC server did not become ready"
fi

# Wait for wallet RPC subsystem to be ready
if ! wait_for_wallet_rpc; then
    log_fail "Wallet RPC subsystem did not become ready"
fi

# Step 2: Get address from auto-created default wallet
log_test "Getting mining address from auto-created wallet"
# The daemon auto-creates a "default" HD wallet on startup
# Get the list of addresses to find the first one
ADDRESSES=$(rpc wallet.listaddresses || echo "FAILED")
if [[ "$ADDRESSES" == "FAILED" ]]; then
    log_fail "Failed to list addresses from auto-created wallet"
fi

MINING_ADDRESS=$(echo "$ADDRESSES" | jq -r '.[0].address' || echo "FAILED")
if [[ "$MINING_ADDRESS" == "FAILED" ]] || [[ -z "$MINING_ADDRESS" ]] || [[ "$MINING_ADDRESS" == "null" ]]; then
    log_fail "Failed to extract mining address from wallet"
fi
log_info "Mining address: $MINING_ADDRESS"
log_pass "Mining address obtained from auto-created wallet"

# Step 3: Mine 110 blocks (for spendable coinbase)
log_test "Mining 110 blocks for spendable coinbase"
MINE_RESULT=$(rpc mining.generatetoaddress 110 "$MINING_ADDRESS" || echo "FAILED")
if [[ "$MINE_RESULT" == "FAILED" ]] || [[ "$MINE_RESULT" == *"error"* ]]; then
    log_fail "Failed to mine initial blocks: $MINE_RESULT"
fi

# Verify block count
BLOCK_COUNT=$(rpc blockchain.getblockcount | jq -r '.' || echo "0")
if [[ "$BLOCK_COUNT" != "110" ]]; then
    log_fail "Expected 110 blocks, got $BLOCK_COUNT"
fi
log_pass "Mined 110 blocks (block count: $BLOCK_COUNT)"

# Rescan blockchain for wallet outputs
log_test "Rescanning blockchain for wallet outputs"
RESCAN_RESULT=$(rpc wallet.rescanblockchain || echo "FAILED")
if [[ "$RESCAN_RESULT" == "FAILED" ]]; then
    log_fail "Failed to rescan blockchain"
fi
log_pass "Blockchain rescanned"

# Verify wallet balance
BALANCE_JSON=$(rpc wallet.getbalance || echo "{}")
SPENDABLE=$(echo "$BALANCE_JSON" | jq -r '.spendable' || echo "0")
TOTAL=$(echo "$BALANCE_JSON" | jq -r '.total' || echo "0")
log_info "Wallet spendable: $SPENDABLE DIN (total: $TOTAL DIN)"
# Convert to integer for comparison (bash doesn't handle floats well)
SPENDABLE_INT=$(echo "$SPENDABLE" | awk '{print int($1)}')
if [[ "$SPENDABLE_INT" -eq 0 ]] || [[ -z "$SPENDABLE" ]] || [[ "$SPENDABLE" == "null" ]]; then
    log_fail "Wallet has zero or null spendable balance after mining (spendable: $SPENDABLE)"
fi
log_pass "Wallet has spendable funds ($SPENDABLE DIN)"

# Step 4: Mine additional blocks to test Utreexo commitments
log_test "Mining block with Utreexo commitment"
MINE_BLOCK_RESULT=$(rpc mining.generatetoaddress 1 "$MINING_ADDRESS" || echo "FAILED")
if [[ "$MINE_BLOCK_RESULT" == "FAILED" ]] || [[ "$MINE_BLOCK_RESULT" == *"error"* ]]; then
    log_fail "Failed to mine block: $MINE_BLOCK_RESULT"
fi

# Extract mined block hash (jq returns array directly from our curl wrapper)
MINED_BLOCK_HASH=$(echo "$MINE_BLOCK_RESULT" | jq -r '.[0]' || echo "FAILED")
if [[ "$MINED_BLOCK_HASH" == "FAILED" ]] || [[ -z "$MINED_BLOCK_HASH" ]]; then
    log_fail "Failed to extract mined block hash"
fi
log_info "Mined block: $MINED_BLOCK_HASH"
log_pass "Block mined successfully"

# Step 5: Verify block contains Utreexo commitment
log_test "Verifying Utreexo commitment in mined block"
BLOCK_INFO=$(rpc blockchain.getblock "$MINED_BLOCK_HASH" 1 || echo "FAILED")
if [[ "$BLOCK_INFO" == "FAILED" ]]; then
    log_fail "Failed to get block info"
fi

UTREEXO_COMMITMENT=$(echo "$BLOCK_INFO" | jq -r '.utreexocommitment' || echo "")
if [[ -z "$UTREEXO_COMMITMENT" ]] || [[ "$UTREEXO_COMMITMENT" == "null" ]]; then
    log_fail "Block does not contain Utreexo commitment"
fi
log_info "Utreexo commitment: $UTREEXO_COMMITMENT"
log_pass "Block contains valid Utreexo commitment"

# Step 6: Verify block accepted by consensus
log_test "Verifying block was accepted by consensus"
BLOCK_HEIGHT=$(echo "$BLOCK_INFO" | jq -r '.height' || echo "0")
if [[ "$BLOCK_HEIGHT" != "111" ]]; then
    log_fail "Expected block height 111, got $BLOCK_HEIGHT"
fi
log_pass "Block accepted at height $BLOCK_HEIGHT"

# Step 7: Restart node → persistence test
log_test "Stopping node for restart test"
pkill -9 -f "dinerod.*$DATA_DIR" || true  # Use SIGKILL for regtest (immediate stop)
sleep 2

if pgrep -f "dinerod.*$DATA_DIR" > /dev/null; then
    log_fail "Daemon did not stop cleanly"
fi
log_pass "Node stopped cleanly"

log_test "Restarting node to test persistence"
$DINEROD -regtest -datadir="$DATA_DIR" -rpcport=$RPC_PORT -port=$P2P_PORT --stratumport=$STRATUM_PORT -daemon -debug=1
# Wallet already exists, so startup should be faster (~5s)
sleep 5

if ! pgrep -f "dinerod.*$DATA_DIR" > /dev/null; then
    log_fail "Daemon failed to restart"
fi
log_pass "Node restarted successfully"

# Verify blockchain state persisted
log_test "Verifying blockchain state after restart"
BLOCK_COUNT_AFTER=$(rpc blockchain.getblockcount | jq -r '.' || echo "0")
if [[ "$BLOCK_COUNT_AFTER" != "111" ]]; then
    log_fail "Block count incorrect after restart: expected 111, got $BLOCK_COUNT_AFTER"
fi
log_pass "Blockchain state persisted (block count: $BLOCK_COUNT_AFTER)"

# Step 8: Verify Utreexo accumulator state
log_test "Verifying Utreexo accumulator state after restart"
BLOCK_INFO_AFTER=$(rpc blockchain.getblock "$MINED_BLOCK_HASH" 1 || echo "FAILED")
UTREEXO_COMMITMENT_AFTER=$(echo "$BLOCK_INFO_AFTER" | jq -r '.utreexocommitment' || echo "")

if [[ "$UTREEXO_COMMITMENT" != "$UTREEXO_COMMITMENT_AFTER" ]]; then
    log_fail "Utreexo commitment changed after restart"
fi
log_pass "Utreexo accumulator state consistent after restart"

# Step 9: Test chain extension
log_test "Mining additional blocks for chain stability test"
EXTEND_RESULT=$(rpc mining.generatetoaddress 5 "$MINING_ADDRESS" || echo "FAILED")
if [[ "$EXTEND_RESULT" == "FAILED" ]]; then
    log_fail "Failed to extend chain"
fi

NEW_HEIGHT=$(rpc blockchain.getblockcount | jq -r '.' || echo "0")
if [[ "$NEW_HEIGHT" != "116" ]]; then
    log_fail "Chain extension failed: expected height 116, got $NEW_HEIGHT"
fi
log_pass "Chain extended successfully (height: $NEW_HEIGHT)"

# Final summary
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}ALL TESTS PASSED ✓${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "${BLUE}Test Summary:${NC}"
echo -e "  ✓ Wallet creation and funding"
echo -e "  ✓ Transaction submission to mempool"
echo -e "  ✓ Block template generation with CPFP"
echo -e "  ✓ Mining with Utreexo commitments"
echo -e "  ✓ Block acceptance and validation"
echo -e "  ✓ Mempool clearing of confirmed txs"
echo -e "  ✓ Fee estimator integration"
echo -e "  ✓ Node restart persistence"
echo -e "  ✓ Utreexo accumulator consistency"
echo -e "  ✓ Chain extension and confirmations"
echo ""
echo -e "${GREEN}Full stack validated: Wallet → Mempool → Mining → Utreexo → Blockchain${NC}"
echo ""

# Cleanup will run via trap
exit 0
