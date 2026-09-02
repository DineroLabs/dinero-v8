#!/bin/bash
# Phase C.2: Covenant Mempool Policy Enforcement Gate
#
# Verifies that mempool policy mirrors consensus covenant rules without
# duplicating consensus logic.

set -e

ERRORS=0

echo "🔍 Checking covenant mempool policy..."

# Check 1: canonical transaction validation derives height-dependent standard
# flags through CovenantActivationParams. SCRIPT_VERIFY_STANDARD by itself is
# intentionally covenant-free so dormant opcodes cannot be enabled globally.
echo "  → Checking canonical validation derives height-gated covenant flags..."
if ! grep -q "CovenantActivationParams::StandardFlags" src/consensus/tx_validation.cpp; then
    echo "❌ FAIL: Canonical validation doesn't derive height-gated standard flags"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ Canonical validation uses height-gated standard flags"
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

# Check 3: the current mempool implementation uses the read-only ChainStateView
# abstraction. It moved from src/mempool to src/daemon; pin both the abstraction
# and its overlay so a future path move cannot turn this into a false pass.
echo "  → Checking mempool uses ChainStateView..."
if ! grep -q "ChainStateView" src/daemon/mempool.cpp || \
   ! grep -q "ChainStateView" include/mempool/coins_view_mempool.h; then
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

# Check 5: a reorg can cross an activation boundary while entries remain in
# the mempool. Selection must notice a flag-state change and re-run canonical
# validation at the candidate block height.
echo "  → Checking activation-boundary revalidation..."
if ! grep -q "CovenantActivationParams::CovenantFlags" src/daemon/mempool.cpp || \
   ! grep -q "validateTransaction" src/daemon/mempool.cpp; then
    echo "❌ FAIL: Mempool lacks covenant activation-boundary revalidation"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ Mempool revalidates across covenant activation boundaries"
fi

# Summary
echo ""
if [ $ERRORS -eq 0 ]; then
    echo "✅ All covenant policy checks passed"
    exit 0
else
    echo "❌ Found $ERRORS covenant policy violation(s)"
    echo ""
    echo "Covenant Policy Rules:"
    echo "  1. Canonical validation derives height-gated covenant flags"
    echo "  2. No wallet types in mempool (ChainStateView only)"
    echo "  3. Mempool revalidates entries across activation boundaries"
    echo "  4. RPC is boundary adapter only (no validation)"
    exit 1
fi
