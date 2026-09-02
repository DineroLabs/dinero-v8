#!/bin/bash
# Phase C.1: Covenant Boundary Enforcement Gate
#
# Ensures wallet layer NEVER validates covenant consensus rules.
# This is a critical architectural boundary that must be maintained.
#
# Rule: Wallet MUST NOT call consensus validation functions.
# Validation happens ONLY in consensus::ScriptInterpreter.

set -e

ERRORS=0

echo "🔍 Checking covenant boundary violations..."

# Check 1: Wallet must NOT call consensus covenant VERIFICATION functions
# Note: ComputeCTVHash is allowed for template CREATION, forbidden for VALIDATION
echo "  → Checking wallet layer for consensus validation calls..."
VALIDATION_CALLS=$(grep -rn "consensus::VerifyCTV\|consensus::VerifySignatureFromStack\|consensus::VerifyContractTransition" \
    src/wallet/ include/wallet/ 2>/dev/null | grep -v "^Binary file" | grep -v "//.*consensus::" | grep -v "\*.*consensus::" || true)

if [ -n "$VALIDATION_CALLS" ]; then
    echo "$VALIDATION_CALLS"
    echo "❌ FAIL: Wallet calling consensus verification functions"
    echo "   Wallet must ONLY construct transactions, NEVER validate covenant rules"
    echo "   See include/wallet/covenant_wallet.h for boundary rules"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ No consensus verification calls in wallet layer"
fi

# Check 1b: Wallet can use ComputeCTVHash for template CREATION but not VALIDATION
echo "  → Checking ComputeCTVHash usage context..."
# Find files with ComputeCTVHash calls
COMPUTE_FILES=$(grep -Elr "consensus::(Try)?ComputeCTVHash" src/wallet/ 2>/dev/null || true)
CALL_COUNT=$(grep -Er "consensus::(Try)?ComputeCTVHash" src/wallet/ 2>/dev/null | grep -v "^Binary file" | wc -l | tr -d ' ')

if [ "$CALL_COUNT" -eq 0 ]; then
    echo "   ✅ No ComputeCTVHash calls in wallet layer"
else
    # Check each file contains CONSTRUCTION or ALLOWED marker
    UNMARKED=0
    for file in $COMPUTE_FILES; do
        if ! grep -q "CONSTRUCTION\|ALLOWED" "$file" 2>/dev/null; then
            echo "   ⚠️  File $file missing CONSTRUCTION marker"
            UNMARKED=$((UNMARKED + 1))
        fi
    done

    if [ "$UNMARKED" -gt 0 ]; then
        echo "❌ FAIL: Found ComputeCTVHash without CONSTRUCTION/ALLOWED marker"
        echo "   Add comment: '// Phase C.3: CONSTRUCTION ONLY - computing hash for template building'"
        ERRORS=$((ERRORS + 1))
    else
        echo "   ✅ Found $CALL_COUNT ComputeCTVHash call(s) (all marked for template CREATION)"
    fi
fi

# Check 2: Mempool must NOT call consensus validation functions (policy only)
echo "  → Checking mempool layer for consensus validation calls..."
if grep -rn "consensus::VerifyCTV\|consensus::VerifySignatureFromStack\|consensus::VerifyContractTransition" \
    src/mempool/ include/mempool/ 2>/dev/null | grep -v "^Binary file" | grep -v "//.*consensus::" | grep -v "\*.*consensus::"; then
    echo "❌ FAIL: Mempool calling consensus validation functions"
    echo "   Mempool must ONLY apply policy rules, NOT validate covenant consensus"
    echo "   Validation happens in consensus::ScriptInterpreter during block validation"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ No consensus validation calls in mempool layer"
fi

# Check 3: Wallet must NOT have methods named "validate*Covenant*"
echo "  → Checking wallet layer for validation methods..."
if grep -rn "validate.*[Cc]ovenant\|[Vv]erify.*[Cc]ovenant" \
    src/wallet/ include/wallet/ 2>/dev/null | \
    grep -v "^Binary file" | \
    grep -v "//" | \
    grep -E "(bool|ValidationResult|VerificationResult).*validate.*[Cc]ovenant"; then
    echo "❌ FAIL: Wallet contains covenant validation methods"
    echo "   Wallet must NOT return 'valid/invalid' based on covenant rules"
    echo "   Use 'check*', 'analyze*', or 'build*' for UX helpers instead"
    ERRORS=$((ERRORS + 1))
else
    echo "   ✅ No covenant validation methods in wallet layer"
fi

# Summary
echo ""
if [ $ERRORS -eq 0 ]; then
    echo "✅ All covenant boundary checks passed"
    exit 0
else
    echo "❌ Found $ERRORS covenant boundary violation(s)"
    echo ""
    echo "Covenant Boundary Rules:"
    echo "  1. Wallet ONLY constructs transactions (createCTVTemplate, buildTx, etc.)"
    echo "  2. Wallet NEVER validates covenant rules (no validateCovenantTx)"
    echo "  3. Mempool ONLY applies policy (limits, fees), NOT validation"
    echo "  4. Validation happens EXCLUSIVELY in consensus::ScriptInterpreter"
    echo ""
    echo "See include/wallet/covenant_wallet.h for complete boundary documentation"
    exit 1
fi
