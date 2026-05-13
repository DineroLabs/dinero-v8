#!/bin/bash
# ==============================================================================
# DINERO FIRST HOUR POST-LAUNCH RUNBOOK
# ==============================================================================
#
# Critical actions and monitoring for the first hour after mainnet launch
# Run this immediately after GO_LIVE completes successfully
#
# ==============================================================================

set -Eeuo pipefail
trap 'code=$?; echo "[FATAL] $0 failed at line $LINENO (exit $code)"; exit $code' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Setup logging
mkdir -p "$PROJECT_ROOT/logs"
exec > >(tee -a "$PROJECT_ROOT/logs/${0##*/}.log") 2>&1

echo "⏰ DINERO FIRST HOUR POST-LAUNCH RUNBOOK"
echo "========================================"
echo "Start time: $(date)"
echo ""

# Configuration
RPC_URL="http://127.0.0.1:20999"
HEALTH_URL="http://127.0.0.1:22001/healthz"
FIRST_HOUR_CHECKS=12  # Check every 5 minutes for 1 hour

# RPC helper
rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    curl -s -H 'content-type: application/json' \
         -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}" \
         "$RPC_URL" 2>/dev/null || echo '{"error": "RPC failed"}'
}

# =============================================================================
# IMMEDIATE POST-LAUNCH VALIDATION
# =============================================================================

echo "🚀 IMMEDIATE POST-LAUNCH VALIDATION"
echo "==================================="
echo ""

# 1. Health endpoint check
echo "📡 1. Health Endpoint Check"
if curl -s -f "$HEALTH_URL" >/dev/null 2>&1; then
    HEALTH_RESPONSE=$(curl -s "$HEALTH_URL")
    echo "✅ Health endpoint: $HEALTH_RESPONSE"
else
    echo "❌ Health endpoint: FAILED"
    exit 1
fi

# 2. Chain tips validation
echo ""
echo "🔗 2. Chain Tips Validation"
CHAIN_TIPS=$(rpc_call "getchaintips")
if echo "$CHAIN_TIPS" | jq -e '.result' >/dev/null 2>&1; then
    ACTIVE_TIPS=$(echo "$CHAIN_TIPS" | jq '[.result[] | select(.status == "active")] | length')
    if [[ "$ACTIVE_TIPS" == "1" ]]; then
        echo "✅ Single active tip confirmed"
        ACTIVE_TIP=$(echo "$CHAIN_TIPS" | jq -r '.result[] | select(.status == "active") | .hash')
        echo "   Active tip: ${ACTIVE_TIP:0:16}..."
    else
        echo "❌ Expected 1 active tip, found $ACTIVE_TIPS"
        exit 1
    fi
else
    echo "❌ Chain tips RPC failed"
    exit 1
fi

# 3. Chainwork progression
echo ""
echo "⚡ 3. Chainwork Progression"
INITIAL_CHAINWORK=$(rpc_call "getchainwork" | jq -r '.result // "0"')
echo "✅ Initial chainwork: ...${INITIAL_CHAINWORK: -16}"

# 4. Reorg status clean
echo ""
echo "🔄 4. Reorg Status Clean"
REORG_STATUS=$(rpc_call "getreorgstatus")
if echo "$REORG_STATUS" | jq -e '.result' >/dev/null 2>&1; then
    SAFE_MODE=$(echo "$REORG_STATUS" | jq -r '.result.safe_mode.active // false')
    if [[ "$SAFE_MODE" == "false" ]]; then
        echo "✅ Safe-mode: INACTIVE (good)"
    else
        echo "❌ Safe-mode: ACTIVE (investigate immediately)"
        exit 1
    fi
else
    echo "⚠️  Reorg status RPC not available"
fi

echo ""
echo "✅ IMMEDIATE VALIDATION COMPLETE"
echo ""

# =============================================================================
# MINING FAIRNESS - FIRST BLOCKS
# =============================================================================

echo "⛏️  MINING FAIRNESS - FIRST BLOCKS"
echo "================================="
echo ""

echo "🎯 Mining 1-2 blocks after public announcement for fairness..."
echo "   This ensures the network is functional before public mining begins"
echo ""

