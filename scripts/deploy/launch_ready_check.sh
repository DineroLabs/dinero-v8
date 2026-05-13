#!/bin/bash
# ==============================================================================
# DINERO LAUNCH READY CHECK
# ==============================================================================
#
# Quick validation that everything is ready for mainnet launch
# macOS compatible version without flock
#
# ==============================================================================

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "🚦 DINERO LAUNCH READY CHECK"
echo "============================"
echo "Timestamp: $(date)"
echo ""

READY=true
CHECKS_TOTAL=0
CHECKS_PASSED=0

# Helper function for checks
check() {
    local name="$1"
    local command="$2"
    CHECKS_TOTAL=$((CHECKS_TOTAL + 1))
    
    echo -n "🔍 $name... "
    
    if eval "$command" >/dev/null 2>&1; then
        echo "✅ PASS"
        CHECKS_PASSED=$((CHECKS_PASSED + 1))
    else
        echo "❌ FAIL"
        READY=false
    fi
}

# =============================================================================
# ESSENTIAL LAUNCH CHECKS
# =============================================================================

echo "📋 ESSENTIAL LAUNCH CHECKS"
echo "--------------------------"

# 1. HLC_Drive and premine keys
check "HLC_Drive mounted" "test -d /Volumes/HLC_Drive"
check "Premine keys exist" "test -f /Volumes/HLC_Drive/dinero_secure/keys/dinero_premine_keys.enc"
check "Public info exists" "test -f /Volumes/HLC_Drive/dinero_secure/keys/public_info.txt"

# 2. Build system
check "Build directory exists" "test -d build"
check "dinerod binary exists" "test -x build/dinerod"
check "dinero-cli exists" "test -x build/dinero-cli"

# 3. Scripts ready
check "Genesis ceremony script" "test -x scripts/deploy/genesis_ceremony.sh"
check "Post-launch checks script" "test -x scripts/deploy/post_launch_checks.sh"
check "GO_LIVE script" "test -x scripts/deploy/GO_LIVE.sh"

# 4. Integration files
check "Integration instructions" "test -f /Volumes/HLC_Drive/dinero_secure/docs/INTEGRATION_INSTRUCTIONS.md"
check "Genesis coinbase code" "test -f /Volumes/HLC_Drive/dinero_secure/docs/genesis_coinbase_integration.cpp"

echo ""

# =============================================================================
# PREMINE VALIDATION
# =============================================================================

echo "🔑 PREMINE VALIDATION"
echo "--------------------"

if [[ -f "/Volumes/HLC_Drive/dinero_secure/keys/public_info.txt" ]]; then
    COMPRESSED_PUBKEY=$(grep "Compressed Public Key:" "/Volumes/HLC_Drive/dinero_secure/keys/public_info.txt" | cut -d: -f2 | tr -d ' ')
    SCRIPT_PUBKEY_HEX=$(grep "P2WPKH scriptPubKey:" "/Volumes/HLC_Drive/dinero_secure/keys/public_info.txt" | cut -d: -f2 | tr -d ' ')
    BECH32_ADDRESS=$(grep "Bech32 Address:" "/Volumes/HLC_Drive/dinero_secure/keys/public_info.txt" | cut -d: -f2 | tr -d ' ')
    
    echo "📋 Premine Parameters:"
    echo "  Compressed pubkey: $COMPRESSED_PUBKEY"
    echo "  scriptPubKey: $SCRIPT_PUBKEY_HEX"
    echo "  Address: $BECH32_ADDRESS"
    
    # Validate format
    check "Pubkey format valid" "[[ '$COMPRESSED_PUBKEY' =~ ^0[23][0-9a-f]{64}$ ]]"
    check "Script format valid" "[[ '$SCRIPT_PUBKEY_HEX' =~ ^0014[0-9a-f]{40}$ ]]"
    check "Address format valid" "[[ '$BECH32_ADDRESS' =~ ^din1 ]]"
else
    echo "❌ Premine public info not found"
    READY=false
fi

echo ""

# =============================================================================
# NETWORK READINESS
# =============================================================================

echo "🌐 NETWORK READINESS"
echo "-------------------"

# Check ports are available
check "P2P port 40999 available" "! nc -z 127.0.0.1 40999"
check "RPC port 20999 available" "! nc -z 127.0.0.1 20999"
check "Health port 22001 available" "! nc -z 127.0.0.1 22001"

# Check external connectivity
check "External connectivity" "curl -s --max-time 5 https://httpbin.org/ip"

echo ""

# =============================================================================
# FINAL SUMMARY
# =============================================================================

echo "🎯 LAUNCH READINESS SUMMARY"
echo "==========================="
echo ""
echo "Checks completed: $CHECKS_PASSED/$CHECKS_TOTAL"

if [[ "$READY" == true ]]; then
    echo ""
    echo "🎉 ALL CHECKS PASSED!"
    echo ""
    echo "✅ DINERO IS READY FOR MAINNET LAUNCH!"
    echo ""
    echo "🚀 Ready to execute:"
    echo "   1. Manually integrate P2WPKH premine into chainparams"
    echo "   2. Build and test integration"
    echo "   3. Run genesis ceremony"
    echo "   4. Execute GO_LIVE.sh"
    echo ""
    echo "🔑 Your P2WPKH premine:"
    echo "   scriptPubKey: $SCRIPT_PUBKEY_HEX"
    echo "   Address: $BECH32_ADDRESS"
    echo ""
    echo "🎯 LAUNCH AUTHORIZATION: GO!"
    
    exit 0
else
    echo ""
    echo "❌ LAUNCH READINESS FAILED!"
    echo ""
    echo "🛑 Fix the failing checks before proceeding"
    echo "🔍 Review the failed items above"
    echo ""
    echo "⚠️  LAUNCH NOT AUTHORIZED"
    
    exit 1
fi
