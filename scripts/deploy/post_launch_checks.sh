#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO POST-LAUNCH VALIDATION
# ==============================================================================
#
# Immediate post-launch checks to verify network health
# Run this immediately after mainnet launch
#
# Usage: ./post_launch_checks.sh [--continuous]
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
RPC_URL="http://127.0.0.1:20999"
HEALTH_URL="http://127.0.0.1:22001/healthz"
METRICS_URL="http://127.0.0.1:22001/metrics"
CONTINUOUS_MODE=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --continuous)
            CONTINUOUS_MODE=true
            shift
            ;;
        --help)
            echo "Usage: $0 [--continuous]"
            echo ""
            echo "Options:"
            echo "  --continuous    Run checks continuously every 30 seconds"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# RPC helper function
rpc_call() {
    local method="$1"
    local params="${2:-[]}"
    
    curl -s -H 'content-type: application/json' \
         -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$method\",\"params\":$params}" \
         "$RPC_URL" 2>/dev/null || echo '{"error": "RPC call failed"}'
}

# Health check function
check_health() {
    echo "🏥 DINERO MAINNET HEALTH CHECK"
    echo "============================="
    echo "Timestamp: $(date)"
    echo "RPC URL: $RPC_URL"
    echo "Health URL: $HEALTH_URL"
    echo ""
    
    local all_healthy=true
    
    # ==========================================================================
    # 1. HTTP HEALTH ENDPOINT
    # ==========================================================================
    
    echo "📡 1. HTTP Health Endpoint"
    echo "-------------------------"
    
    if curl -s -f "$HEALTH_URL" >/dev/null 2>&1; then
        HEALTH_RESPONSE=$(curl -s "$HEALTH_URL" 2>/dev/null || echo "ERROR")
        echo "✅ Health endpoint: HEALTHY"
        echo "   Response: $HEALTH_RESPONSE"
    else
        echo "❌ Health endpoint: FAILED"
        all_healthy=false
    fi
    
    echo ""
    
    # ==========================================================================
    # 2. RPC CONNECTIVITY
    # ==========================================================================
    
    echo "🔌 2. RPC Connectivity"
    echo "---------------------"
    
    # Test basic RPC
    BLOCK_COUNT_RESPONSE=$(rpc_call "getblockcount")
    if echo "$BLOCK_COUNT_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        BLOCK_COUNT=$(echo "$BLOCK_COUNT_RESPONSE" | jq -r '.result')
        echo "✅ RPC connectivity: WORKING"
        echo "   Block height: $BLOCK_COUNT"
    else
        echo "❌ RPC connectivity: FAILED"
        echo "   Error: $(echo "$BLOCK_COUNT_RESPONSE" | jq -r '.error.message // "Unknown error"')"
        all_healthy=false
    fi
    
    echo ""
    
    # ==========================================================================
    # 3. CHAIN TIPS
    # ==========================================================================
    
    echo "🔗 3. Chain Tips"
    echo "---------------"
    
    CHAIN_TIPS_RESPONSE=$(rpc_call "getchaintips")
    if echo "$CHAIN_TIPS_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        echo "✅ Chain tips: AVAILABLE"
        
        # Parse and display tips
        echo "$CHAIN_TIPS_RESPONSE" | jq -r '.result[] | "   \(.status): height=\(.height) work=\(.chainwork[48:64])... hash=\(.hash[0:16])..."' 2>/dev/null || echo "   (parsing failed)"
        
        # Count active vs fork tips
        ACTIVE_TIPS=$(echo "$CHAIN_TIPS_RESPONSE" | jq '[.result[] | select(.status == "active")] | length' 2>/dev/null || echo "0")
        FORK_TIPS=$(echo "$CHAIN_TIPS_RESPONSE" | jq '[.result[] | select(.status != "active")] | length' 2>/dev/null || echo "0")
        
        echo "   Active tips: $ACTIVE_TIPS"
        echo "   Fork tips: $FORK_TIPS"
        
        if [[ "$ACTIVE_TIPS" != "1" ]]; then
            echo "⚠️  Warning: Expected exactly 1 active tip, found $ACTIVE_TIPS"
        fi
    else
        echo "❌ Chain tips: FAILED"
        all_healthy=false
    fi
    
    echo ""
    
    # ==========================================================================
    # 4. CHAINWORK
    # ==========================================================================
    
    echo "⚡ 4. Chainwork"
    echo "-------------"
    
    CHAINWORK_RESPONSE=$(rpc_call "getchainwork")
    if echo "$CHAINWORK_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        CHAINWORK=$(echo "$CHAINWORK_RESPONSE" | jq -r '.result')
        echo "✅ Chainwork: AVAILABLE"
        echo "   Full: $CHAINWORK"
        echo "   Last 16 chars: ...${CHAINWORK: -16}"
        
        # Validate chainwork is not all zeros
        if [[ "$CHAINWORK" =~ ^0+$ ]]; then
            echo "⚠️  Warning: Chainwork is all zeros (genesis only?)"
        fi
    else
        echo "❌ Chainwork: FAILED"
        all_healthy=false
    fi
    
    echo ""
    
    # ==========================================================================
    # 5. REORG STATUS / SAFE-MODE
    # ==========================================================================
    
    echo "🔄 5. Reorg Status / Safe-Mode"
    echo "-----------------------------"
    
    REORG_STATUS_RESPONSE=$(rpc_call "getreorgstatus")
    if echo "$REORG_STATUS_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        echo "✅ Reorg status: AVAILABLE"
        
        # Parse safe-mode status
        SAFE_MODE=$(echo "$REORG_STATUS_RESPONSE" | jq -r '.result.safe_mode.active // false')
        if [[ "$SAFE_MODE" == "true" ]]; then
            echo "🚨 SAFE-MODE: ACTIVE"
            SAFE_MODE_REASON=$(echo "$REORG_STATUS_RESPONSE" | jq -r '.result.safe_mode.reason // "unknown"')
            echo "   Reason: $SAFE_MODE_REASON"
            all_healthy=false
        else
            echo "   Safe-mode: INACTIVE (good)"
        fi
        
        # Parse last reorg info
        LAST_REORG=$(echo "$REORG_STATUS_RESPONSE" | jq -r '.result.last_reorg // {}')
        if [[ "$LAST_REORG" != "{}" ]]; then
            REORG_DEPTH=$(echo "$LAST_REORG" | jq -r '.disconnect_depth // 0')
            REORG_DURATION=$(echo "$LAST_REORG" | jq -r '.duration_ms // 0')
            echo "   Last reorg: ${REORG_DEPTH} blocks in ${REORG_DURATION}ms"
        else
            echo "   Last reorg: None"
        fi
    else
        echo "❌ Reorg status: FAILED"
        all_healthy=false
    fi
    
    echo ""
    
    # ==========================================================================
    # 6. MINING READINESS (BLOCK TEMPLATE)
    # ==========================================================================
    
    echo "⛏️  6. Mining Readiness"
    echo "---------------------"
    
    BLOCK_TEMPLATE_RESPONSE=$(rpc_call "getblocktemplate")
    if echo "$BLOCK_TEMPLATE_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        echo "✅ Block template: AVAILABLE"
        
        # Parse template details
        TEMPLATE_HEIGHT=$(echo "$BLOCK_TEMPLATE_RESPONSE" | jq -r '.result.height // "unknown"')
        TEMPLATE_BITS=$(echo "$BLOCK_TEMPLATE_RESPONSE" | jq -r '.result.bits // "unknown"')
        TEMPLATE_SUBSIDY=$(echo "$BLOCK_TEMPLATE_RESPONSE" | jq -r '.result.subsidy_din // "unknown"')
        TEMPLATE_TXS=$(echo "$BLOCK_TEMPLATE_RESPONSE" | jq -r '.result.transactions | length // 0')
        
        echo "   Height: $TEMPLATE_HEIGHT"
        echo "   Bits: $TEMPLATE_BITS"
        echo "   Subsidy: $TEMPLATE_SUBSIDY DIN"
        echo "   Transactions: $TEMPLATE_TXS"
        
        # Validate template is not null/empty
        if [[ "$TEMPLATE_HEIGHT" == "null" || "$TEMPLATE_HEIGHT" == "unknown" ]]; then
            echo "⚠️  Warning: Block template appears invalid"
        fi
    else
        echo "❌ Block template: FAILED"
        echo "   Error: $(echo "$BLOCK_TEMPLATE_RESPONSE" | jq -r '.error.message // "Unknown error"')"
        all_healthy=false
    fi
    
    echo ""
    
    # ==========================================================================
    # 7. NETWORK CONNECTIVITY
    # ==========================================================================
    
    echo "🌐 7. Network Connectivity"
    echo "-------------------------"
    
    # Get peer count
    PEER_COUNT_RESPONSE=$(rpc_call "getconnectioncount")
    if echo "$PEER_COUNT_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        PEER_COUNT=$(echo "$PEER_COUNT_RESPONSE" | jq -r '.result')
        echo "✅ Peer connectivity: WORKING"
        echo "   Connected peers: $PEER_COUNT"
        
        if [[ "$PEER_COUNT" -eq 0 ]]; then
            echo "⚠️  Warning: No peers connected (isolated node)"
        elif [[ "$PEER_COUNT" -lt 4 ]]; then
            echo "⚠️  Warning: Low peer count ($PEER_COUNT < 4)"
        fi
    else
        echo "❌ Peer connectivity: FAILED"
        all_healthy=false
    fi
    
    # Get network info
    NETWORK_INFO_RESPONSE=$(rpc_call "getnetworkinfo")
    if echo "$NETWORK_INFO_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        NETWORK_NAME=$(echo "$NETWORK_INFO_RESPONSE" | jq -r '.result.networkname // "unknown"')
        P2P_PORT=$(echo "$NETWORK_INFO_RESPONSE" | jq -r '.result.localservices // "unknown"')
        echo "   Network: $NETWORK_NAME"
    fi
    
    echo ""
    
    # ==========================================================================
    # 8. COOKIE SECURITY & SUPPLY SANITY
    # ==========================================================================
    
    echo "🔒 8. Cookie Security & Supply Sanity"
    echo "------------------------------------"
    
    # Check cookie permissions
    COOKIE_FILE="/var/lib/dinerod/mainnet/.cookie"
    if [[ -f "$COOKIE_FILE" ]]; then
        COOKIE_PERMS=$(stat -c "%a" "$COOKIE_FILE" 2>/dev/null || stat -f "%Lp" "$COOKIE_FILE" 2>/dev/null || echo "unknown")
        if [[ "$COOKIE_PERMS" == "600" ]]; then
            echo "✅ Cookie permissions: secure ($COOKIE_PERMS)"
        else
            echo "⚠️  Cookie permissions: $COOKIE_PERMS (should be 600)"
            all_healthy=false
        fi
    else
        # Try alternative locations
        for cookie_path in "$HOME/.dinero/.cookie" "./data/.cookie"; do
            if [[ -f "$cookie_path" ]]; then
                COOKIE_PERMS=$(stat -c "%a" "$cookie_path" 2>/dev/null || stat -f "%Lp" "$cookie_path" 2>/dev/null || echo "unknown")
                echo "   Cookie found at: $cookie_path (perms: $COOKIE_PERMS)"
                break
            fi
        done
    fi
    
    # Supply sanity check (post-genesis)
    SUPPLY_INFO_RESPONSE=$(rpc_call "getsupplyinfo")
    if echo "$SUPPLY_INFO_RESPONSE" | jq -e '.result' >/dev/null 2>&1; then
        TOTAL_SUPPLY=$(echo "$SUPPLY_INFO_RESPONSE" | jq -r '.result.total_supply // 0')
        PREMINE_SUPPLY=$(echo "$SUPPLY_INFO_RESPONSE" | jq -r '.result.premine_supply // 0')
        
        echo "   Total supply: $TOTAL_SUPPLY DIN"
        echo "   Premine: $PREMINE_SUPPLY DIN"
        
        # Validate supply makes sense
        if [[ "$TOTAL_SUPPLY" -gt 0 ]] && [[ "$PREMINE_SUPPLY" -eq 2000000 ]]; then
            echo "✅ Supply sanity: Valid"
        else
            echo "⚠️  Supply sanity: May be incorrect (total: $TOTAL_SUPPLY, premine: $PREMINE_SUPPLY)"
        fi
    else
        echo "   Supply info: Not available (RPC may not support getsupplyinfo)"
    fi
    
    echo ""
    
    # ==========================================================================
    # 9. SYSTEM RESOURCES
    # ==========================================================================
    
    echo "💻 9. System Resources"
    echo "---------------------"
    
    # Check if dinerod process is running
    if pgrep -f dinerod >/dev/null; then
        DINEROD_PID=$(pgrep -f dinerod | head -1)
        echo "✅ Process: RUNNING (PID: $DINEROD_PID)"
        
        # Get CPU and memory usage
        if command -v ps >/dev/null 2>&1; then
            CPU_USAGE=$(ps -p "$DINEROD_PID" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "unknown")
            MEM_USAGE=$(ps -p "$DINEROD_PID" -o %mem --no-headers 2>/dev/null | tr -d ' ' || echo "unknown")
            echo "   CPU usage: ${CPU_USAGE}%"
            echo "   Memory usage: ${MEM_USAGE}%"
        fi
    else
        echo "❌ Process: NOT RUNNING"
        all_healthy=false
    fi
    
    # System load
    if [[ -f /proc/loadavg ]]; then
        LOAD_AVG=$(cat /proc/loadavg | cut -d' ' -f1-3)
        echo "   Load average: $LOAD_AVG"
    fi
    
    # Disk space
    if command -v df >/dev/null 2>&1; then
        DISK_USAGE=$(df -h /var/lib/dinerod 2>/dev/null | awk 'NR==2 {print $5}' || echo "unknown")
        echo "   Disk usage: $DISK_USAGE"
    fi
    
    echo ""
    
    # ==========================================================================
    # OVERALL HEALTH SUMMARY
    # ==========================================================================
    
    echo "🎯 OVERALL HEALTH SUMMARY"
    echo "========================"
    
    if [[ "$all_healthy" == true ]]; then
        echo "🎉 ALL SYSTEMS HEALTHY!"
        echo ""
        echo "✅ Dinero mainnet is operating normally"
        echo "🚀 Network is ready for production use"
        echo "⛏️  Mining operations can proceed"
        
        return 0
    else
        echo "⚠️  SOME ISSUES DETECTED!"
        echo ""
        echo "🔍 Review the failed checks above"
        echo "🛠️  Address issues before declaring launch successful"
        echo "📞 Consider alerting operations team"
        
        return 1
    fi
}

# =============================================================================
# MAIN EXECUTION
# =============================================================================

if [[ "$CONTINUOUS_MODE" == true ]]; then
    echo "🔄 CONTINUOUS MONITORING MODE"
    echo "Press Ctrl+C to stop"
    echo ""
    
    while true; do
        check_health
        echo ""
        echo "⏰ Next check in 30 seconds..."
        echo "$(printf '=%.0s' {1..50})"
        echo ""
        sleep 30
    done
else
    # Single check
    check_health
    exit $?
fi
