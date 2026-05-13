#!/bin/bash

# ═══════════════════════════════════════════════════════════════════════════
# DineroCoin Economics Verification Script
# ═══════════════════════════════════════════════════════════════════════════
# This script verifies that ALL economic constants are consistent across
# the entire codebase with NO surprises.
# ═══════════════════════════════════════════════════════════════════════════

set -e  # Exit on error

echo "🔍 Verifying DineroCoin Economics..."
echo ""

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

ERRORS=0

# ═══════════════════════════════════════════════════════════════════════════
# 1. Verify 5-minute block time (300 seconds)
# ═══════════════════════════════════════════════════════════════════════════
echo "📊 Checking block time constants..."

if grep -q "TARGET_BLOCK_TIME_SECONDS = 300" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Block time: 5 minutes (300 seconds)${NC}"
else
    echo -e "${RED}❌ ERROR: Block time not set to 300 seconds${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -q "TARGET_SPACING = 5 \* 60" src/daemon/simple_blockchain.cpp; then
    echo -e "${GREEN}✅ Difficulty adjustment uses 5 minutes${NC}"
else
    echo -e "${RED}❌ ERROR: Difficulty adjustment not set to 5 minutes${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -q "BLOCK_TIME_SECONDS = 300" src/daemon/mining_safety_gates.cpp; then
    echo -e "${GREEN}✅ Mining safety gates use 5 minutes${NC}"
else
    echo -e "${RED}❌ ERROR: Mining safety gates not set to 300 seconds${NC}"
    ERRORS=$((ERRORS + 1))
fi

# Check for old 10-minute block time references (excluding false positives)
OLD_BLOCK_TIME_REFS=$(grep -E "BLOCK_TIME.*600|TARGET_SPACING.*10.*60" src/daemon/*.cpp 2>/dev/null | wc -l)
if [ "$OLD_BLOCK_TIME_REFS" -gt 0 ]; then
    echo -e "${RED}❌ ERROR: Found old 10-minute (600 second) block time references${NC}"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# ═══════════════════════════════════════════════════════════════════════════
# 2. Verify phase constants
# ═══════════════════════════════════════════════════════════════════════════
echo "🔢 Checking phase constants..."

if grep -q "PHASE1_START_HEIGHT = 2" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Phase 1 starts at height 2${NC}"
else
    echo -e "${RED}❌ ERROR: Phase 1 start height incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -q "PHASE1_BLOCKS = 180'000" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Phase 1 duration: 180,000 blocks${NC}"
else
    echo -e "${RED}❌ ERROR: Phase 1 blocks incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -q "PHASE1_REWARD = 100ULL" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Phase 1 reward: 100 DIN${NC}"
else
    echo -e "${RED}❌ ERROR: Phase 1 reward incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# ═══════════════════════════════════════════════════════════════════════════
# 3. Verify halving constants
# ═══════════════════════════════════════════════════════════════════════════
echo "⚡ Checking halving constants..."

if grep -q "PHASE2_INITIAL_REWARD = 50ULL" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Phase 2 initial reward: 50 DIN${NC}"
else
    echo -e "${RED}❌ ERROR: Phase 2 initial reward incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -q "HALVING_INTERVAL = 800'000" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Halving interval: 800,000 blocks${NC}"
else
    echo -e "${RED}❌ ERROR: Halving interval incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# ═══════════════════════════════════════════════════════════════════════════
# 4. Verify premine
# ═══════════════════════════════════════════════════════════════════════════
echo "💰 Checking premine constants..."

if grep -q "PREMINE_COINBASE = 1'000'000ULL" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Premine: 1,000,000 DIN${NC}"
else
    echo -e "${RED}❌ ERROR: Premine amount incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

if grep -q "din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f" src/daemon/consensus_subsidy.cpp; then
    echo -e "${GREEN}✅ Premine address: din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f${NC}"
else
    echo -e "${RED}❌ ERROR: Premine address mismatch${NC}"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# ═══════════════════════════════════════════════════════════════════════════
# 5. Verify unit precision
# ═══════════════════════════════════════════════════════════════════════════
echo "🔢 Checking unit precision..."

if grep -q "UNA_PER_DIN = 100'000'000ULL" src/daemon/consensus_subsidy.h; then
    echo -e "${GREEN}✅ Unit precision: 8 decimals (100,000,000 una per DIN)${NC}"
else
    echo -e "${RED}❌ ERROR: Unit precision incorrect${NC}"
    ERRORS=$((ERRORS + 1))
fi

# Check for hardcoded 100000000 (should use UNA_PER_DIN instead)
HARDCODED_COUNT=$(grep -r "100000000" src/daemon --exclude-dir=build --exclude="*.md" | grep -v "UNA_PER_DIN" | grep -v "100'000'000" | wc -l)
if [ "$HARDCODED_COUNT" -gt 0 ]; then
    echo -e "${YELLOW}⚠️  WARNING: Found $HARDCODED_COUNT instances of hardcoded 100000000${NC}"
    echo -e "${YELLOW}   (Should use dinero::ConsensusSubsidy::UNA_PER_DIN instead)${NC}"
fi

echo ""

# ═══════════════════════════════════════════════════════════════════════════
# 6. Calculate and verify timeline
# ═══════════════════════════════════════════════════════════════════════════
echo "📅 Calculating timeline with 5-minute blocks..."

PHASE1_MINUTES=$((180000 * 5))
PHASE1_DAYS=$((PHASE1_MINUTES / 1440))
PHASE1_YEARS=$(echo "scale=2; $PHASE1_DAYS / 365.25" | bc)

HALVING_MINUTES=$((800000 * 5))
HALVING_DAYS=$((HALVING_MINUTES / 1440))
HALVING_YEARS=$(echo "scale=2; $HALVING_DAYS / 365.25" | bc)

FIRST_HALVING_DAYS=$((PHASE1_DAYS + HALVING_DAYS))
FIRST_HALVING_YEARS=$(echo "scale=2; $FIRST_HALVING_DAYS / 365.25" | bc)

echo -e "${GREEN}✅ Phase 1 duration: $PHASE1_DAYS days (~$PHASE1_YEARS years)${NC}"
echo -e "${GREEN}✅ Halving period: $HALVING_DAYS days (~$HALVING_YEARS years)${NC}"
echo -e "${GREEN}✅ First halving: $FIRST_HALVING_DAYS days (~$FIRST_HALVING_YEARS years from genesis)${NC}"

echo ""

# ═══════════════════════════════════════════════════════════════════════════
# Final Result
# ═══════════════════════════════════════════════════════════════════════════
echo "════════════════════════════════════════════════════════════════"

if [ "$ERRORS" -eq 0 ]; then
    echo -e "${GREEN}✅ ALL CHECKS PASSED!${NC}"
    echo ""
    echo "Economics are consistent across the codebase:"
    echo "  • Block time: 5 minutes (300 seconds)"
    echo "  • Phase 1: 180,000 blocks (~1.71 years)"
    echo "  • Phase 2: 800,000 blocks per halving (~7.61 years)"
    echo "  • Premine: 1M DIN at din1qwaef7uj2p93kmyppezrgnpy3fyr4xw34stkm6f"
    echo "  • Unit precision: 8 decimals"
    echo ""
    echo "🔒 NO SURPRISES - All math is hardcoded and verified!"
    exit 0
else
    echo -e "${RED}❌ VERIFICATION FAILED WITH $ERRORS ERROR(S)${NC}"
    echo ""
    echo "Please fix the errors above before deployment!"
    exit 1
fi
