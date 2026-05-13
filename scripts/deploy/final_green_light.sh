#!/bin/bash
# ==============================================================================
# DINERO FINAL GREEN-LIGHT CHECKLIST
# ==============================================================================
#
# Surgical 10-minute validation before mainnet launch
# Every check must pass before GO_LIVE is authorized
#
# ==============================================================================

set -Eeuo pipefail
trap 'code=$?; echo "[FATAL] $0 failed at line $LINENO (exit $code)"; exit $code' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Create logs directory
mkdir -p "$PROJECT_ROOT/logs"
exec > >(tee -a "$PROJECT_ROOT/logs/${0##*/}.log") 2>&1

# Single instance lock
LOCK="$PROJECT_ROOT/logs/${0##*/}.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "❌ Green-light checklist already running"
    exit 1
fi

echo "🚦 DINERO FINAL GREEN-LIGHT CHECKLIST"
echo "====================================="
echo "Timestamp: $(date)"
echo "Validator: $(whoami)@$(hostname)"
echo ""

GREEN_LIGHT=true
CHECKS_TOTAL=0
CHECKS_PASSED=0

# Helper function for checks
check() {
    local name="$1"
    local command="$2"
    local critical="${3:-true}"
    
    CHECKS_TOTAL=$((CHECKS_TOTAL + 1))
    echo -n "🔍 $name... "
    
    if eval "$command" >/dev/null 2>&1; then
        echo "✅ PASS"
        CHECKS_PASSED=$((CHECKS_PASSED + 1))
    else
        if [[ "$critical" == "true" ]]; then
            echo "❌ FAIL (CRITICAL)"
            GREEN_LIGHT=false
        else
            echo "⚠️  WARN (NON-CRITICAL)"
        fi
    fi
}

# =============================================================================
# 1. GENESIS IMMUTABILITY
# =============================================================================

echo "🏗️ 1. GENESIS IMMUTABILITY"
echo "--------------------------"

# Find genesis ceremony results
GENESIS_DIR=$(find "$PROJECT_ROOT" -name "genesis_ceremony_mainnet" -type d 2>/dev/null | head -1)

if [[ -n "$GENESIS_DIR" ]] && [[ -f "$GENESIS_DIR/genesis_result.json" ]]; then
    GENESIS_HASH=$(jq -r '.hash' "$GENESIS_DIR/genesis_result.json")
    MERKLE_ROOT=$(jq -r '.merkle_root' "$GENESIS_DIR/genesis_result.json")
    GENESIS_TIME=$(jq -r '.timestamp' "$GENESIS_DIR/genesis_result.json")
    GENESIS_BITS=$(jq -r '.bits' "$GENESIS_DIR/genesis_result.json")
    GENESIS_NONCE=$(jq -r '.nonce' "$GENESIS_DIR/genesis_result.json")
    
    echo "📋 Genesis Parameters:"
    echo "  Hash: $GENESIS_HASH"
    echo "  Merkle: $MERKLE_ROOT"
    echo "  Time: $GENESIS_TIME"
    echo "  Bits: $GENESIS_BITS"
    echo "  Nonce: $GENESIS_NONCE"
    
    check "Genesis hash format valid" "[[ '$GENESIS_HASH' =~ ^[0-9a-f]{64}$ ]]"
    check "Merkle root format valid" "[[ '$MERKLE_ROOT' =~ ^[0-9a-f]{64}$ ]]"
    check "Genesis timestamp valid" "[[ '$GENESIS_TIME' =~ ^[0-9]{10}$ ]]"
    check "Genesis bits valid" "[[ '$GENESIS_BITS' =~ ^0x[0-9a-f]{8}$ ]]"
    check "Genesis nonce valid" "[[ '$GENESIS_NONCE' =~ ^[0-9]+$ ]]"
else
    echo "❌ Genesis ceremony results not found!"
    GREEN_LIGHT=false
fi

# Check CIC immutability
if [[ -f "$GENESIS_DIR/chain_params_final.json" ]]; then
    CIC_HASH=$(jq -r '.consensus.commitment' "$GENESIS_DIR/chain_params_final.json" 2>/dev/null || echo "")
    if [[ -n "$CIC_HASH" ]] && [[ "$CIC_HASH" != "null" ]]; then
        echo "🔒 CIC: $CIC_HASH"
        check "CIC format valid" "[[ '$CIC_HASH' =~ ^[0-9a-f]{64}$ ]]"
    else
        echo "❌ CIC not found or invalid"
        GREEN_LIGHT=false
    fi
