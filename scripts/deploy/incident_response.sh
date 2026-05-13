#!/bin/bash
# ==============================================================================
# DINERO INCIDENT RESPONSE & ROLLBACK PLAYBOOK
# ==============================================================================
#
# Emergency procedures for mainnet incidents
# Keep this script ready but hope you never need it
#
# ==============================================================================

set -Eeuo pipefail
trap 'code=$?; echo "[FATAL] $0 failed at line $LINENO (exit $code)"; exit $code' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Setup emergency logging
mkdir -p "$PROJECT_ROOT/logs/incidents"
INCIDENT_ID="incident_$(date +%Y%m%d_%H%M%S)"
exec > >(tee -a "$PROJECT_ROOT/logs/incidents/${INCIDENT_ID}.log") 2>&1

echo "🚨 DINERO INCIDENT RESPONSE PLAYBOOK"
echo "===================================="
echo "Incident ID: $INCIDENT_ID"
echo "Response time: $(date)"
echo "Operator: $(whoami)@$(hostname)"
echo ""

# Parse incident type
INCIDENT_TYPE="${1:-unknown}"

case "$INCIDENT_TYPE" in
    "genesis-mismatch")
        echo "🔥 INCIDENT TYPE: Genesis/CIC Mismatch"
        echo "======================================"
        ;;
    "deep-reorg")
        echo "🔥 INCIDENT TYPE: Deep Reorganization Attack"
        echo "==========================================="
        ;;
    "safe-mode")
        echo "🔥 INCIDENT TYPE: Safe-Mode Activation"
        echo "====================================="
        ;;
    "network-split")
        echo "🔥 INCIDENT TYPE: Network Split/Partition"
        echo "========================================"
        ;;
    "mining-halt")
        echo "🔥 INCIDENT TYPE: Mining Halt/Stall"
        echo "=================================="
        ;;
    *)
        echo "🔥 INCIDENT TYPE: General Emergency"
        echo "================================="
        echo "Usage: $0 [genesis-mismatch|deep-reorg|safe-mode|network-split|mining-halt]"
        ;;
esac

echo ""

# =============================================================================
# IMMEDIATE ASSESSMENT
# =============================================================================

echo "📊 IMMEDIATE NETWORK ASSESSMENT"
echo "==============================="
echo ""

# RPC helper
rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    timeout 10 curl -s -H 'content-type: application/json' \
         -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}" \
         "http://127.0.0.1:20999" 2>/dev/null || echo '{"error": "RPC timeout/failed"}'
}

# Collect critical network state
echo "🔍 Collecting network state..."

BLOCK_COUNT=$(rpc_call "getblockcount" | jq -r '.result // "ERROR"')
CHAIN_WORK=$(rpc_call "getchainwork" | jq -r '.result // "ERROR"')
PEER_COUNT=$(rpc_call "getconnectioncount" | jq -r '.result // "ERROR"')
CHAIN_TIPS=$(rpc_call "getchaintips")
REORG_STATUS=$(rpc_call "getreorgstatus")

echo "📋 Current Network State:"
echo "  Block Height: $BLOCK_COUNT"
echo "  Chain Work: ${CHAIN_WORK: -16}..."
echo "  Peer Count: $PEER_COUNT"
echo ""

# Check safe-mode status
if echo "$REORG_STATUS" | jq -e '.result.safe_mode.active' >/dev/null 2>&1; then
    SAFE_MODE=$(echo "$REORG_STATUS" | jq -r '.result.safe_mode.active')
    if [[ "$SAFE_MODE" == "true" ]]; then
        SAFE_MODE_REASON=$(echo "$REORG_STATUS" | jq -r '.result.safe_mode.reason // "unknown"')
        echo "🚨 SAFE-MODE: ACTIVE"
        echo "   Reason: $SAFE_MODE_REASON"
    else
        echo "✅ Safe-mode: INACTIVE"
    fi
fi

# Check for recent reorgs
if echo "$REORG_STATUS" | jq -e '.result.last_reorg' >/dev/null 2>&1; then
    LAST_REORG=$(echo "$REORG_STATUS" | jq -r '.result.last_reorg')
    if [[ "$LAST_REORG" != "null" ]]; then
        REORG_DEPTH=$(echo "$LAST_REORG" | jq -r '.disconnect_depth // 0')
        REORG_TIME=$(echo "$LAST_REORG" | jq -r '.timestamp // 0')
        echo "⚠️  Recent reorg: $REORG_DEPTH blocks at $(date -d "@$REORG_TIME" 2>/dev/null || echo "unknown time")"
    fi
fi

echo ""

# =============================================================================
# INCIDENT-SPECIFIC RESPONSE
# =============================================================================

