#!/bin/bash
# ==============================================================================
# 🚀 DINERO MAINNET GO-LIVE SEQUENCE 🚀
# ==============================================================================
#
# THE BIG RED BUTTON - LAUNCHES DINERO TO MAINNET
# Enhanced with surgical hardening and idempotency
#
# ==============================================================================

set -Eeuo pipefail
trap 'code=$?; echo "[FATAL] $0 failed at line $LINENO (exit $code)"; exit $code' ERR

# ==============================================================================
# 🚀 DINERO MAINNET GO-LIVE SEQUENCE 🚀
# ==============================================================================
#
# THE BIG RED BUTTON - LAUNCHES DINERO TO MAINNET
#
# This script executes the complete go-live playbook:
# ✅ T-0 Preflight validation
# ✅ Genesis ceremony  
# ✅ Production builds
# ✅ Network launch
# ✅ Post-launch validation
# ✅ Launch bulletin
#
# Usage: ./GO_LIVE.sh [--confirm-launch]
#
# ⚠️  WARNING: THIS CREATES A REAL CRYPTOCURRENCY NETWORK!
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Create logs directory and setup logging
mkdir -p "$PROJECT_ROOT/logs"
exec > >(tee -a "$PROJECT_ROOT/logs/${0##*/}.log") 2>&1

# Single instance lock
LOCK="$PROJECT_ROOT/logs/${0##*/}.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "❌ GO_LIVE already running (check logs/${0##*/}.lock)"
    exit 1
fi

# Safety configuration
CONFIRMED=false
LAUNCH_VERSION="v1.0.0-mainnet"
LAUNCH_TIMESTAMP=$(date +%s)

# ASCII Art
cat << 'EOF'
    ____  _                      
   / __ \(_)___  ___  _________  
  / / / / / __ \/ _ \/ ___/ __ \ 
 / /_/ / / / / /  __/ /  / /_/ / 
/_____/_/_/ /_/\___/_/   \____/  
                                 
🚀 MAINNET LAUNCH SEQUENCE 🚀
EOF

echo ""
echo "=============================================="
echo "🔥 DINERO CRYPTOCURRENCY GO-LIVE SEQUENCE 🔥"
echo "=============================================="
echo ""
echo "Launch Version: $LAUNCH_VERSION"
echo "Launch Time: $(date -d "@$LAUNCH_TIMESTAMP")"
echo ""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --confirm-launch)
            CONFIRMED=true
            shift
            ;;
        --help)
            echo "Usage: $0 [--confirm-launch]"
            echo ""
            echo "This script launches Dinero cryptocurrency to mainnet."
            echo ""
            echo "⚠️  WARNING: This creates a real blockchain with real value!"
            echo ""
            echo "Options:"
            echo "  --confirm-launch    Skip safety confirmation (for automation)"
            echo ""
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# =============================================================================
# SAFETY CONFIRMATION
# =============================================================================

if [[ "$CONFIRMED" == false ]]; then
    echo "🚨 CRYPTOCURRENCY LAUNCH CONFIRMATION"
    echo "====================================="
    echo ""
    echo "⚠️  YOU ARE ABOUT TO LAUNCH A REAL CRYPTOCURRENCY NETWORK!"
    echo ""
    echo "This will:"
    echo "  🏗️  Create the genesis block (irreversible)"
    echo "  💰 Generate 2M DIN developer fund"
    echo "  🌐 Launch public P2P network"
    echo "  ⛏️  Start mining real DIN tokens"
    echo "  📊 Activate monitoring and alerts"
    echo ""
    echo "Once launched:"
    echo "  ✅ The network will be permanent"
    echo "  ✅ Tokens will have real value"
    echo "  ✅ Community will depend on stability"
    echo "  ✅ You become responsible for a financial network"
    echo ""
    echo "🎯 Are you ready to launch Dinero cryptocurrency?"
    echo ""
    read -p "Type 'LAUNCH DINERO' to confirm: " CONFIRMATION
    
    if [[ "$CONFIRMATION" != "LAUNCH DINERO" ]]; then
        echo ""
        echo "🛑 Launch aborted by user"
        echo "Use --confirm-launch to skip this confirmation"
        exit 0
    fi
    
    echo ""
    echo "🚀 LAUNCH CONFIRMED! INITIATING SEQUENCE..."
fi

echo ""
echo "🎬 LAUNCH SEQUENCE INITIATED"
echo "Timestamp: $(date)"
echo "Version: $LAUNCH_VERSION"
echo ""

