#!/bin/bash
# CI Script: Consensus Hygiene Checks
# Prevents accidental reintroduction of hardcoded difficulty values or
# direct bits assignments outside the canonical GetNextWorkRequired() selector.
#
# Usage: .github/scripts/check_consensus_hygiene.sh
# Exit code: 0 = pass, 1 = violations found

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

echo "=== Consensus Hygiene Checks ==="
echo "Repository: $REPO_ROOT"
echo ""

FAILED=0

# Check 1: No direct difficulty bits assignments outside allowed contexts
echo "[Check 1] Scanning for suspicious difficulty bits assignments..."
# Only flag: .bits= assignments OR assignments of difficulty hex literals (0x1d.../0x1f...)
# Exclude: crypto files, codecs, address files, bech32, hash implementations, test setup, miner SHA256 state
BITS_ASSIGNMENTS=$(rg -n -e '\.bits\s*=\s*0x1[dDfF][0-9a-fA-F]{6}' -e '=\s*0x1[dDfF][0-9a-fA-F]{6}\s*;' src 2>/dev/null | \
    rg -v 'src/crypto/' | \
    rg -v 'src/core/crypto/' | \
    rg -v 'bech32' | \
    rg -v 'address' | \
    rg -v 'Address' | \
    rg -v 'codec' | \
    rg -v 'ripemd' | \
    rg -v 'sha256' | \
    rg -v 'dinero_crypto' | \
    rg -v 'consensus/consensus.hpp' | \
    rg -v 'src/consensus/chainparams_impl.cpp:.*g\.nBits' | \
    rg -v 'consensus\.easyPhaseBits' | \
    rg -v 'c\.easyPhaseBits\s*=' | \
    rg -v 'state\[' | \
    rg -v '\.bak' | \
    rg -v '\.backup' | \
    rg -v 'db_init_example' | \
    rg -v '//' | \
    rg -v '/\*' || true)

if [ -n "$BITS_ASSIGNMENTS" ]; then
    echo "❌ FAIL: Found direct bits assignments outside selector:"
    echo "$BITS_ASSIGNMENTS"
    echo ""
    echo "Bits must only be assigned by GetNextWorkRequired() or formatting functions."
    echo "See docs/CONSENSUS.md for details."
    FAILED=1
else
    echo "✅ PASS: No unauthorized bits assignments found"
fi
echo ""

# Check 2: No legacy 0x1d00ffff constant (the original bug)
echo "[Check 2] Scanning for legacy 0x1d00ffff constant in active code..."
LEGACY_CONSTANT=$(rg -n '0x1d00ffff' src 2>/dev/null | \
    rg -v '\.bak' | \
    rg -v '\.backup' | \
    rg -v 'historical' | \
    rg -v 'was 0x1d00ffff' | \
    rg -v 'incorrect.*0x1d00ffff' | \
    rg -v 'legacy.*0x1d00ffff' | \
    rg -v 'Bitcoin level' | \
    rg -v '"0x1d00ffff' | \
    rg -v 'db_init_example' | \
    rg -v 'headers_first_sync' | \
    rg -v 'chainparams' | \
    rg -v 'genesis' | \
    rg -v 'src/consensus/pow.cpp:.*\* Example: bits = 0x1d00ffff' | \
    rg -v 'src/mining/mining_coordinator.cpp:.*\* difficulty=1 means.*0x1d00ffff' | \
    rg -v 'test' | \
    rg -v '//' | \
    rg -v '/\*' || true)

if [ -n "$LEGACY_CONSTANT" ]; then
    echo "❌ FAIL: Found legacy 0x1d00ffff constant:"
    echo "$LEGACY_CONSTANT"
    echo ""
    echo "This value caused the original getblocktemplate bug."
    echo "Phase 1 uses 0x1d3fffff. Use easyPhaseBits from Consensus struct."
    FAILED=1
else
    echo "✅ PASS: No legacy 0x1d00ffff constants found"
fi
echo ""

# Check 3: No hardcoded Phase 1 constant outside consensus params
echo "[Check 3] Scanning for hardcoded 0x1d3fffff outside consensus params..."
HARDCODED_PHASE1=$(rg -n '0x1d3fffff' src 2>/dev/null | \
    rg -v 'consensus' | \
    rg -v 'test' | \
    rg -v 'Consensus' | \
    rg -v 'expected' | \
    rg -v '\.bak' | \
    rg -v '"0x1d3fffff' | \
    rg -v 'genesis_init' | \
    rg -v '//' | \
    rg -v '/\*' || true)

if [ -n "$HARDCODED_PHASE1" ]; then
    echo "⚠️  WARNING: Found hardcoded 0x1d3fffff outside consensus params:"
    echo "$HARDCODED_PHASE1"
    echo ""
    echo "Consider using consensus.easyPhaseBits instead of magic numbers."
    # Not failing build, just warning
fi
echo ""

# Check 4: Verify GetNextWorkRequired is called in GBT handler
echo "[Check 4] Verifying GetNextWorkRequired is used in getblocktemplate..."
GBT_USES_SELECTOR=$(rg -n 'GetNextWorkRequiredWithChainDB' \
    src/daemon/rpc_server.cpp || true)

if [ -z "$GBT_USES_SELECTOR" ]; then
    echo "❌ FAIL: GetNextWorkRequiredWithChainDB not found in rpc_server.cpp"
    echo "The GBT handler must use the canonical difficulty selector."
    FAILED=1
else
    echo "✅ PASS: GetNextWorkRequired is used in daemon"
fi
echo ""

# Check 5: Verify BIP22 formatting (8-char hex, no 0x prefix)
echo "[Check 5] Verifying BIP22 bits formatting..."
BIP22_FORMAT=$(rg -n 'snprintf.*%08x.*next_bits' \
    src/daemon/rpc_server.cpp || true)

if [ -z "$BIP22_FORMAT" ]; then
    echo "⚠️  WARNING: Could not verify BIP22 bits formatting in rpc_server.cpp"
    echo "Expected: snprintf(bits_hex, sizeof(bits_hex), \"%08x\", next_bits);"
else
    echo "✅ PASS: BIP22 formatting pattern found"
fi
echo ""

# Summary
echo "=== Summary ==="
if [ $FAILED -eq 0 ]; then
    echo "✅ All consensus hygiene checks passed"
    exit 0
else
    echo "❌ Consensus hygiene violations detected"
    echo ""
    echo "Fix the violations above before merging."
    echo "See docs/CONSENSUS.md for guidance."
    exit 1
fi