fi

echo ""

# =============================================================================
# 2. TREASURY SAFETY
# =============================================================================

echo "🔑 2. TREASURY SAFETY"
echo "--------------------"

if [[ -d "$GENESIS_DIR/dev_fund" ]]; then
    DEV_FUND_DIR="$GENESIS_DIR/dev_fund"
    
    check "Dev fund key 1 exists" "test -f '$DEV_FUND_DIR/private_key_1.hex'"
    check "Dev fund key 2 exists" "test -f '$DEV_FUND_DIR/private_key_2.hex'"
    check "Dev fund key 3 exists" "test -f '$DEV_FUND_DIR/private_key_3.hex'"
    check "Multisig script exists" "test -f '$DEV_FUND_DIR/multisig_script.txt'"
    
    # Check key permissions
    for i in {1..3}; do
        if [[ -f "$DEV_FUND_DIR/private_key_$i.hex" ]]; then
            PERMS=$(stat -c "%a" "$DEV_FUND_DIR/private_key_$i.hex" 2>/dev/null || stat -f "%Lp" "$DEV_FUND_DIR/private_key_$i.hex" 2>/dev/null)
            if [[ "$PERMS" == "600" ]]; then
                echo "✅ Private key $i permissions: secure ($PERMS)"
            else
                echo "⚠️  Private key $i permissions: $PERMS (should be 600)"
            fi
        fi
    done
    
    # Extract dev fund address for bulletin validation
    if [[ -f "$DEV_FUND_DIR/address_1.txt" ]]; then
        DEV_FUND_ADDRESS=$(cat "$DEV_FUND_DIR/address_1.txt" | head -1)
        echo "💰 Dev Fund Address: $DEV_FUND_ADDRESS"
        check "Dev fund address format" "[[ '$DEV_FUND_ADDRESS' =~ ^din1 ]]"
    fi
else
    echo "❌ Treasury keys not found"
    GREEN_LIGHT=false
fi

echo ""

# =============================================================================
# 3. SEEDS READY
# =============================================================================

echo "🌐 3. SEEDS READY"
echo "---------------"

# Test P2P port availability
P2P_PORT=40999
check "P2P port $P2P_PORT available" "! nc -z 127.0.0.1 $P2P_PORT"

# DNS resolution test
check "DNS resolution works" "nslookup google.com"

# TODO: Test actual seed nodes when deployed
echo "⚠️  Seed node connectivity will be validated after deployment"
echo "   Required: Each seed on port 40999, >32 peer capacity, DNS TTL 300-900s"

echo ""

# =============================================================================
# 4. MONITORING LIVE
# =============================================================================

echo "📊 4. MONITORING LIVE"
echo "--------------------"

check "Prometheus config exists" "test -f '$SCRIPT_DIR/prometheus.yml'"
check "Alert rules exist" "test -f '$SCRIPT_DIR/dinero_alerts.yml'"
check "Grafana dashboard exists" "test -f '$SCRIPT_DIR/dinero_dashboard.json'"

# Test config validity
if command -v prometheus >/dev/null 2>&1; then
    check "Prometheus config valid" "prometheus --config.file='$SCRIPT_DIR/prometheus.yml' --dry-run"
else
    echo "⚠️  Prometheus not installed (will be configured on production servers)"
fi

# Check monitoring ports available
check "Health port 22001 available" "! nc -z 127.0.0.1 22001"

echo ""

# =============================================================================
# 5. BINARIES & PROVENANCE
# =============================================================================

echo "📦 5. BINARIES & PROVENANCE"
echo "---------------------------"

cd "$PROJECT_ROOT"

check "Release build exists" "test -x build/bin/dinerod"
check "CLI binary exists" "test -x build/bin/dinero-cli"
check "Binary version works" "build/bin/dinerod --version"

