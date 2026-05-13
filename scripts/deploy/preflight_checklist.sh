#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO MAINNET PREFLIGHT CHECKLIST
# ==============================================================================
#
# T-0 Preflight validation before mainnet launch
# This script validates EVERYTHING before you press the big red button
#
# Usage: ./preflight_checklist.sh
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "🚨 DINERO MAINNET PREFLIGHT CHECKLIST"
echo "====================================="
echo "Timestamp: $(date)"
echo ""

PREFLIGHT_PASSED=true
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
        PREFLIGHT_PASSED=false
    fi
}

# =============================================================================
# 1. CODE FREEZE VALIDATION
# =============================================================================

echo "📋 1. CODE FREEZE VALIDATION"
echo "----------------------------"

# Check git status
check "Git working directory clean" "test -z \"\$(git status --porcelain)\""
check "Git tag v1.0.0-mainnet exists" "git tag | grep -q 'v1.0.0-mainnet'"
check "Current commit is tagged" "git describe --exact-match HEAD | grep -q 'v1.0.0-mainnet'"

# Check consensus lock
check "ChainParams CIC locked" "grep -q 'FROZEN.*consensus' src/consensus/chainparams_mainnet_final.cpp"
check "Network magic bytes defined" "grep -q '0xD1.*0x4E.*0x52.*0x01' src/consensus/chainparams_mainnet_final.cpp"

echo ""

# =============================================================================
# 2. BUILD VALIDATION
# =============================================================================

echo "🔨 2. BUILD VALIDATION"
echo "---------------------"

cd "$PROJECT_ROOT"

# Check build system
check "CMake available" "command -v cmake"
check "Build directory exists" "test -d build"
check "Release build successful" "test -x build/bin/dinerod"
check "CLI binary exists" "test -x build/bin/dinero-cli"

# Test binary execution
check "dinerod version works" "build/bin/dinerod --version"
check "dinero-cli help works" "build/bin/dinero-cli --help"

echo ""

# =============================================================================
# 3. WALLET RELEASE GATES
# =============================================================================

echo "👛 3. WALLET RELEASE GATES"
echo "--------------------------"

check "Wallet release gates pass" "./scripts/wallet_release_gates.sh build"

echo ""

# =============================================================================
# 4. GENESIS ARTIFACTS VALIDATION
# =============================================================================

echo "🏗️ 4. GENESIS ARTIFACTS VALIDATION"
echo "----------------------------------"

# Look for genesis ceremony results
GENESIS_DIR=$(find . -name "genesis_ceremony_mainnet" -type d 2>/dev/null | head -1)

if [[ -n "$GENESIS_DIR" ]]; then
    echo "📁 Genesis ceremony directory: $GENESIS_DIR"
    
    check "Genesis result JSON exists" "test -f \"$GENESIS_DIR/genesis_result.json\""
    check "Chain params final exists" "test -f \"$GENESIS_DIR/chain_params_final.json\""
    check "Developer fund keys exist" "test -d \"$GENESIS_DIR/dev_fund\""
    
    if [[ -f "$GENESIS_DIR/genesis_result.json" ]]; then
        GENESIS_HASH=$(jq -r '.hash' "$GENESIS_DIR/genesis_result.json" 2>/dev/null || echo "")
        MERKLE_ROOT=$(jq -r '.merkle_root' "$GENESIS_DIR/genesis_result.json" 2>/dev/null || echo "")
        GENESIS_TIME=$(jq -r '.timestamp' "$GENESIS_DIR/genesis_result.json" 2>/dev/null || echo "")
        GENESIS_BITS=$(jq -r '.bits' "$GENESIS_DIR/genesis_result.json" 2>/dev/null || echo "")
        GENESIS_NONCE=$(jq -r '.nonce' "$GENESIS_DIR/genesis_result.json" 2>/dev/null || echo "")
        
        echo "📋 Genesis Artifacts:"
        echo "  Hash: $GENESIS_HASH"
        echo "  Merkle Root: $MERKLE_ROOT"
        echo "  Timestamp: $GENESIS_TIME ($(date -d "@$GENESIS_TIME" 2>/dev/null || echo "invalid"))"
        echo "  Bits: $GENESIS_BITS"
        echo "  Nonce: $GENESIS_NONCE"
    fi
else
    echo "⚠️  No genesis ceremony results found"
    echo "Run: ./scripts/deploy/genesis_ceremony.sh"
    PREFLIGHT_PASSED=false
fi

echo ""

# =============================================================================
# 5. TREASURY KEYS VALIDATION
# =============================================================================

echo "🔑 5. TREASURY KEYS VALIDATION"
echo "------------------------------"

