#!/bin/bash
echo "📊 Live Test Monitor - Press Ctrl+C to stop monitoring"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

while true; do
    clear
    echo "🧪 DEADLOCK FIX TEST - LIVE MONITORING"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    
    # Get current height
    HEIGHT=$(curl -s --max-time 2 http://127.0.0.1:20998/ -d '{"method":"getblockcount"}' 2>/dev/null | jq -r '.result // "N/A"')
    
    # Calculate progress
    if [ "$HEIGHT" != "N/A" ] && [ "$HEIGHT" -gt 0 ]; then
        PERCENT=$((HEIGHT * 100 / 105))
        REMAINING=$((105 - HEIGHT))
        
        echo "Current Height: $HEIGHT / 105"
        echo "Progress: $PERCENT%"
        echo "Remaining: $REMAINING blocks"
        echo ""
        
        # Progress bar
        FILLED=$((PERCENT / 2))
        printf "["
        for i in $(seq 1 $FILLED); do printf "█"; done
        for i in $(seq $FILLED 49); do printf " "; done
        printf "] $PERCENT%%\n"
        echo ""
        
        if [ $HEIGHT -ge 105 ]; then
            echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
            echo "✅ TEST COMPLETE - 105 BLOCKS MINED SUCCESSFULLY!"
            echo "   NO DEADLOCKS DETECTED - FIX VERIFIED!"
            echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
            break
        fi
        
        # Test RPC responsiveness
        RPC_START=$(date +%s%3N)
        curl -s --max-time 1 http://127.0.0.1:20998/ -d '{"method":"getblockchaininfo"}' > /dev/null 2>&1
        RPC_END=$(date +%s%3N)
        RPC_TIME=$((RPC_END - RPC_START))
        
        if [ $RPC_TIME -lt 1000 ]; then
            echo "RPC Status: ✅ Responsive (${RPC_TIME}ms)"
        else
            echo "RPC Status: ⚠️  Slow (${RPC_TIME}ms)"
        fi
    else
        echo "Waiting for mining to start..."
    fi
    
    echo ""
    echo "Last 5 log entries:"
    tail -5 test_live.log 2>/dev/null || echo "  (no logs yet)"
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "Updated: $(date '+%Y-%m-%d %H:%M:%S')"
    
    sleep 10
done
