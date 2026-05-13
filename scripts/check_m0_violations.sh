#!/bin/bash
# Phase M.0 Enforcement: Detect .GetHex() violations in consensus/daemon layers
# This script ensures Phase M.0 stays clean forever
# Exit code: 0 = clean, 1 = violations found

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "🔒 Phase M.0 Enforcement Check"
echo "=============================="
echo ""

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

VIOLATIONS=0

# Check 1: String comparisons (most critical)
# Look for patterns like: .GetHex() == something or something == .GetHex()
# But NOT: status() == Ok) ? .GetHex() (ternary operator)
echo "🔍 Checking for string comparisons..."
STRING_COMP=$(grep -rn "\.GetHex()" src/consensus/ src/daemon/ 2>/dev/null | \
              grep -E "\.GetHex\(\)\s*(==|!=)|( == | != )\s*.*\.GetHex\(\)" | \
              grep -v "// Phase M.0" | \
              grep -v "\.substr" | \
              grep -v " ? .* : " || true)  # Exclude ternary operators

if [ -n "$STRING_COMP" ]; then
    echo -e "${RED}❌ CRITICAL: String comparisons found:${NC}"
    echo "$STRING_COMP"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
else
    echo -e "${GREEN}✅ No string comparisons found${NC}"
fi

echo ""

# Check 2: Early downgrades (storing hex in variables)
# Pattern: std::string var = ...GetHex()
# But NOT in logging statements
echo "🔍 Checking for early downgrades..."
EARLY_DOWN=$(grep -rn "std::string.*=.*\.GetHex()" src/consensus/ src/daemon/ 2>/dev/null | \
             grep -v "// Phase M.0" | \
             grep -v "logger\|MPLOG\|g_logger\|std::cout\|std::cerr\|fprintf" | \
             grep -v "\.substr" | \
             grep -v " ? .* : " || true)  # Exclude ternary operators

if [ -n "$EARLY_DOWN" ]; then
    echo -e "${RED}❌ CRITICAL: Early downgrades found:${NC}"
    echo "$EARLY_DOWN"
    echo ""
    VIOLATIONS=$((VIOLATIONS + 1))
else
    echo -e "${GREEN}✅ No early downgrades found${NC}"
fi

echo ""

# Check 3: Consensus layer purity (extra strict)
# Any .GetHex() in consensus that's not logging should be reviewed
echo "🔍 Checking consensus layer purity..."
CONSENSUS_HEX=$(grep -rn "\.GetHex()" src/consensus/ 2>/dev/null | \
                grep -v "// Phase M.0" | \
                grep -v "logger\|g_logger\|std::cout\|std::cerr\|fprintf" | \
                grep -v "\.substr" | \
                grep -v "GetHex().substr" || true)

if [ -n "$CONSENSUS_HEX" ]; then
    echo -e "${YELLOW}⚠️  INFO: Non-logging .GetHex() in consensus layer:${NC}"
    echo "$CONSENSUS_HEX"
    echo ""
    echo -e "${YELLOW}These are flagged for review (many are acceptable RPC/storage boundaries)${NC}"
else
    echo -e "${GREEN}✅ Consensus layer uses .GetHex() only for logging${NC}"
fi

echo ""
echo "=============================="

# Final verdict
if [ $VIOLATIONS -eq 0 ]; then
    echo -e "${GREEN}🎯 Phase M.0: CLEAN${NC}"
    echo -e "${GREEN}✅ All checks passed - no violations found${NC}"
    exit 0
else
    echo -e "${RED}❌ Phase M.0: VIOLATIONS DETECTED${NC}"
    echo -e "${RED}Found $VIOLATIONS critical violation(s)${NC}"
    echo ""
    echo "Phase M.0 Rule: \"uint256 is identity, .GetHex() is presentation\""
    echo ""
    echo "Forbidden patterns:"
    echo "  ❌ if (hash.GetHex() == other.GetHex()) { ... }"
    echo "  ❌ std::string h = hash.GetHex(); if (h == ...) { ... }"
    echo ""
    echo "Correct patterns:"
    echo "  ✅ if (hash == other) { ... }  // Direct uint256 comparison"
    echo "  ✅ logger->info(\"Hash: \" + hash.GetHex());  // Inline for logging"
    echo ""
    echo "See PHASE_M0_UINT256_INTEGRITY_LOCK.md for details"
    exit 1
fi