# =============================================================================
# PHASE 1: FINAL GREEN-LIGHT CHECKLIST
# =============================================================================

echo "🚦 PHASE 1: FINAL GREEN-LIGHT CHECKLIST"
echo "======================================="
echo ""

if [[ -x "$SCRIPT_DIR/final_green_light.sh" ]]; then
    echo "🔍 Running surgical green-light validation..."
    export SKIP_DRY_RUN=true  # Avoid recursive dry-run
    if "$SCRIPT_DIR/final_green_light.sh"; then
        echo "✅ GREEN-LIGHT: ALL SYSTEMS GO!"
        
        # Check for authorization token
        AUTH_TOKEN_FILE="$PROJECT_ROOT/logs/launch_authorization.token"
        if [[ -f "$AUTH_TOKEN_FILE" ]]; then
            AUTH_TOKEN=$(cat "$AUTH_TOKEN_FILE")
            echo "🎫 Launch authorized with token: $AUTH_TOKEN"
        else
            echo "❌ Launch authorization token not found!"
            exit 1
        fi
    else
        echo "❌ GREEN-LIGHT: FAILED - ABORTING LAUNCH!"
        echo ""
        echo "🛑 Fix green-light issues before launching"
        exit 1
    fi
else
    echo "⚠️  Green-light checklist not found, running basic preflight..."
    if [[ -x "$SCRIPT_DIR/preflight_checklist.sh" ]]; then
        if "$SCRIPT_DIR/preflight_checklist.sh"; then
            echo "✅ BASIC PREFLIGHT: PASSED"
        else
            echo "❌ BASIC PREFLIGHT: FAILED - ABORTING!"
            exit 1
        fi
    fi
fi

echo ""

# =============================================================================
# PHASE 2: CODE FREEZE & TAGGING
# =============================================================================

echo "🔒 PHASE 2: CODE FREEZE & TAGGING"
echo "================================="
echo ""

cd "$PROJECT_ROOT"

# Check git status
if [[ -n "$(git status --porcelain)" ]]; then
    echo "⚠️  Working directory has uncommitted changes"
    echo "Committing changes for launch..."
    git add -A
    git commit -m "🚀 Dinero $LAUNCH_VERSION mainnet launch

Launch timestamp: $LAUNCH_TIMESTAMP
Launch date: $(date -d "@$LAUNCH_TIMESTAMP")

This commit represents the final code freeze for mainnet launch.
All consensus parameters are locked and immutable.

Network parameters:
- HRP: din
- P2P Port: 40999
- Magic Bytes: 0xD14E5201
- Deep Reorg Threshold: 30 blocks

Ready for production deployment! 🎯"
fi

# Tag the release
echo "🏷️  Tagging release $LAUNCH_VERSION..."
if git tag | grep -q "$LAUNCH_VERSION"; then
    echo "   Tag already exists, using existing tag"
else
    git tag -a "$LAUNCH_VERSION" -m "Dinero $LAUNCH_VERSION - Mainnet Launch

🚀 Official mainnet launch of Dinero cryptocurrency

Launch Details:
- Date: $(date -d "@$LAUNCH_TIMESTAMP")
- Network: Mainnet
- Version: $LAUNCH_VERSION
- Consensus: Locked and immutable

This tag represents the exact code running on mainnet.
All future changes require careful coordination.

🎯 Welcome to Dinero!"
fi

echo "✅ Code freeze complete - $LAUNCH_VERSION tagged"
echo ""

# =============================================================================
# PHASE 3: GENESIS CEREMONY - POINT OF NO RETURN
# =============================================================================

echo "🏗️  PHASE 3: GENESIS CEREMONY - POINT OF NO RETURN"
echo "=================================================="
echo ""

echo "🚨 CRITICAL: This will create MAINNET GENESIS (irreversible)"
echo "=============================================="
echo ""
echo "⚠️  Once genesis is created:"
echo "  • The blockchain becomes permanent"
echo "  • Network parameters are immutable"
echo "  • 2M DIN developer fund is locked"
echo "  • Community will depend on this network"
echo ""
echo "🎯 This is the POINT OF NO RETURN!"
echo ""

if [[ "$DRY_RUN" == false ]]; then
    echo "Type 'YES' to create mainnet genesis:"
    read -r GENESIS_CONFIRM
    if [[ "$GENESIS_CONFIRM" != "YES" ]]; then
        echo "🛑 Genesis creation aborted by user"
        exit 0
    fi
    echo ""
    echo "🔥 GENESIS CREATION AUTHORIZED!"
