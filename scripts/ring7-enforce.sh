#!/bin/bash
#
# Ring 7 Enforcement Script
#
# This script mechanically enforces Ring 7 semantic immutability.
# It is called by:
# - Git pre-commit hook (local enforcement)
# - CI pipeline (PR enforcement)
# - Build system (compilation enforcement)
#
# Exit codes:
# 0 = Ring 7 verified (semantics unchanged)
# 1 = Ring 7 violation detected (commit MUST be rejected)

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo ""
echo "🔒 Ring 7 Semantic Immutability Enforcement"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Check if this is a consensus-affecting change
CONSENSUS_CHANGED=0

if [ -n "$(git diff --cached --name-only 2>/dev/null | grep -E 'src/consensus|src/script|include/consensus|include/script|tests/execution')" ]; then
    CONSENSUS_CHANGED=1
    echo "⚠️  Consensus-affecting changes detected:"
    git diff --cached --name-only 2>/dev/null | grep -E 'src/consensus|src/script|include/consensus|include/script|tests/execution' | sed 's/^/    /'
    echo ""
elif [ -n "$(git diff --name-only HEAD~1 2>/dev/null | grep -E 'src/consensus|src/script|include/consensus|include/script|tests/execution')" ]; then
    CONSENSUS_CHANGED=1
    echo "⚠️  Consensus-affecting changes detected in last commit:"
    git diff --name-only HEAD~1 2>/dev/null | grep -E 'src/consensus|src/script|include/consensus|include/script|tests/execution' | sed 's/^/    /'
    echo ""
fi

# If no consensus changes, skip Ring 7 verification
if [ $CONSENSUS_CHANGED -eq 0 ]; then
    echo "✅ No consensus changes detected - Ring 7 verification not required"
    echo ""
    exit 0
fi

# Consensus changes detected - Ring 7 verification MANDATORY
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔐 MANDATORY: Ring 7 verification required for consensus changes"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Check if build directory exists
if [ ! -d "$PROJECT_ROOT/build" ]; then
    echo -e "${RED}❌ Build directory not found${NC}"
    echo ""
    echo "Ring 7 enforcement requires running tests."
    echo "Please run: cmake -S . -B build && cmake --build build"
    echo ""
    exit 1
fi

# Run Ring 7 tests
echo "Running Ring 7 test suite (25 properties, 91 tests)..."
echo ""

cd "$PROJECT_ROOT"

# Run all Ring 7 tests
if ctest --test-dir build -R "ring7|Execution_" --output-on-failure; then
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${GREEN}✅ Ring 7 VERIFIED${NC}"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    echo "Script execution semantics unchanged."
    echo "Consensus change is backward compatible (Ring 8a: BC1)."
    echo ""
    exit 0
else
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${RED}❌ RING 7 VIOLATION DETECTED${NC}"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    echo -e "${RED}THIS COMMIT VIOLATES RING 7 SEMANTIC IMMUTABILITY${NC}"
    echo ""
    echo "Ring 7 properties (S1-S25) are FROZEN. Your change:"
    echo "  1. Altered script execution semantics, OR"
    echo "  2. Changed opcode meanings, OR"
    echo "  3. Broke determinism guarantees, OR"
    echo "  4. Modified Ring 7 test behavior"
    echo ""
    echo "This is a CONSENSUS BREAK and MUST be reverted."
    echo ""
    echo "Allowed changes (see docs/consensus/RING8_PHASE8A.md):"
    echo "  ✅ Refactoring that preserves ALL Ring 7 properties"
    echo "  ✅ Performance optimizations that pass ALL Ring 7 tests"
    echo "  ✅ New script versions (gated, Ring 8b)"
    echo ""
    echo "Forbidden changes:"
    echo "  ❌ Opcode redefinition"
    echo "  ❌ Semantic reinterpretation"
    echo "  ❌ Behavioral modification"
    echo "  ❌ Ring 7 test alterations"
    echo ""
    echo "See: docs/consensus/RING7_FREEZE.md"
    echo "See: docs/consensus/RING8_PHASE8A.md"
    echo ""
    exit 1
fi
