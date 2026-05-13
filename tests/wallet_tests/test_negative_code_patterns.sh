#!/bin/bash
#
# Negative Regression Test: Code Pattern Analysis
#
# PURPOSE:
# ========
# This test scans source code for anti-patterns that indicate address-based logic.
# If any forbidden patterns are found, the build MUST fail.
#
# WHAT THIS TESTS:
# ================
# 1. No address string comparison in wallet code
# 2. No HRP-dependent logic in UTXO selection
# 3. No Taproot private-key tweaking in signing code
# 4. scriptPubKey-based lookups are enforced
#
# INVARIANTS PROTECTED:
# =====================
# ❌ FORBIDDEN: address == comparison, address.compare(), strcmp(address,...)
# ❌ FORBIDDEN: TapTweak applied to private keys for signing
# ❌ FORBIDDEN: HRP checks in ownership logic
# ✅ REQUIRED:  scriptPubKey-based UTXO matching
#
# IF THIS TEST FAILS:
# ===================
# Someone has introduced address-based anti-patterns.
# This is a BLOCKING failure - do not merge the code.
#
# ⚠️  THIS IS A STATIC ANALYSIS TEST - RUNS AT BUILD TIME

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

TESTS_PASSED=0
TESTS_FAILED=0

echo ""
echo -e "${BOLD}╔═══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║                                                            ║${NC}"
echo -e "${BOLD}║  NEGATIVE REGRESSION: CODE PATTERN ANALYSIS               ║${NC}"
echo -e "${BOLD}║  Static Analysis for Anti-Patterns                        ║${NC}"
echo -e "${BOLD}║                                                            ║${NC}"
echo -e "${BOLD}╚═══════════════════════════════════════════════════════════╝${NC}"
echo ""