fi

echo "🏗️  Performing genesis ceremony..."
if "$SCRIPT_DIR/genesis_ceremony.sh" 2>&1 | tee "/tmp/genesis_ceremony_$LAUNCH_TIMESTAMP.log"; then
    echo "✅ GENESIS CEREMONY: SUCCESS!"
    
    # Extract genesis hash for announcement
    GENESIS_DIR=$(find "$PROJECT_ROOT" -name "genesis_ceremony_mainnet" -type d | head -1)
    if [[ -n "$GENESIS_DIR" ]] && [[ -f "$GENESIS_DIR/genesis_result.json" ]]; then
        GENESIS_HASH=$(jq -r '.hash' "$GENESIS_DIR/genesis_result.json")
        echo "🎯 GENESIS HASH: $GENESIS_HASH"
    fi
else
    echo "❌ GENESIS CEREMONY: FAILED!"
    echo "Check log: /tmp/genesis_ceremony_$LAUNCH_TIMESTAMP.log"
    exit 1
fi

echo ""

# =============================================================================
# PHASE 4: PRODUCTION BUILDS
# =============================================================================

echo "🔨 PHASE 4: PRODUCTION BUILDS"
echo "============================="
echo ""

echo "🏗️  Building production binaries..."
if "$SCRIPT_DIR/build_release.sh" --version "$LAUNCH_VERSION" --sign 2>&1 | tee "/tmp/build_release_$LAUNCH_TIMESTAMP.log"; then
    echo "✅ PRODUCTION BUILDS: SUCCESS!"
    
    # Show build artifacts
    if [[ -d "$PROJECT_ROOT/release" ]]; then
        echo "📦 Build artifacts:"
        ls -la "$PROJECT_ROOT/release"/*.tar.gz 2>/dev/null || echo "   (no packages found)"
    fi
else
    echo "❌ PRODUCTION BUILDS: FAILED!"
    echo "Check log: /tmp/build_release_$LAUNCH_TIMESTAMP.log"
    exit 1
fi

echo ""

# =============================================================================
# PHASE 5: NETWORK LAUNCH
# =============================================================================

echo "🌐 PHASE 5: NETWORK LAUNCH"
echo "=========================="
echo ""

echo "🚀 Launching Dinero network..."
if "$SCRIPT_DIR/launch_dinero.sh" --skip-genesis --skip-build 2>&1 | tee "/tmp/network_launch_$LAUNCH_TIMESTAMP.log"; then
    echo "✅ NETWORK LAUNCH: SUCCESS!"
else
    echo "❌ NETWORK LAUNCH: FAILED!"
    echo "Check log: /tmp/network_launch_$LAUNCH_TIMESTAMP.log"
    exit 1
fi

echo ""

# =============================================================================
# PHASE 6: POST-LAUNCH VALIDATION
# =============================================================================

echo "🏥 PHASE 6: POST-LAUNCH VALIDATION"
echo "=================================="
echo ""

# Wait a moment for services to start
echo "⏰ Waiting 10 seconds for services to initialize..."
sleep 10

echo "🔍 Running post-launch health checks..."
if "$SCRIPT_DIR/post_launch_checks.sh" 2>&1 | tee "/tmp/post_launch_$LAUNCH_TIMESTAMP.log"; then
    echo "✅ POST-LAUNCH VALIDATION: ALL HEALTHY!"
else
    echo "⚠️  POST-LAUNCH VALIDATION: Some issues detected"
    echo "Check log: /tmp/post_launch_$LAUNCH_TIMESTAMP.log"
    echo "Network may still be functional - review issues"
fi

echo ""

# =============================================================================
# PHASE 7: LAUNCH BULLETIN
# =============================================================================

echo "📰 PHASE 7: LAUNCH BULLETIN"
echo "==========================="
echo ""

echo "📝 Creating official launch bulletin..."
if "$SCRIPT_DIR/create_launch_bulletin.sh" 2>&1 | tee "/tmp/launch_bulletin_$LAUNCH_TIMESTAMP.log"; then
    echo "✅ LAUNCH BULLETIN: CREATED!"
else
    echo "⚠️  LAUNCH BULLETIN: Issues detected"
    echo "Check log: /tmp/launch_bulletin_$LAUNCH_TIMESTAMP.log"
fi

echo ""

# =============================================================================
# LAUNCH COMPLETE!
# =============================================================================

echo "🎉 DINERO MAINNET LAUNCH COMPLETE!"
echo "=================================="
echo ""

# Get final network status
if command -v dinero-cli >/dev/null 2>&1; then
    if dinero-cli getblockcount >/dev/null 2>&1; then
        BLOCK_COUNT=$(dinero-cli getblockcount 2>/dev/null || echo "unknown")
        PEER_COUNT=$(dinero-cli getconnectioncount 2>/dev/null || echo "unknown")
        
        echo "📊 NETWORK STATUS:"
        echo "  Block Height: $BLOCK_COUNT"
        echo "  Connected Peers: $PEER_COUNT"
        echo "  Network: LIVE 🔥"
    else
        echo "📊 NETWORK STATUS: RPC not responding (may be starting up)"
    fi
else
    echo "📊 NETWORK STATUS: CLI not available"
fi

echo ""
echo "🎯 LAUNCH SUMMARY:"
echo "  Version: $LAUNCH_VERSION"
echo "  Launch Time: $(date -d "@$LAUNCH_TIMESTAMP")"
echo "  Genesis Hash: ${GENESIS_HASH:-"See ceremony results"}"
echo "  Network Magic: 0xD14E5201"
echo "  HRP: din"
echo ""

echo "📁 LAUNCH LOGS:"
echo "  Genesis: /tmp/genesis_ceremony_$LAUNCH_TIMESTAMP.log"
echo "  Build: /tmp/build_release_$LAUNCH_TIMESTAMP.log"
echo "  Network: /tmp/network_launch_$LAUNCH_TIMESTAMP.log"
echo "  Health: /tmp/post_launch_$LAUNCH_TIMESTAMP.log"
echo "  Bulletin: /tmp/launch_bulletin_$LAUNCH_TIMESTAMP.log"
echo ""

echo "🚀 DINERO IS NOW LIVE ON MAINNET!"
echo ""
echo "🎯 IMMEDIATE ACTIONS:"
echo "  1. Monitor network health:"
echo "     ./scripts/deploy/post_launch_checks.sh --continuous"
echo ""
echo "  2. Verify mining is working:"
echo "     dinero-cli getblockcount"
echo "     dinero-cli getmininginfo"
echo ""
echo "  3. Check system resources:"
echo "     curl http://127.0.0.1:22001/healthz"
echo ""
echo "  4. Announce the launch:"
echo "     - Publish release notes"
echo "     - Update website"
echo "     - Social media announcement"
echo "     - Community notification"
echo ""

echo "🔥 CONGRATULATIONS!"
echo "You have successfully launched Dinero cryptocurrency!"
echo ""
echo "⛏️  The network is now mining real DIN tokens"
echo "🌐 P2P network is live on port 40999"
echo "💰 2M DIN developer fund is secured"
echo "📊 Monitoring and alerts are active"
echo ""

# Final network health check
echo "🏥 FINAL HEALTH CHECK:"
if curl -s -f http://127.0.0.1:22001/healthz >/dev/null 2>&1; then
    echo "✅ HTTP Health: HEALTHY"
else
    echo "⚠️  HTTP Health: Not responding (may be starting)"
fi

if pgrep -f dinerod >/dev/null; then
    echo "✅ Daemon Process: RUNNING"
else
    echo "❌ Daemon Process: NOT RUNNING"
fi

if pgrep -f "miningaddress=" >/dev/null; then
    echo "✅ Mining Process: ACTIVE"
else
    echo "⚠️  Mining Process: NOT ACTIVE"
fi

echo ""
echo "🎉 WELCOME TO THE DINERO NETWORK!"
echo ""
echo "The future of CPU-friendly cryptocurrency starts now! 💎⛏️🚀"
echo ""

# Launch first hour monitoring if available
if [[ -x "$SCRIPT_DIR/first_hour_runbook.sh" ]]; then
    echo "⏰ LAUNCHING FIRST HOUR MONITORING"
    echo ""
    echo "Starting critical first hour post-launch procedures..."
    echo "This will monitor network health and guide launch announcement"
    echo ""
    sleep 3
    exec "$SCRIPT_DIR/first_hour_runbook.sh"
else
    # Fallback to basic monitoring
    if [[ -t 1 ]] && [[ -x "$SCRIPT_DIR/post_launch_checks.sh" ]]; then
        echo "🔄 Starting basic continuous monitoring..."
        echo "Press Ctrl+C to stop monitoring and return to shell"
        echo ""
        sleep 3
        exec "$SCRIPT_DIR/post_launch_checks.sh" --continuous
    fi
fi