# Check if mining is active
if pgrep -f "miningaddress=" >/dev/null; then
    echo "✅ Mining process: ACTIVE"
    
    # Wait for first block after genesis
    echo "⏰ Waiting for first mined block..."
    INITIAL_HEIGHT=$(rpc_call "getblockcount" | jq -r '.result // 0')
    echo "   Starting height: $INITIAL_HEIGHT"
    
    # Wait up to 10 minutes for first block
    for i in {1..120}; do
        sleep 5
        CURRENT_HEIGHT=$(rpc_call "getblockcount" | jq -r '.result // 0')
        
        if [[ "$CURRENT_HEIGHT" -gt "$INITIAL_HEIGHT" ]]; then
            echo "✅ First block mined! Height: $CURRENT_HEIGHT"
            break
        fi
        
        if [[ $((i % 12)) -eq 0 ]]; then
            echo "   Still waiting... (${i}0 seconds elapsed)"
        fi
    done
    
    # Check if we got a block
    FINAL_HEIGHT=$(rpc_call "getblockcount" | jq -r '.result // 0')
    if [[ "$FINAL_HEIGHT" -gt "$INITIAL_HEIGHT" ]]; then
        echo "🎉 Mining is functional - first blocks produced!"
    else
        echo "⚠️  No blocks mined in 10 minutes - may be normal for high difficulty"
    fi
else
    echo "⚠️  Mining process not active - manual mining may be needed"
fi

echo ""

# =============================================================================
# LAUNCH BULLETIN PUBLICATION
# =============================================================================

echo "📰 LAUNCH BULLETIN PUBLICATION"
echo "=============================="
echo ""

# Check if launch bulletin exists
BULLETIN_FILE="$PROJECT_ROOT/DINERO_LAUNCH_BULLETIN.md"
if [[ -f "$BULLETIN_FILE" ]]; then
    echo "✅ Launch bulletin exists: $BULLETIN_FILE"
    
    # Extract key parameters for announcement
    if command -v grep >/dev/null 2>&1; then
        GENESIS_HASH=$(grep "Hash:" "$BULLETIN_FILE" | head -1 | cut -d'`' -f2 || echo "NOT_FOUND")
        CIC_HASH=$(grep "CIC:" "$BULLETIN_FILE" | head -1 | cut -d'`' -f2 || echo "NOT_FOUND")
        
        echo ""
        echo "📋 KEY PARAMETERS FOR ANNOUNCEMENT:"
        echo "  Genesis Hash: $GENESIS_HASH"
        echo "  CIC: $CIC_HASH"
        echo "  Network Magic: 0xD14E5201"
        echo "  HRP: din"
        echo "  P2P Port: 40999"
        echo ""
    fi
    
    echo "📤 PUBLICATION CHECKLIST:"
    echo "  [ ] Update website with launch bulletin"
    echo "  [ ] Create GitHub release with binaries"
    echo "  [ ] Post on social media (Twitter, Discord)"
    echo "  [ ] Notify community channels"
    echo "  [ ] Update documentation"
    echo "  [ ] Inform exchanges and partners"
    
else
    echo "⚠️  Launch bulletin not found - create with:"
    echo "   ./scripts/deploy/create_launch_bulletin.sh"
fi

echo ""

# =============================================================================
# FIRST HOUR MONITORING LOOP
# =============================================================================

echo "📊 FIRST HOUR MONITORING LOOP"
echo "============================="
echo ""

echo "🔄 Starting continuous monitoring for first hour..."
echo "   Checking every 5 minutes for network health"
echo "   Press Ctrl+C to stop monitoring"
echo ""

START_TIME=$(date +%s)
CHECK_COUNT=0