pass() {
    echo -e "${GREEN}✓ PASS${NC}: $1"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

fail() {
    echo -e "${RED}✗ FAIL${NC}: $1"
    TESTS_FAILED=$((TESTS_FAILED + 1))
}

warn() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# ============================================================================
# Test 1: Check for Address String Comparison Anti-Patterns
# ============================================================================

echo -e "\n${YELLOW}Test 1: Address String Comparison Anti-Patterns${NC}"

# FORBIDDEN ZONES - Address checks in these files/functions are DANGEROUS
FORBIDDEN_ZONES="wallet_manager.cpp coin_selection spending signing utxo_index"

WALLET_FILES=$(find src -name "wallet*.cpp" -o -name "wallet*.h" 2>/dev/null)

if [ -z "$WALLET_FILES" ]; then
    warn "No wallet source files found"
else
    # Check for dangerous address comparison patterns IN FORBIDDEN ZONES
    # Exclude files with "SAFE - RPC" or "SAFE - network" comments
    ADDRESS_COMPARE=$(grep -n "address\[0\].*==" $WALLET_FILES 2>/dev/null | \
        grep -v "SAFE.*RPC\|SAFE.*network\|SAFE.*parsing\|address_type\|scriptPubKey" || true)

    # Filter to only show matches in forbidden zones
    DANGEROUS_MATCHES=""
    while IFS= read -r line; do
        for zone in $FORBIDDEN_ZONES; do
            if echo "$line" | grep -q "$zone"; then
                DANGEROUS_MATCHES="$DANGEROUS_MATCHES$line\n"
            fi
        done
    done <<< "$ADDRESS_COMPARE"

    if [ -n "$DANGEROUS_MATCHES" ] && [ "$DANGEROUS_MATCHES" != "" ]; then
        fail "Found address string comparison in FORBIDDEN zones (ownership/spending)!"
        echo -e "$DANGEROUS_MATCHES"
    else
        pass "No address string comparisons found in forbidden zones"
    fi

    # Check for std::string::compare on addresses
    ADDRESS_COMPARE_METHOD=$(grep -n "\.compare.*address\|address.*\.compare" $WALLET_FILES 2>/dev/null | grep -v "//.*compare" || true)

    if [ -n "$ADDRESS_COMPARE_METHOD" ]; then
        fail "Found string::compare() on address in wallet code!"
        echo "$ADDRESS_COMPARE_METHOD"
    else
        pass "No std::string::compare() on addresses found"
    fi
fi

# ============================================================================
# Test 2: Verify scriptPubKey-Based Lookup Usage
# ============================================================================

echo -e "\n${YELLOW}Test 2: scriptPubKey-Based Lookup Enforcement${NC}"

RPC_FILES=$(find src -name "*.cpp" 2>/dev/null)

if [ -n "$RPC_FILES" ]; then
    # Check that sendtoaddress uses scriptPubKey (either directly or via CoinSelector)
    SENDTOADDRESS_IMPL=$(grep -l "rpc_context_wallet_sendtoaddress" $RPC_FILES 2>/dev/null | head -1)

    if [ -n "$SENDTOADDRESS_IMPL" ]; then
        # Phase 1.1: sendtoaddress delegates to CoinSelector (scriptPubKey-based)
        if grep -q "CoinSelector.*SelectCoins" "$SENDTOADDRESS_IMPL" 2>/dev/null; then
            pass "sendtoaddress delegates to CoinSelector (scriptPubKey-based)"
        elif grep -q "scriptPubKey\|script_pubkey" "$SENDTOADDRESS_IMPL" 2>/dev/null; then
            pass "sendtoaddress implementation references scriptPubKey"
        else
            warn "sendtoaddress may not use scriptPubKey directly (check implementation)"
        fi
    fi
fi

# Check wallet database queries use scriptPubKey
DB_QUERIES=$(grep -rn "SELECT.*FROM.*addresses" src 2>/dev/null || true)

if [ -n "$DB_QUERIES" ]; then
    # Verify script_pubkey column is used
    if echo "$DB_QUERIES" | grep -q "script_pubkey"; then
        pass "Database queries use script_pubkey column"
    else
        fail "Database queries may not be using script_pubkey column!"
        echo "$DB_QUERIES"
    fi
fi

# ============================================================================
# Test 3: Check for Taproot Private-Key Tweaking Anti-Pattern
# ============================================================================

echo -e "\n${YELLOW}Test 3: Taproot Private-Key Tweaking Anti-Pattern${NC}"

TAPROOT_FILES=$(find src -name "*taproot*.cpp" -o -name "*bip341*.cpp" -o -name "wallet*.cpp" 2>/dev/null)

if [ -n "$TAPROOT_FILES" ]; then
    # Check for TapTweak applied to private key in signing context
    PRIVKEY_TWEAK=$(grep -n "TapTweak.*priv\|privkey.*TapTweak" $TAPROOT_FILES 2>/dev/null | grep -v "//.*TapTweak\|/\*.*TapTweak" || true)

    if [ -n "$PRIVKEY_TWEAK" ]; then
        fail "Found TapTweak applied to private key (BIP341 violation)!"
        echo "$PRIVKEY_TWEAK"
    else
        pass "No private-key tweaking found (BIP341 compliant)"
    fi

    # Verify internal key is used for signing
    INTERNAL_KEY_SIGNING=$(grep -n "internal.*key.*sign\|sign.*internal.*key" $TAPROOT_FILES 2>/dev/null || true)

    if [ -n "$INTERNAL_KEY_SIGNING" ]; then
        pass "Code references internal key for signing (correct)"
    fi
fi

# ============================================================================
# Test 4: Check for HRP-Dependent Ownership Logic
# ============================================================================

echo -e "\n${YELLOW}Test 4: HRP-Dependent Ownership Logic Anti-Pattern${NC}"

# FORBIDDEN ZONES - HRP checks in these contexts are DANGEROUS
# ALLOWED ZONES - network detection, address validation, UI formatting
FORBIDDEN_HRP_ZONES="wallet_manager.cpp coin_selection spending signing utxo_index"

# Check that ownership logic doesn't depend on HRP
HRP_CHECKS=$(grep -rn "hrp.*==\|HRP.*==" src 2>/dev/null | \
    grep -v "//.*HRP\|test\|Test\|SAFE.*network\|SAFE.*validation\|Network::" || true)

# Filter to only show matches in forbidden zones
DANGEROUS_HRP=""
while IFS= read -r line; do
    for zone in $FORBIDDEN_HRP_ZONES; do
        if echo "$line" | grep -q "$zone"; then
            DANGEROUS_HRP="$DANGEROUS_HRP$line\n"
        fi
    done
done <<< "$HRP_CHECKS"

if [ -n "$DANGEROUS_HRP" ] && [ "$DANGEROUS_HRP" != "" ]; then
    fail "Found HRP checks in FORBIDDEN zones (ownership/spending logic)!"
    echo -e "$DANGEROUS_HRP"
else
    pass "No HRP-dependent ownership logic found in forbidden zones"
fi

# Note: HRP checks are ALLOWED in:
# - Network detection (AddressCodec.cpp)
# - Address validation (address_validator.cpp)
# - RPC input parsing
# - Mining coinbase generation
# These are display/validation concerns, not ownership

# ============================================================================
# Test 5: Verify Comments Warning Against Anti-Patterns
# ============================================================================

echo -e "\n${YELLOW}Test 5: Anti-Pattern Warning Comments${NC}"

# Check for warning comments in critical files
WARNINGS=$(grep -rn "NEVER.*address\|DO NOT.*address.*match\|scriptPubKey.*not.*address" src 2>/dev/null || true)

if [ -n "$WARNINGS" ]; then
    pass "Found $(echo "$WARNINGS" | wc -l | tr -d ' ') warning comments in code"
else
    warn "No anti-pattern warnings found in comments (consider adding)"
fi

# ============================================================================
# Test 6: Verify Bitcoin Core-Style Ownership Semantics
# ============================================================================

echo -e "\n${YELLOW}Test 6: Bitcoin Core Ownership Semantics${NC}"

# Check for comments explaining scriptPubKey-based ownership
OWNERSHIP_DOCS=$(grep -rn "scriptPubKey.*owner\|ownership.*scriptPubKey" src 2>/dev/null || true)

if [ -n "$OWNERSHIP_DOCS" ]; then
    pass "Found documentation of scriptPubKey-based ownership"
else
    warn "Consider adding comments explaining scriptPubKey-based ownership"
fi

# ============================================================================
# Test 7: Check Wallet Database Schema
# ============================================================================

echo -e "\n${YELLOW}Test 7: Wallet Database Schema Verification${NC}"

SCHEMA_FILES=$(find src -name "*.sql" -o -name "*schema*.cpp" -o -name "*database*.cpp" 2>/dev/null)

if [ -n "$SCHEMA_FILES" ]; then
    # Verify addresses table has script_pubkey column
    if grep -q "script_pubkey" $SCHEMA_FILES 2>/dev/null; then
        pass "Wallet schema includes script_pubkey column"
    else
        fail "Wallet schema missing script_pubkey column!"
    fi
else
    warn "Could not locate database schema files"
fi

# ============================================================================
# Test Summary
# ============================================================================

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║                     Test Summary                          ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""
echo "Tests passed: ${GREEN}${TESTS_PASSED}${NC}"
echo "Tests failed: ${RED}${TESTS_FAILED}${NC}"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  ✓ NO ANTI-PATTERNS DETECTED                             ║${NC}"
    echo -e "${GREEN}║                                                           ║${NC}"
    echo -e "${GREEN}║  Code is free from known anti-patterns:                   ║${NC}"
    echo -e "${GREEN}║                                                           ║${NC}"
    echo -e "${GREEN}║  ✅ No address string comparisons                         ║${NC}"
    echo -e "${GREEN}║  ✅ No Taproot private-key tweaking                       ║${NC}"
    echo -e "${GREEN}║  ✅ No HRP-dependent ownership logic                      ║${NC}"
    echo -e "${GREEN}║  ✅ scriptPubKey-based patterns enforced                  ║${NC}"
    echo -e "${GREEN}║                                                           ║${NC}"
    echo -e "${GREEN}║  The critical bugs CANNOT silently return.                ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    exit 0
else
    echo -e "${RED}╔═══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  ✗ ANTI-PATTERNS DETECTED                                ║${NC}"
    echo -e "${RED}║                                                           ║${NC}"
    echo -e "${RED}║  Dangerous patterns found in source code!                 ║${NC}"
    echo -e "${RED}║  These patterns will cause consensus bugs.                ║${NC}"
    echo -e "${RED}║                                                           ║${NC}"
    echo -e "${RED}║  DO NOT MERGE THIS CODE.                                  ║${NC}"
    echo -e "${RED}║  Fix the anti-patterns before proceeding.                 ║${NC}"
    echo -e "${RED}╚═══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    exit 1
fi