case "$INCIDENT_TYPE" in
    "genesis-mismatch")
        echo "🛑 GENESIS/CIC MISMATCH RESPONSE"
        echo "==============================="
        echo ""
        echo "⚠️  CRITICAL: Different genesis/CIC detected across nodes"
        echo ""
        echo "🚨 IMMEDIATE ACTIONS:"
        echo "  1. HALT ALL MINING immediately"
        echo "  2. STOP seed nodes"
        echo "  3. PUBLISH advisory to community"
        echo ""
        
        # Stop mining if running
        if pgrep -f "miningaddress=" >/dev/null; then
            echo "🛑 Stopping mining processes..."
            pkill -f "miningaddress=" || true
            echo "✅ Mining halted"
        fi
        
        echo "📋 RECOVERY PROCEDURE:"
        echo "  1. Investigate genesis mismatch source"
        echo "  2. Re-run genesis ceremony with new parameters"
        echo "  3. Bump CIC to new value"
        echo "  4. Retag as v1.0.1-mainnet"
        echo "  5. Coordinate network restart"
        echo ""
        echo "🔧 Commands to run:"
        echo "   ./scripts/deploy/genesis_ceremony.sh --emergency-redo"
        echo "   git tag v1.0.1-mainnet"
        echo "   ./scripts/deploy/GO_LIVE.sh --confirm-launch"
        ;;
        
    "deep-reorg")
        echo "🔄 DEEP REORGANIZATION RESPONSE"
        echo "=============================="
        echo ""
        echo "⚠️  Deep reorg detected - coordinating network response"
        echo ""
        
        # Get reorg details
        if echo "$REORG_STATUS" | jq -e '.result.last_reorg.disconnect_depth' >/dev/null 2>&1; then
            REORG_DEPTH=$(echo "$REORG_STATUS" | jq -r '.result.last_reorg.disconnect_depth')
            echo "📊 Reorg depth: $REORG_DEPTH blocks"
            
            if [[ "$REORG_DEPTH" -ge 30 ]]; then
                echo "🚨 CRITICAL: Reorg depth >= 30 blocks"
                echo "   Safe-mode should be automatically active"
            fi
        fi
        
        echo ""
        echo "📋 COORDINATION ACTIONS:"
        echo "  1. Communicate reorg depth to community"
        echo "  2. Share chainwork views with pools"
        echo "  3. Coordinate on accepted tip"
        echo "  4. Monitor for continued attacks"
        echo ""
        
        # Show competing tips
        echo "🔗 Chain tips analysis:"
        if echo "$CHAIN_TIPS" | jq -e '.result' >/dev/null 2>&1; then
            echo "$CHAIN_TIPS" | jq -r '.result[] | "  \(.status): height=\(.height) work=\(.chainwork[48:64])... hash=\(.hash[0:16])..."'
        fi
        ;;
        
    "safe-mode")
        echo "🛡️ SAFE-MODE RESPONSE"
        echo "===================="
        echo ""
        echo "ℹ️  Safe-mode activated - network protection engaged"
        echo ""
        
        if echo "$REORG_STATUS" | jq -e '.result.safe_mode.reason' >/dev/null 2>&1; then
            SAFE_MODE_REASON=$(echo "$REORG_STATUS" | jq -r '.result.safe_mode.reason')
            echo "📋 Safe-mode reason: $SAFE_MODE_REASON"
        fi
        
        echo ""
        echo "📋 SAFE-MODE PROCEDURES:"
        echo "  1. Investigate trigger cause"
        echo "  2. Verify network consensus"
        echo "  3. Coordinate with other operators"
        echo "  4. Consider reconsiderblock if needed"
        echo ""
        echo "🔧 Commands available:"
        echo "   dinero-cli getreorgstatus"
        echo "   dinero-cli getchaintips"
        echo "   dinero-cli reconsiderblock <hash>  # If consensus reached"
        ;;
        
    "network-split")
        echo "🌐 NETWORK SPLIT RESPONSE"
        echo "========================"
        echo ""
        echo "⚠️  Network partition detected"
        echo ""
        echo "📊 Peer analysis:"
        echo "   Connected peers: $PEER_COUNT"
        
        if [[ "$PEER_COUNT" -lt 4 ]]; then
            echo "   🚨 LOW PEER COUNT - possible isolation"
        fi
        
        echo ""
        echo "📋 SPLIT RECOVERY:"
        echo "  1. Check seed node connectivity"
        echo "  2. Verify DNS seed responses"
        echo "  3. Manual peer connections if needed"
        echo "  4. Monitor for network healing"
        echo ""
        echo "🔧 Commands to try:"
        echo "   dinero-cli addnode <seed_ip>:40999 add"
        echo "   dinero-cli getpeerinfo"
        ;;
        
    "mining-halt")
        echo "⛏️  MINING HALT RESPONSE"
        echo "======================="
        echo ""
        echo "⚠️  Mining appears stalled"
        echo ""
        
        # Check last block time
        if [[ "$BLOCK_COUNT" != "ERROR" ]] && [[ "$BLOCK_COUNT" -gt 0 ]]; then
            LAST_BLOCK=$(rpc_call "getblock" "[$BLOCK_COUNT]")
            if echo "$LAST_BLOCK" | jq -e '.result.time' >/dev/null 2>&1; then
                LAST_BLOCK_TIME=$(echo "$LAST_BLOCK" | jq -r '.result.time')
                CURRENT_TIME=$(date +%s)
                TIME_SINCE_BLOCK=$(( CURRENT_TIME - LAST_BLOCK_TIME ))
                
                echo "📊 Last block: $((TIME_SINCE_BLOCK / 60)) minutes ago"
                
                if [[ "$TIME_SINCE_BLOCK" -gt 1800 ]]; then  # 30 minutes
                    echo "🚨 STALL: No blocks for >30 minutes"
                fi
            fi
        fi
        
        echo ""
        echo "📋 MINING RECOVERY:"
        echo "  1. Check difficulty adjustment"
        echo "  2. Verify mining pool connectivity"
        echo "  3. Restart local mining if needed"
        echo "  4. Community mining coordination"
        echo ""
        echo "🔧 Commands to check:"
        echo "   dinero-cli getmininginfo"
        echo "   dinero-cli getblocktemplate"
        ;;