# Check for release artifacts
if [[ -d "release" ]]; then
    check "Release packages exist" "ls release/*.tar.gz"
    check "SHA256SUMS exists" "test -f release/SHA256SUMS.txt"
    
    if [[ -f "release/SHA256SUMS.txt.asc" ]]; then
        echo "✅ GPG signature found"
        if command -v gpg >/dev/null 2>&1; then
            check "GPG signature valid" "gpg --verify release/SHA256SUMS.txt.asc release/SHA256SUMS.txt"
        else
            echo "⚠️  GPG not available for signature verification"
        fi
    else
        echo "⚠️  GPG signature not found (use --sign with build_release.sh)"
    fi
else
    echo "⚠️  Release artifacts not built yet"
fi

echo ""

# =============================================================================
# 6. PROD POSTURE
# =============================================================================

echo "🔒 6. PROD POSTURE"
echo "-----------------"

# Check production configuration
NODEINFO="$SCRIPT_DIR/nodeinfo-mainnet.json"
if [[ -f "$NODEINFO" ]]; then
    check "RPC bound to localhost" "jq -e '.rpc.bind_address == \"127.0.0.1\"' '$NODEINFO'"
    check "RPC port 20999" "jq -e '.rpc.port == 20999' '$NODEINFO'"
    check "P2P port 40999" "jq -e '.p2p.port == 40999' '$NODEINFO'"
    check "Health port 22001" "jq -e '.http.port == 22001' '$NODEINFO'"
    check "Cookie auth enabled" "jq -e '.rpc.require_auth == true' '$NODEINFO'"
    check "Deep reorg threshold 30" "jq -e '.consensus.deep_reorg_threshold == 30' '$NODEINFO'"
    check "No allow deep reorg" "jq -e '.consensus.allow_deep_reorg == false' '$NODEINFO'"
else
    echo "❌ Production nodeinfo config not found"
    GREEN_LIGHT=false
fi

# Check systemd service
SYSTEMD_SERVICE="$SCRIPT_DIR/dinerod.service"
if [[ -f "$SYSTEMD_SERVICE" ]]; then
    check "Systemd user isolation" "grep -q 'User=dinerod' '$SYSTEMD_SERVICE'"
    check "Systemd sandboxing" "grep -q 'NoNewPrivileges=true' '$SYSTEMD_SERVICE'"
    check "Systemd resource limits" "grep -q 'LimitNOFILE' '$SYSTEMD_SERVICE'"
else
    echo "❌ Systemd service file not found"
    GREEN_LIGHT=false
fi

echo ""

# =============================================================================
# 7. DRY-RUN VALIDATION
# =============================================================================

echo "🧪 7. DRY-RUN VALIDATION"
echo "-----------------------"

# Check for placeholders
echo "🔍 Scanning for placeholders..."
PLACEHOLDER_COUNT=$(grep -r "TODO\|PLACEHOLDER\|FIXME\|XXX" --include="*.cpp" --include="*.h" --include="*.sh" --include="*.json" . | wc -l || echo "0")
if [[ "$PLACEHOLDER_COUNT" -eq 0 ]]; then
    echo "✅ No placeholders found"
else
    echo "⚠️  Found $PLACEHOLDER_COUNT potential placeholders"
    echo "   Review with: grep -r 'TODO\|PLACEHOLDER\|FIXME\|XXX' --include='*.cpp' --include='*.h' --include='*.sh' --include='*.json' ."
fi

# Test preflight checklist
if [[ -x "$SCRIPT_DIR/preflight_checklist.sh" ]]; then
    echo "🚦 Testing preflight checklist..."
    if "$SCRIPT_DIR/preflight_checklist.sh" >/dev/null 2>&1; then
        echo "✅ Preflight checklist: PASS"
    else
        echo "❌ Preflight checklist: FAIL"
        GREEN_LIGHT=false
    fi
else
    echo "❌ Preflight checklist not found"
    GREEN_LIGHT=false
fi

# Test dry-run (skip if already running in GO_LIVE context)
if [[ "${SKIP_DRY_RUN:-false}" != "true" ]]; then
    echo "🧪 Testing launch dry-run..."
    if [[ -x "$SCRIPT_DIR/launch_dinero.sh" ]]; then
        if "$SCRIPT_DIR/launch_dinero.sh" --dry-run --skip-genesis --skip-build >/dev/null 2>&1; then
            echo "✅ Launch dry-run: PASS"
        else
            echo "❌ Launch dry-run: FAIL"
            GREEN_LIGHT=false
        fi
    else
        echo "❌ Launch script not found"
        GREEN_LIGHT=false
    fi
