#!/bin/bash
# ============================================================================
# UTXO Persistence Regression Test
# ============================================================================
#
# Purpose: Verify that mined blocks persist UTXOs to ChainDB so gettxout works.
#
# This test catches the bug where apply_canonical_writes was hardcoded to false
# and ConnectTip never persisted UTXOs, leaving only genesis UTXOs in ChainDB.
#
# Test Flow:
#   1. Start node (regtest)
#   2. Create wallet & get address
#   3. Record initial UTXO count (gettxoutsetinfo)
#   4. Mine 3 blocks
#   5. For each block: verify gettxout returns non-null for coinbase output
#   6. Verify gettxoutsetinfo.txouts increased by expected amount
#
# Exit codes: 0 = pass, 1 = fail
# ============================================================================

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuration
DATADIR="/tmp/utxo_persistence_test_$$"
RPC_PORT=19900
P2P_PORT=19901
DINEROD="${DINEROD:-./dinerod}"
BLOCKS_TO_MINE=3
PASS=0
FAIL=0

echo -e "${BLUE}============================================================================${NC}"
echo -e "${BLUE}UTXO Persistence Regression Test${NC}"
echo -e "${BLUE}============================================================================${NC}"
echo ""

# ── Helpers ──────────────────────────────────────────────────────────────────

rpc_call() {
    local method="$1"
    local params="$2"
    curl -s -X POST "http://127.0.0.1:$RPC_PORT" \
        -u "$COOKIE" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":1}"
}

assert_eq() {
    local desc="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC} $desc (got: $actual)"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $desc (expected: $expected, got: $actual)"
        FAIL=$((FAIL + 1))
    fi
}

assert_not_null() {
    local desc="$1" value="$2"
    if [ -n "$value" ] && [ "$value" != "null" ] && [ "$value" != "" ]; then
        echo -e "  ${GREEN}PASS${NC} $desc (value: $value)"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $desc (got null/empty)"
        FAIL=$((FAIL + 1))
    fi
}

assert_ge() {
    local desc="$1" actual="$2" expected="$3"
    if [ "$actual" -ge "$expected" ] 2>/dev/null; then
        echo -e "  ${GREEN}PASS${NC} $desc ($actual >= $expected)"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $desc (expected >= $expected, got: $actual)"
        FAIL=$((FAIL + 1))
    fi
}

# ── Cleanup ──────────────────────────────────────────────────────────────────

cleanup() {
    echo ""
    echo -e "${YELLOW}[Cleanup] Stopping daemon...${NC}"
    if [ -n "$DINEROD_PID" ]; then
        kill "$DINEROD_PID" 2>/dev/null || true
        wait "$DINEROD_PID" 2>/dev/null || true
    fi
    rm -rf "$DATADIR"
    echo -e "${GREEN}[Cleanup] Complete${NC}"
}

trap cleanup EXIT

# ── Step 1: Start Node ──────────────────────────────────────────────────────

echo -e "${BLUE}Step 1: Start regtest node${NC}"

rm -rf "$DATADIR"
mkdir -p "$DATADIR"

"$DINEROD" --regtest --datadir="$DATADIR" --rpcport=$RPC_PORT --p2pport=$P2P_PORT >/dev/null 2>&1 &
DINEROD_PID=$!

# Wait for cookie file and RPC to become available (up to 60s)
RPC_READY=0
for i in $(seq 1 60); do
    if [ -f "$DATADIR/.cookie" ]; then
        COOKIE=$(cat "$DATADIR/.cookie")
        if curl -s "http://127.0.0.1:$RPC_PORT" -u "$COOKIE" >/dev/null 2>&1; then
            RPC_READY=1
            break
        fi
    fi
    sleep 1
done

if [ "$RPC_READY" -ne 1 ]; then
    echo -e "${RED}[FAIL] Node failed to start (RPC not available after 60s)${NC}"
    exit 1
fi

echo -e "  ${GREEN}Node started (PID $DINEROD_PID, RPC port $RPC_PORT)${NC}"
echo ""

# ── Step 2: Create Wallet ───────────────────────────────────────────────────

echo -e "${BLUE}Step 2: Create wallet${NC}"

CREATE_RESULT=$(rpc_call "wallet.createhd" '["utxo_test"]')
ADDR=$(echo "$CREATE_RESULT" | jq -r '.result.first_address // empty')

if [ -z "$ADDR" ]; then
    echo -e "${RED}[FAIL] Failed to create wallet${NC}"
    echo "Response: $CREATE_RESULT"
    exit 1
fi

echo -e "  ${GREEN}Wallet created, address: ${ADDR:0:20}...${NC}"
echo ""

# ── Step 3: Record Initial UTXO Count ──────────────────────────────────────

echo -e "${BLUE}Step 3: Record initial UTXO count${NC}"

INITIAL_INFO=$(rpc_call "blockchain.gettxoutsetinfo" '["none"]')
INITIAL_UTXOS=$(echo "$INITIAL_INFO" | jq -r '.result.txouts // 0')
INITIAL_HEIGHT=$(echo "$INITIAL_INFO" | jq -r '.result.height // 0')

echo "  Initial UTXOs: $INITIAL_UTXOS"
echo "  Initial height: $INITIAL_HEIGHT"
echo ""

# ── Step 4: Mine 3 Blocks ──────────────────────────────────────────────────