while [[ $CHECK_COUNT -lt $FIRST_HOUR_CHECKS ]]; do
    CHECK_COUNT=$((CHECK_COUNT + 1))
    CURRENT_TIME=$(date +%s)
    ELAPSED_MINUTES=$(( (CURRENT_TIME - START_TIME) / 60 ))
    
    echo "🕐 Check $CHECK_COUNT/$FIRST_HOUR_CHECKS (${ELAPSED_MINUTES} minutes elapsed)"
    echo "$(date): Network status check..."
    
    # Quick health checks
    HEALTH_OK=true
    
    # 1. Health endpoint
    if ! curl -s -f "$HEALTH_URL" >/dev/null 2>&1; then
        echo "❌ Health endpoint failed"
        HEALTH_OK=false
    fi
    
    # 2. Block height progression
    CURRENT_HEIGHT=$(rpc_call "getblockcount" | jq -r '.result // 0')
    CURRENT_CHAINWORK=$(rpc_call "getchainwork" | jq -r '.result // "0"')
    
    echo "   Height: $CURRENT_HEIGHT"
    echo "   Chainwork: ...${CURRENT_CHAINWORK: -16}"
    
    # 3. Chainwork should increase over time
    if [[ "$CURRENT_CHAINWORK" > "$INITIAL_CHAINWORK" ]]; then
        echo "   ✅ Chainwork increasing (network active)"
    elif [[ "$CURRENT_CHAINWORK" == "$INITIAL_CHAINWORK" ]]; then
        echo "   ⚠️  Chainwork unchanged (may be normal)"
    else
        echo "   ❌ Chainwork decreased (investigate immediately!)"
        HEALTH_OK=false
    fi
    
    # 4. Peer count
    PEER_COUNT=$(rpc_call "getconnectioncount" | jq -r '.result // 0')
    echo "   Peers: $PEER_COUNT"
    
    if [[ "$PEER_COUNT" -ge 1 ]]; then
        echo "   ✅ Network connectivity good"
    else
        echo "   ⚠️  No peers (isolated node)"
    fi
    
    # 5. Safe-mode check
    REORG_STATUS=$(rpc_call "getreorgstatus")
    if echo "$REORG_STATUS" | jq -e '.result.safe_mode.active' >/dev/null 2>&1; then
        SAFE_MODE=$(echo "$REORG_STATUS" | jq -r '.result.safe_mode.active')
        if [[ "$SAFE_MODE" == "true" ]]; then
            echo "   🚨 SAFE-MODE ACTIVE - INVESTIGATE IMMEDIATELY!"
            HEALTH_OK=false
        fi
    fi
    
    # 6. Reorg depth check
    if echo "$REORG_STATUS" | jq -e '.result.last_reorg.disconnect_depth' >/dev/null 2>&1; then
        REORG_DEPTH=$(echo "$REORG_STATUS" | jq -r '.result.last_reorg.disconnect_depth // 0')
        if [[ "$REORG_DEPTH" -gt 2 ]]; then
            echo "   ⚠️  Recent reorg depth: $REORG_DEPTH blocks"
        fi
    fi
    
    # Overall status
    if [[ "$HEALTH_OK" == true ]]; then
        echo "   🎯 Status: HEALTHY"
    else
        echo "   ⚠️  Status: ISSUES DETECTED"
    fi
    
    echo "   ────────────────────────────────────"
    
    # Wait 5 minutes before next check (unless last check)
    if [[ $CHECK_COUNT -lt $FIRST_HOUR_CHECKS ]]; then
        sleep 300  # 5 minutes
    fi
done

# =============================================================================
# FIRST HOUR SUMMARY
# =============================================================================

echo ""
echo "🎉 FIRST HOUR MONITORING COMPLETE"
echo "================================="
echo ""

FINAL_TIME=$(date +%s)
TOTAL_MINUTES=$(( (FINAL_TIME - START_TIME) / 60 ))

echo "📊 FIRST HOUR SUMMARY:"
echo "  Duration: $TOTAL_MINUTES minutes"
echo "  Checks performed: $CHECK_COUNT"
echo "  Final height: $(rpc_call "getblockcount" | jq -r '.result // 0')"
echo "  Final chainwork: ...$(rpc_call "getchainwork" | jq -r '.result // "0"' | tail -c 17)"
echo "  Final peers: $(rpc_call "getconnectioncount" | jq -r '.result // 0')"
echo ""

# Check if network is stable
FINAL_HEIGHT=$(rpc_call "getblockcount" | jq -r '.result // 0')
FINAL_CHAINWORK=$(rpc_call "getchainwork" | jq -r '.result // "0"')

if [[ "$FINAL_CHAINWORK" > "$INITIAL_CHAINWORK" ]] && [[ "$FINAL_HEIGHT" -ge 1 ]]; then
    echo "🎯 NETWORK STATUS: STABLE AND ACTIVE"
    echo ""
    echo "✅ Dinero mainnet has successfully completed its first hour!"
    echo "✅ Network is producing blocks and accumulating work"
    echo "✅ Ready for public announcement and community mining"
    echo ""
    echo "🚀 MAINNET LAUNCH: SUCCESS!"
else
    echo "⚠️  NETWORK STATUS: NEEDS ATTENTION"
    echo ""
    echo "🔍 Network may need investigation:"
    echo "  - Check mining configuration"
    echo "  - Verify peer connectivity"
    echo "  - Review daemon logs"
    echo ""
    echo "📞 Consider alerting operations team"
fi

echo ""
echo "📈 NEXT STEPS:"
echo "  1. Continue monitoring with: ./post_launch_checks.sh --continuous"
echo "  2. Publish launch announcement"
echo "  3. Monitor community feedback"
echo "  4. Set up automated alerts"
echo "  5. Plan Day-2 improvements"
echo ""

echo "🎉 Welcome to the Dinero network! The future of CPU-friendly mining! ⛏️💎"