esac

echo ""

# =============================================================================
# COMMUNICATION TEMPLATE
# =============================================================================

echo "📢 INCIDENT COMMUNICATION TEMPLATE"
echo "=================================="
echo ""

cat << EOF
🚨 DINERO NETWORK INCIDENT ALERT

Incident ID: $INCIDENT_ID
Type: $INCIDENT_TYPE
Time: $(date)
Status: INVESTIGATING

Network State:
- Block Height: $BLOCK_COUNT
- Peer Count: $PEER_COUNT
- Safe Mode: $(echo "$REORG_STATUS" | jq -r '.result.safe_mode.active // "unknown"')

Actions Taken:
- [ ] Incident response activated
- [ ] Network state assessed
- [ ] Stakeholders notified
- [ ] Recovery procedures initiated

Next Update: [TIME]

For technical details: https://github.com/dinerocoin/dinero/issues
Contact: security@dinero-coin.com

#DineroIncident #NetworkAlert
EOF

echo ""

# =============================================================================
# RECOVERY CHECKLIST
# =============================================================================

echo "✅ RECOVERY CHECKLIST"
echo "===================="
echo ""

echo "📋 Immediate (0-15 minutes):"
echo "  [ ] Assess incident severity"
echo "  [ ] Stop mining if critical"
echo "  [ ] Notify core team"
echo "  [ ] Document incident details"
echo ""

echo "📋 Short-term (15-60 minutes):"
echo "  [ ] Coordinate with other operators"
echo "  [ ] Implement emergency fixes"
echo "  [ ] Communicate to community"
echo "  [ ] Monitor recovery progress"
echo ""

echo "📋 Long-term (1+ hours):"
echo "  [ ] Conduct post-incident review"
echo "  [ ] Update procedures"
echo "  [ ] Implement preventive measures"
echo "  [ ] Publish incident report"
echo ""

# =============================================================================
# EMERGENCY CONTACTS
# =============================================================================

echo "📞 EMERGENCY CONTACTS"
echo "===================="
echo ""

echo "🔧 Technical Team:"
echo "  - Core Developer: [CONTACT_INFO]"
echo "  - Operations Lead: [CONTACT_INFO]"
echo "  - Security Team: security@dinero-coin.com"
echo ""

echo "📢 Communications:"
echo "  - Community Manager: [CONTACT_INFO]"
echo "  - Social Media: @dinerocoin"
echo "  - Discord: #emergency-alerts"
echo ""

echo "🏢 External Partners:"
echo "  - Major Mining Pools: [CONTACT_LIST]"
echo "  - Exchange Partners: [CONTACT_LIST]"
echo "  - Infrastructure Providers: [CONTACT_LIST]"
echo ""

# =============================================================================
# INCIDENT LOG
# =============================================================================

echo "📝 INCIDENT LOG ENTRY"
echo "===================="
echo ""

cat << EOF >> "$PROJECT_ROOT/logs/incidents/incident_log.txt"
================================================================================
INCIDENT: $INCIDENT_ID
================================================================================
Type: $INCIDENT_TYPE
Start Time: $(date)
Operator: $(whoami)@$(hostname)

Network State at Detection:
- Block Height: $BLOCK_COUNT
- Chain Work: $CHAIN_WORK
- Peer Count: $PEER_COUNT
- Safe Mode: $(echo "$REORG_STATUS" | jq -r '.result.safe_mode.active // "unknown"')

Actions Taken:
- Incident response playbook executed
- Network assessment completed
- [Additional actions to be logged]

Resolution:
- [To be updated when resolved]

Lessons Learned:
- [To be added during post-incident review]

================================================================================
EOF

echo "✅ Incident logged to: logs/incidents/incident_log.txt"
echo "✅ Full incident log: logs/incidents/${INCIDENT_ID}.log"
echo ""

echo "🎯 INCIDENT RESPONSE ACTIVATED"
echo ""
echo "Next steps:"
echo "1. Follow incident-specific procedures above"
echo "2. Coordinate with team using communication template"
echo "3. Monitor recovery using: ./post_launch_checks.sh --continuous"
echo "4. Update incident log as situation evolves"
echo ""

echo "🚨 Remember: Stay calm, communicate clearly, document everything!"