echo -e "${BLUE}Step 4: Mine $BLOCKS_TO_MINE blocks${NC}"

MINE_RESULT=$(rpc_call "generatetoaddress" "[$BLOCKS_TO_MINE,\"$ADDR\"]")
MINE_ERROR=$(echo "$MINE_RESULT" | jq -r '.error // empty')

if [ -n "$MINE_ERROR" ] && [ "$MINE_ERROR" != "null" ]; then
    echo -e "${RED}[FAIL] generatetoaddress failed: $MINE_ERROR${NC}"
    exit 1
fi

# Small delay for UTXO writes to flush
sleep 2

# Verify block count increased
NEW_HEIGHT_RESULT=$(rpc_call "blockchain.getblockcount" '[]')
NEW_HEIGHT=$(echo "$NEW_HEIGHT_RESULT" | jq -r '.result // 0')

EXPECTED_HEIGHT=$((INITIAL_HEIGHT + BLOCKS_TO_MINE))
echo "  New height: $NEW_HEIGHT (expected: $EXPECTED_HEIGHT)"
assert_eq "Block height after mining" "$NEW_HEIGHT" "$EXPECTED_HEIGHT"
echo ""

# ── Step 5: Verify gettxout for Each Coinbase ──────────────────────────────

echo -e "${BLUE}Step 5: Verify gettxout returns non-null for each coinbase${NC}"

for i in $(seq 1 $BLOCKS_TO_MINE); do
    BLOCK_HEIGHT=$((INITIAL_HEIGHT + i))

    # Get block hash for this height
    HASH_RESULT=$(rpc_call "blockchain.getblockhash" "[$BLOCK_HEIGHT]")
    BLOCK_HASH=$(echo "$HASH_RESULT" | jq -r '.result // empty')
    assert_not_null "Block $BLOCK_HEIGHT hash" "$BLOCK_HASH"

    if [ -z "$BLOCK_HASH" ] || [ "$BLOCK_HASH" = "null" ]; then
        echo -e "  ${RED}Skipping gettxout — no block hash${NC}"
        continue
    fi

    # Get block to find coinbase txid
    BLOCK_RESULT=$(rpc_call "blockchain.getblock" "[\"$BLOCK_HASH\"]")
    COINBASE_TXID=$(echo "$BLOCK_RESULT" | jq -r '.result.tx[0] // empty')

    # If tx[0] is an object, try .txid field
    if [ -z "$COINBASE_TXID" ] || [ "$COINBASE_TXID" = "null" ]; then
        COINBASE_TXID=$(echo "$BLOCK_RESULT" | jq -r '.result.tx[0].txid // empty')
    fi

    assert_not_null "Block $BLOCK_HEIGHT coinbase txid" "$COINBASE_TXID"

    if [ -z "$COINBASE_TXID" ] || [ "$COINBASE_TXID" = "null" ]; then
        echo -e "  ${RED}Skipping gettxout — no coinbase txid${NC}"
        continue
    fi

    # THE KEY TEST: gettxout must return non-null for this coinbase output
    UTXO_RESULT=$(rpc_call "blockchain.gettxout" "[\"$COINBASE_TXID\",0]")
    UTXO_VALUE=$(echo "$UTXO_RESULT" | jq -r '.result.amount // .result.value // empty')
    UTXO_ERROR=$(echo "$UTXO_RESULT" | jq -r '.error // empty')

    if [ -n "$UTXO_ERROR" ] && [ "$UTXO_ERROR" != "null" ]; then
        echo -e "  ${RED}FAIL${NC} gettxout(block $BLOCK_HEIGHT coinbase) returned error: $UTXO_ERROR"
        FAIL=$((FAIL + 1))
    else
        assert_not_null "gettxout(block $BLOCK_HEIGHT coinbase, vout=0)" "$UTXO_VALUE"
    fi
done

echo ""

# ── Step 6: Verify UTXO Count Increased ────────────────────────────────────

echo -e "${BLUE}Step 6: Verify gettxoutsetinfo.txouts increased${NC}"

FINAL_INFO=$(rpc_call "blockchain.gettxoutsetinfo" '["none"]')
FINAL_UTXOS=$(echo "$FINAL_INFO" | jq -r '.result.txouts // 0')

echo "  Initial UTXOs: $INITIAL_UTXOS"
echo "  Final UTXOs:   $FINAL_UTXOS"

UTXO_INCREASE=$((FINAL_UTXOS - INITIAL_UTXOS))
echo "  Increase:      $UTXO_INCREASE"

# Each mined block creates exactly 1 coinbase output (no inputs consumed)
assert_ge "UTXO count increase" "$UTXO_INCREASE" "$BLOCKS_TO_MINE"
echo ""

# ── Summary ─────────────────────────────────────────────────────────────────

echo ""
echo -e "${BLUE}============================================================================${NC}"
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}ALL $TOTAL ASSERTIONS PASSED${NC}"
    echo -e "${GREEN}UTXO persistence is working correctly.${NC}"
else
    echo -e "${RED}$FAIL of $TOTAL ASSERTIONS FAILED${NC}"
    echo -e "${RED}UTXO persistence regression detected!${NC}"
fi
echo -e "${BLUE}============================================================================${NC}"
echo ""

exit "$FAIL"
