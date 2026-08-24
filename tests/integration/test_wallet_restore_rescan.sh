#!/bin/bash
#
# Wallet Restore E2E Test (REQUIRED CI GATE)
#
# This test validates the complete wallet restore flow:
#   1. Mine blocks, receive coins
#   2. Record balance
#   3. Delete wallet database
#   4. Restore from mnemonic
#   5. Rescan blockchain
#   6. Assert balance matches
#
# This is a CONSENSUS-ADJACENT test - wallet restore must be correct.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Find dinerod
# Honour $DINEROD first (and require it to be executable); the chain
# below never consulted it, so it CLOBBERED the caller's choice and an
# arbitrary build directory could not be used.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    echo "dinerod not found (tried: \$DINEROD unset, ${PROJECT_ROOT}/build/dinerod, ${PROJECT_ROOT}/dinerod)" >&2
    echo "set DINEROD=/path/to/dinerod to override" >&2
    exit 1
fi

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

DATADIR=""
cleanup() {
    [[ -n "$DATADIR" ]] && pkill -9 -f "dinerod.*${DATADIR}" 2>/dev/null || true
    sleep 1
    [[ -n "$DATADIR" && -d "$DATADIR" ]] && rm -rf "$DATADIR"
}
trap cleanup EXIT

rpc_call() {
    local port=$1
    local method=$2
    shift 2
    local params="$*"
    local cookie=$(cat "$DATADIR/.cookie" 2>/dev/null)
    [[ -z "$cookie" ]] && return 1
    local json_params="[]"
    [[ -n "$params" ]] && json_params="[$params]"
    curl -s -u "$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$json_params,\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

echo ""
echo "================================================================"
echo "  WALLET RESTORE E2E TEST (REQUIRED CI GATE)"
echo "  Validates: wallet restore + rescan = correct balance"
echo "================================================================"
echo ""

# ===================================================================
# Step 1: Start node and create wallet
# ===================================================================
echo -e "${CYAN}[1/7] Starting node...${NC}"
DATADIR=$(mktemp -d -t dinero_wallet_restore_XXXXXX)
RPC_PORT=$((27000 + RANDOM % 1000))
P2P_PORT=$((RPC_PORT + 1))

"$DINEROD" --regtest --datadir="$DATADIR" --rpcport="$RPC_PORT" --port="$P2P_PORT" > "$DATADIR/daemon.log" 2>&1 &
sleep 8

COOKIE=$(cat "$DATADIR/.cookie" 2>/dev/null)
if [[ -z "$COOKIE" ]]; then
    echo -e "${RED}FAILED: Node did not start${NC}"
    exit 1
fi
echo "  Node ready on port $RPC_PORT"

# ===================================================================
# Step 2: Create HD wallet and get mnemonic
# ===================================================================
echo -e "${CYAN}[2/7] Creating HD wallet...${NC}"

WALLET_RESULT=$(rpc_call "$RPC_PORT" "wallet.createhd" '"restore_test_wallet"')

# Extract mnemonic (the seed phrase we'll use to restore)
MNEMONIC=$(echo "$WALLET_RESULT" | tr -d '\n\t' | sed -n 's/.*"mnemonic"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
ADDR=$(echo "$WALLET_RESULT" | tr -d '\n\t' | sed -n 's/.*"first_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

if [[ -z "$MNEMONIC" || -z "$ADDR" ]]; then
    echo -e "${RED}FAILED: Could not create wallet or get mnemonic${NC}"
    echo "  Result: $WALLET_RESULT"
    exit 1
fi

MNEMONIC_WORDS=$(echo "$MNEMONIC" | wc -w | tr -d ' ')
echo "  Mnemonic: $MNEMONIC_WORDS words"
echo "  First address: ${ADDR:0:20}..."

# ===================================================================
# Step 3: Mine blocks and receive coins
# ===================================================================
echo -e "${CYAN}[3/7] Mining 50 blocks...${NC}"

rpc_call "$RPC_PORT" "generatetoaddress" "50, \"$ADDR\"" > /dev/null

# Wait a moment for wallet to process
sleep 2

# Get balance
BALANCE_RESULT=$(rpc_call "$RPC_PORT" "wallet.getbalance")
ORIGINAL_CONFIRMED=$(echo "$BALANCE_RESULT" | tr -d '\n\t' | sed -n 's/.*"confirmed"[[:space:]]*:[[:space:]]*\([0-9.]*\).*/\1/p')
ORIGINAL_IMMATURE=$(echo "$BALANCE_RESULT" | tr -d '\n\t' | sed -n 's/.*"immature"[[:space:]]*:[[:space:]]*\([0-9.]*\).*/\1/p')

# Total = confirmed + immature (immature is coinbase not yet mature)
ORIGINAL_TOTAL=$(echo "$ORIGINAL_CONFIRMED + $ORIGINAL_IMMATURE" | bc 2>/dev/null || echo "0")

echo "  Confirmed: $ORIGINAL_CONFIRMED DIN"
echo "  Immature: $ORIGINAL_IMMATURE DIN"
echo "  Total: $ORIGINAL_TOTAL DIN"

if [[ "$ORIGINAL_TOTAL" == "0" || -z "$ORIGINAL_TOTAL" ]]; then
    echo -e "${RED}FAILED: No balance after mining${NC}"
    exit 1
fi

# ===================================================================
# Step 4: Shutdown node
# ===================================================================
echo -e "${CYAN}[4/7] Shutting down node...${NC}"
rpc_call "$RPC_PORT" "stop" > /dev/null 2>&1 || true
sleep 3
pkill -9 -f "dinerod.*${DATADIR}" 2>/dev/null || true
sleep 2
echo "  Node stopped"

# ===================================================================
# Step 5: Delete wallet database (simulate lost wallet)
# ===================================================================
echo -e "${CYAN}[5/7] Deleting wallet database (simulating loss)...${NC}"

# Find and delete wallet database files
WALLET_DIR="$DATADIR/regtest/wallets"
if [[ -d "$WALLET_DIR" ]]; then
    rm -rf "$WALLET_DIR"
    echo "  Deleted: $WALLET_DIR"
else
    # Alternative location
    find "$DATADIR" -name "*.wallet.db" -delete 2>/dev/null
    find "$DATADIR" -name "wallet.db" -delete 2>/dev/null
    echo "  Deleted wallet database files"
fi

# ===================================================================
# Step 6: Restart node and restore wallet from mnemonic
# ===================================================================
echo -e "${CYAN}[6/7] Restoring wallet from mnemonic...${NC}"

"$DINEROD" --regtest --datadir="$DATADIR" --rpcport="$RPC_PORT" --port="$P2P_PORT" > "$DATADIR/daemon.log" 2>&1 &
sleep 8

COOKIE=$(cat "$DATADIR/.cookie" 2>/dev/null)
if [[ -z "$COOKIE" ]]; then
    echo -e "${RED}FAILED: Node did not restart${NC}"
    exit 1
fi

# Restore wallet from mnemonic
RESTORE_RESULT=$(rpc_call "$RPC_PORT" "wallet.restorehd" "\"restored_wallet\", \"$MNEMONIC\"")

if echo "$RESTORE_RESULT" | grep -qi "error"; then
    echo -e "${RED}FAILED: Could not restore wallet${NC}"
    echo "  Result: $RESTORE_RESULT"
    exit 1
fi

RESTORED_ADDR=$(echo "$RESTORE_RESULT" | tr -d '\n\t' | sed -n 's/.*"first_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')

# Verify same address
if [[ "$RESTORED_ADDR" != "$ADDR" ]]; then
    echo -e "${RED}FAILED: Restored address doesn't match original${NC}"
    echo "  Original: $ADDR"
    echo "  Restored: $RESTORED_ADDR"
    exit 1
fi
echo "  Address verified: ${RESTORED_ADDR:0:20}..."

# Trigger rescan
echo "  Triggering rescan..."
rpc_call "$RPC_PORT" "wallet.rescan" "0" > /dev/null 2>&1 || true

# Wait for rescan to complete
sleep 5

# ===================================================================
# Step 7: Verify balance matches
# ===================================================================
echo -e "${CYAN}[7/7] Verifying balance...${NC}"

RESTORED_BALANCE=$(rpc_call "$RPC_PORT" "wallet.getbalance")
RESTORED_CONFIRMED=$(echo "$RESTORED_BALANCE" | tr -d '\n\t' | sed -n 's/.*"confirmed"[[:space:]]*:[[:space:]]*\([0-9.]*\).*/\1/p')
RESTORED_IMMATURE=$(echo "$RESTORED_BALANCE" | tr -d '\n\t' | sed -n 's/.*"immature"[[:space:]]*:[[:space:]]*\([0-9.]*\).*/\1/p')
RESTORED_TOTAL=$(echo "$RESTORED_CONFIRMED + $RESTORED_IMMATURE" | bc 2>/dev/null || echo "0")

echo "  Original balance: $ORIGINAL_TOTAL DIN"
echo "  Restored balance: $RESTORED_TOTAL DIN"

# Compare balances
if [[ "$RESTORED_TOTAL" == "$ORIGINAL_TOTAL" ]]; then
    echo -e "  ${GREEN}BALANCE MATCH${NC}"
else
    echo -e "${RED}FAILED: Balance mismatch!${NC}"
    echo "  Expected: $ORIGINAL_TOTAL"
    echo "  Got: $RESTORED_TOTAL"
    exit 1
fi

echo ""
echo "================================================================"
echo -e "${GREEN}WALLET RESTORE E2E TEST PASSED${NC}"
echo "================================================================"
echo ""
echo "Validated:"
echo "  - HD wallet creation and mnemonic export"
echo "  - Wallet restoration from mnemonic"
echo "  - Blockchain rescan finds all UTXOs"
echo "  - Balance matches after restore"
echo ""

exit 0
