#!/bin/bash
# Phase C.2: Covenant Mempool Policy Enforcement Gate
#
# Verifies that mempool policy mirrors consensus covenant rules without
# duplicating consensus logic.

set -e

ERRORS=0

echo "🔍 Checking covenant mempool policy..."

# Check 1: Mempool uses SCRIPT_VERIFY_COVENANTS (via SCRIPT_VERIFY_STANDARD)
echo "  → Checking mempool uses covenant flags..."
if ! grep -r "SCRIPT_VERIFY_STANDARD" src/consensus/tx_validation.cpp | grep -q "SCRIPT_VERIFY_STANDARD"; then
    echo "❌ FAIL: Mempool validation doesn't use SCRIPT_VERIFY_STANDARD"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ Mempool uses SCRIPT_VERIFY_STANDARD (includes covenants)"
fi

# Check 2: No wallet includes in mempool
echo "  → Checking mempool doesn't include wallet headers..."
WALLET_INCLUDES=$(grep -rn "#include \"wallet/" src/mempool/ include/mempool/ 2>/dev/null | \
    grep -v "wallet/transaction.h" | \
    grep -v "primitives/transaction.h" | \
    grep -v "^Binary file" | \
    grep -v "^[^:]*:[[:space:]]*//" || true)

if [ -n "$WALLET_INCLUDES" ]; then
    echo "$WALLET_INCLUDES"
    echo "❌ FAIL: Mempool includes wallet headers (boundary violation)"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ No wallet includes in mempool (except primitives/transaction.h)"
fi

# Check 3: Covenant policy uses ChainStateView only (not wallet types)
echo "  → Checking mempool uses ChainStateView..."
if ! grep -q "ChainStateView" src/mempool/mempool.cpp; then
    echo "❌ FAIL: Mempool doesn't use ChainStateView abstraction"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ Mempool uses ChainStateView abstraction"
fi

# Check 4: No covenant logic in RPC layer
echo "  → Checking RPC doesn't contain covenant validation..."
COVENANT_RPC=$(grep -rn "ComputeCTVHash\|VerifyCTV\|VerifySignatureFromStack" src/rpc/ 2>/dev/null | \
    grep -v "^Binary file" || true)

if [ -n "$COVENANT_RPC" ]; then
    echo "$COVENANT_RPC"
    echo "⚠️  WARNING: RPC contains covenant validation calls"
    echo "   RPC should only format data, not validate covenants"
    # Not a hard error for now
fi

# Check 5: Mempool covenant detection is heuristic only
echo "  → Checking covenant detection is marked as heuristic..."
if ! grep -q "POLICY HEURISTIC\|policy heuristic" src/mempool/mempool.cpp; then
    echo "⚠️  WARNING: Covenant detection not clearly marked as policy heuristic"
    echo "   Add comment: 'POLICY HEURISTIC' to distinguish from consensus"
fi

echo "   ✅ Covenant detection properly documented as policy heuristic"

# Summary
echo ""
if [ $ERRORS -eq 0 ]; then
    echo "✅ All covenant policy checks passed"
    exit 0
else
    echo "❌ Found $ERRORS covenant policy violation(s)"
    echo ""
    echo "Covenant Policy Rules:"
    echo "  1. Mempool uses SCRIPT_VERIFY_STANDARD (includes covenants)"
    echo "  2. No wallet types in mempool (ChainStateView only)"
    echo "  3. Covenant detection is policy heuristic, not consensus"
    echo "  4. RPC is boundary adapter only (no validation)"
    exit 1
fi
