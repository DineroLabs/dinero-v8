#!/usr/bin/env bash
# ============================================================================
# Verify Lightning Network Dependency Checksums
# Security verification for vendored cryptographic libraries
# ============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Lightning Network Dependency Verification${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# Track results
VERIFIED=0
FAILED=0
SKIPPED=0

# ============================================================================
# Helper Functions
# ============================================================================

verify_library() {
    local name="$1"
    local lib_path="$2"
    local expected_hash="$3"

    if [ ! -f "$lib_path" ]; then
        echo -e "${YELLOW}  ⚠️  $name - SKIPPED (not built)${NC}"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    echo -e "${BLUE}  🔍 Verifying $name...${NC}"

    # Calculate SHA256
    if command -v sha256sum &> /dev/null; then
        actual_hash=$(sha256sum "$lib_path" | cut -d' ' -f1)
    elif command -v shasum &> /dev/null; then
        actual_hash=$(shasum -a 256 "$lib_path" | cut -d' ' -f1)
    else
        echo -e "${RED}     ❌ sha256sum/shasum not found${NC}"
        FAILED=$((FAILED + 1))
        return
    fi

    # Compare hashes
    if [ "$actual_hash" = "$expected_hash" ]; then
        echo -e "${GREEN}     ✅ SHA256 match${NC}"
        echo -e "     Hash: ${actual_hash:0:16}...${actual_hash:48}"
        VERIFIED=$((VERIFIED + 1))
    else
        echo -e "${RED}     ❌ SHA256 MISMATCH${NC}"
        echo -e "     Expected: $expected_hash"
        echo -e "     Actual:   $actual_hash"
        FAILED=$((FAILED + 1))
    fi
}

# ============================================================================
# Verify Each Lightning Library
# ============================================================================

# Note: These are example hashes - replace with actual hashes from your builds
# You can generate them with: sha256sum <library_path>

echo -e "${BLUE}Checking Lightning Network libraries...${NC}"
echo ""

# libwally-core
LIBWALLY_LIB="$PROJECT_ROOT/third_party/libwally-core/src/.libs/libwallycore.a"
# Replace this hash with your actual build hash
LIBWALLY_HASH="1d517946de536afb7fc51c8c260dcfb483697c9c15e559231137e1754f68345a"

if [ "$LIBWALLY_HASH" = "REPLACE_WITH_ACTUAL_SHA256_HASH_FROM_YOUR_BUILD" ]; then
    echo -e "${YELLOW}  ℹ️  libwally-core - Hash verification disabled (first-time setup)${NC}"
    if [ -f "$LIBWALLY_LIB" ]; then
        if command -v sha256sum &> /dev/null; then
            actual=$(sha256sum "$LIBWALLY_LIB" | cut -d' ' -f1)
        elif command -v shasum &> /dev/null; then
            actual=$(shasum -a 256 "$LIBWALLY_LIB" | cut -d' ' -f1)
        fi
        echo -e "     Current hash: $actual"
        echo -e "     ${YELLOW}Update LIBWALLY_HASH in this script to enable verification${NC}"
    fi
    SKIPPED=$((SKIPPED + 1))
else
    verify_library "libwally-core" "$LIBWALLY_LIB" "$LIBWALLY_HASH"
fi

echo ""

# secp256k1-zkp
SECP256K1_ZKP_LIB="$PROJECT_ROOT/third_party/secp256k1-zkp/.libs/libsecp256k1.a"
SECP256K1_ZKP_HASH="e1342f264663d476e05fcff8daa68cfb3c63f5cd2ad04ba5497db6e7a2ad2767"

if [ "$SECP256K1_ZKP_HASH" = "REPLACE_WITH_ACTUAL_SHA256_HASH_FROM_YOUR_BUILD" ]; then
    echo -e "${YELLOW}  ℹ️  secp256k1-zkp - Hash verification disabled (first-time setup)${NC}"
    if [ -f "$SECP256K1_ZKP_LIB" ]; then
        if command -v sha256sum &> /dev/null; then
            actual=$(sha256sum "$SECP256K1_ZKP_LIB" | cut -d' ' -f1)
        elif command -v shasum &> /dev/null; then
            actual=$(shasum -a 256 "$SECP256K1_ZKP_LIB" | cut -d' ' -f1)
        fi
        echo -e "     Current hash: $actual"
        echo -e "     ${YELLOW}Update SECP256K1_ZKP_HASH in this script to enable verification${NC}"
    fi
    SKIPPED=$((SKIPPED + 1))
else
    verify_library "secp256k1-zkp" "$SECP256K1_ZKP_LIB" "$SECP256K1_ZKP_HASH"
fi

echo ""

# blake3
BLAKE3_LIB="$PROJECT_ROOT/third_party/blake3/c/build/libblake3.a"
BLAKE3_HASH="621b4d82371ae929159e5bddfb65d5bde0528f5de35c7d57cbcec6ffc71a3a77"

if [ "$BLAKE3_HASH" = "REPLACE_WITH_ACTUAL_SHA256_HASH_FROM_YOUR_BUILD" ]; then
    echo -e "${YELLOW}  ℹ️  blake3 - Hash verification disabled (first-time setup)${NC}"
    if [ -f "$BLAKE3_LIB" ]; then
        if command -v sha256sum &> /dev/null; then
            actual=$(sha256sum "$BLAKE3_LIB" | cut -d' ' -f1)
        elif command -v shasum &> /dev/null; then
            actual=$(shasum -a 256 "$BLAKE3_LIB" | cut -d' ' -f1)
        fi
        echo -e "     Current hash: $actual"
        echo -e "     ${YELLOW}Update BLAKE3_HASH in this script to enable verification${NC}"
    fi
    SKIPPED=$((SKIPPED + 1))
else
    verify_library "blake3" "$BLAKE3_LIB" "$BLAKE3_HASH"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Verification Summary${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

echo -e "  ✅ Verified: $VERIFIED"
echo -e "  ⚠️  Skipped:  $SKIPPED"

if [ $FAILED -gt 0 ]; then
    echo -e "  ${RED}❌ Failed:   $FAILED${NC}"
    echo ""
    echo -e "${RED}⚠️  WARNING: Some libraries failed SHA256 verification!${NC}"
    echo -e "${RED}This could indicate tampering or build inconsistencies.${NC}"
    echo ""
    echo -e "${YELLOW}Recommended actions:${NC}"
    echo -e "  1. Re-build the failed library from scratch"
    echo -e "  2. Verify you're using the correct git commit/tag"
    echo -e "  3. Check for compiler flag differences"
    echo -e "  4. Update expected hashes in this script if intentional"
    echo ""
    exit 1
else
    echo ""
    if [ $VERIFIED -gt 0 ]; then
        echo -e "${GREEN}✅ All verified libraries passed SHA256 checks!${NC}"
    else
        echo -e "${YELLOW}ℹ️  No libraries verified (all skipped - first-time setup)${NC}"
        echo ""
        echo -e "${YELLOW}To enable verification:${NC}"
        echo -e "  1. Build Lightning libraries:"
        echo -e "     ./scripts/vendor-libwally.sh"
        echo -e "     ./scripts/vendor-secp256k1-zkp.sh"
        echo -e "     ./scripts/vendor-blake3.sh"
        echo ""
        echo -e "  2. Run this script to see current hashes"
        echo ""
        echo -e "  3. Update hash constants in:"
        echo -e "     scripts/verify-lightning-checksums.sh"
    fi
    echo ""
fi

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