if [[ -n "$GENESIS_DIR" ]] && [[ -d "$GENESIS_DIR/dev_fund" ]]; then
    DEV_FUND_DIR="$GENESIS_DIR/dev_fund"
    
    check "Private key 1 exists" "test -f \"$DEV_FUND_DIR/private_key_1.hex\""
    check "Private key 2 exists" "test -f \"$DEV_FUND_DIR/private_key_2.hex\""
    check "Private key 3 exists" "test -f \"$DEV_FUND_DIR/private_key_3.hex\""
    check "Multisig script exists" "test -f \"$DEV_FUND_DIR/multisig_script.txt\""
    
    # Check key file permissions
    for i in {1..3}; do
        if [[ -f "$DEV_FUND_DIR/private_key_$i.hex" ]]; then
            PERMS=$(stat -c "%a" "$DEV_FUND_DIR/private_key_$i.hex" 2>/dev/null || stat -f "%Lp" "$DEV_FUND_DIR/private_key_$i.hex" 2>/dev/null || echo "unknown")
            if [[ "$PERMS" == "600" ]]; then
                echo "✅ Private key $i permissions: secure ($PERMS)"
            else
                echo "⚠️  Private key $i permissions: $PERMS (should be 600)"
            fi
        fi
    done
else
    echo "❌ Treasury keys not found"
    PREFLIGHT_PASSED=false
fi

echo ""

# =============================================================================
# 6. NETWORK CONNECTIVITY
# =============================================================================

echo "🌐 6. NETWORK CONNECTIVITY"
echo "-------------------------"

# Test DNS resolution
check "DNS resolution works" "nslookup google.com"
check "External connectivity" "curl -s --max-time 5 https://httpbin.org/ip"

# Test P2P port availability
P2P_PORT=40999
check "P2P port $P2P_PORT available locally" "! nc -z 127.0.0.1 $P2P_PORT"

# TODO: Test seed node connectivity when they're deployed
echo "⚠️  Seed node connectivity will be tested after deployment"

echo ""

# =============================================================================
# 7. MONITORING READINESS
# =============================================================================

echo "📊 7. MONITORING READINESS"
echo "-------------------------"

# Check monitoring configuration
check "Prometheus config exists" "test -f scripts/deploy/prometheus.yml"
check "Alert rules exist" "test -f scripts/deploy/dinero_alerts.yml"
check "Health endpoint port available" "! nc -z 127.0.0.1 22001"

# Check if Prometheus is available (optional)
if command -v prometheus >/dev/null 2>&1; then
    check "Prometheus binary available" "true"
    
    # Test config validation
    check "Prometheus config valid" "prometheus --config.file=scripts/deploy/prometheus.yml --dry-run"
else
    echo "⚠️  Prometheus not installed (optional for launch)"
fi

echo ""

# =============================================================================
# 8. DEPLOYMENT SCRIPTS VALIDATION
# =============================================================================

echo "🚀 8. DEPLOYMENT SCRIPTS VALIDATION"
echo "-----------------------------------"

# Check deployment scripts exist and are executable
DEPLOY_SCRIPTS=(
    "scripts/deploy/launch_dinero.sh"
    "scripts/deploy/deploy_mainnet.sh"
    "scripts/deploy/genesis_ceremony.sh"
    "scripts/deploy/build_release.sh"
    "scripts/deploy/launch_mining.sh"
)

for script in "${DEPLOY_SCRIPTS[@]}"; do
    check "$(basename "$script") executable" "test -x \"$script\""
done

# Test dry-run capability
check "Launch orchestrator dry-run works" "./scripts/deploy/launch_dinero.sh --dry-run --skip-genesis --skip-build"

echo ""

# =============================================================================
# 9. SECURITY VALIDATION
# =============================================================================

echo "🔒 9. SECURITY VALIDATION"
echo "-------------------------"

# Check configuration security
check "Node config exists" "test -f scripts/deploy/nodeinfo-mainnet.json"
check "RPC bound to localhost" "jq -e '.rpc.bind_address == \"127.0.0.1\"' scripts/deploy/nodeinfo-mainnet.json"
check "Cookie auth enabled" "jq -e '.rpc.require_auth == true' scripts/deploy/nodeinfo-mainnet.json"
check "Deep reorg protection enabled" "jq -e '.consensus.deep_reorg_threshold >= 30' scripts/deploy/nodeinfo-mainnet.json"

# Check systemd security
check "Systemd service hardened" "grep -q 'NoNewPrivileges=true' scripts/deploy/dinerod.service"
check "Systemd user isolation" "grep -q 'User=dinerod' scripts/deploy/dinerod.service"

echo ""

# =============================================================================
# PREFLIGHT SUMMARY
# =============================================================================

echo "🎯 PREFLIGHT SUMMARY"
echo "===================="
echo ""
echo "Checks completed: $CHECKS_PASSED/$CHECKS_TOTAL"

if [[ "$PREFLIGHT_PASSED" == true ]]; then
    echo "🎉 ALL PREFLIGHT CHECKS PASSED!"
    echo ""
    echo "✅ DINERO IS GO FOR MAINNET LAUNCH!"
    echo ""
    echo "🚀 Ready to execute:"
    echo "   ./scripts/deploy/launch_dinero.sh"
    echo ""
    echo "🎯 T-0 LAUNCH SEQUENCE AUTHORIZED!"
    
    exit 0
else
    echo "❌ PREFLIGHT FAILED - DO NOT LAUNCH!"
    echo ""
    echo "🛑 Fix the failing checks before proceeding"
    echo "🔍 Review the failed items above"
    echo ""
    echo "⚠️  LAUNCH ABORTED FOR SAFETY"
    
    exit 1
fi