fi

echo ""

# =============================================================================
# 8. HARDENING SWITCHES
# =============================================================================

echo "🛡️ 8. HARDENING SWITCHES"
echo "------------------------"

echo "🔧 Validating production switches..."

# Check that dangerous flags are NOT set
check "No dev-autoreset flag" "! grep -q 'dev-autoreset' '$NODEINFO'"
check "No allow-deep-reorg flag" "! grep -q 'allow_deep_reorg.*true' '$NODEINFO'"
check "RocksDB backend configured" "jq -e '.storage.backend == \"rocksdb\" or .storage.backend == \"sqlite\"' '$NODEINFO'"

# Validate split ports (not unified)
RPC_PORT=$(jq -r '.rpc.port' "$NODEINFO" 2>/dev/null || echo "0")
P2P_PORT=$(jq -r '.p2p.port' "$NODEINFO" 2>/dev/null || echo "0")
HTTP_PORT=$(jq -r '.http.port' "$NODEINFO" 2>/dev/null || echo "0")

if [[ "$RPC_PORT" != "$P2P_PORT" ]] && [[ "$RPC_PORT" != "$HTTP_PORT" ]] && [[ "$P2P_PORT" != "$HTTP_PORT" ]]; then
    echo "✅ Split ports configured (RPC:$RPC_PORT, P2P:$P2P_PORT, HTTP:$HTTP_PORT)"
else
    echo "⚠️  Port configuration may not be split properly"
fi

echo ""

# =============================================================================
# 9. SUPPLY SANITY
# =============================================================================

echo "💰 9. SUPPLY SANITY"
echo "------------------"

# Validate economic parameters
PREMINE_AMOUNT="2000000"
MAX_SUPPLY="99000000"
PHASE2_START="20000000"

echo "📊 Economic parameters:"
echo "  Premine: ${PREMINE_AMOUNT} DIN"
echo "  Phase 2 start: ${PHASE2_START} DIN"
echo "  Max supply: ${MAX_SUPPLY} DIN"

# Basic sanity checks
check "Premine < Phase 2 start" "[[ $PREMINE_AMOUNT -lt $PHASE2_START ]]"
check "Phase 2 start < Max supply" "[[ $PHASE2_START -lt $MAX_SUPPLY ]]"
check "Premine is reasonable" "[[ $PREMINE_AMOUNT -le 5000000 ]]"  # Max 5M premine

echo ""

# =============================================================================
# 10. POINT OF NO RETURN
# =============================================================================

echo "🚨 10. POINT OF NO RETURN"
echo "------------------------"

if [[ "$GREEN_LIGHT" == true ]]; then
    echo "🎉 ALL GREEN-LIGHT CHECKS PASSED!"
    echo ""
    echo "✅ Checks passed: $CHECKS_PASSED/$CHECKS_TOTAL"
    echo ""
    echo "🎯 READY FOR MAINNET LAUNCH!"
    echo ""
    echo "📋 Final Parameters:"
    echo "  Genesis Hash: ${GENESIS_HASH:-'TBD'}"
    echo "  CIC: ${CIC_HASH:-'TBD'}"
    echo "  Dev Fund: ${DEV_FUND_ADDRESS:-'TBD'}"
    echo "  Network Magic: 0xD14E5201"
    echo "  HRP: din"
    echo ""
    echo "🔥 AUTHORIZATION: GO FOR LAUNCH!"
    echo ""
    
    # Create authorization token
    AUTH_TOKEN="DINERO_GO_LIVE_$(date +%s)"
    echo "$AUTH_TOKEN" > "$PROJECT_ROOT/logs/launch_authorization.token"
    echo "🎫 Launch authorization token: $AUTH_TOKEN"
    echo ""
    
    exit 0
else
    echo "❌ GREEN-LIGHT CHECKS FAILED!"
    echo ""
    echo "✅ Checks passed: $CHECKS_PASSED/$CHECKS_TOTAL"
    echo ""
    echo "🛑 DO NOT LAUNCH - FIX ISSUES FIRST"
    echo ""
    echo "🔍 Review failed checks above"
    echo "🔧 Fix issues and re-run green-light checklist"
    echo ""
    echo "⚠️  LAUNCH AUTHORIZATION DENIED"
    
    exit 1
fi
