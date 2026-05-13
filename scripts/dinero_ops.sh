#!/bin/bash
# Dinero Operations Helper Script
# Production-ready mining and blockchain monitoring tools

# Configuration
export RPC="http://127.0.0.1:20998"
export COOKIE="$(tr -d '\r\n' < /Users/haydarevich/Documents/DineroCoin/data/.cookie)"

# RPC helper function
din() {
    curl -s --user "$COOKIE" -H "content-type: application/json" "$@"
}

# Mining status watch (real-time)
watch_mining() {
    echo "🔍 Watching Dinero mining status (Ctrl+C to stop)..."
    watch -n1 'curl -s --user "$COOKIE" -H "content-type: application/json" --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getmininginfo\",\"params\":[]}" "$RPC" | jq "{hps:.result.hashrate_hps, bits:.result.difficulty_bits, height:.result.blocks, target:.result.target}"'
}

# Block interval analysis (last N blocks)
block_intervals() {
    local N=${1:-50}
    echo "📊 Analyzing block intervals for last $N blocks..."
    
    H=$(curl -s --user "$COOKIE" -H "content-type: application/json" --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' "$RPC" | jq -r .result)
    echo "Current height: $H"
    
    # Get timestamps for last N blocks
    local timestamps=()
    for ((i=H-N+1; i<=H; i++)); do
        local hash=$(curl -s --user "$COOKIE" -H "content-type: application/json" --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockhash\",\"params\":[$i]}" "$RPC" | jq -r .result)
        local time=$(curl -s --user "$COOKIE" -H "content-type: application/json" --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblock\",\"params\":[\"$hash\",true]}" "$RPC" | jq -r .result.time)
        timestamps+=($time)
    done
    
    # Calculate intervals
    local intervals=()
    for ((i=1; i<${#timestamps[@]}; i++)); do
        local interval=$((${timestamps[i]} - ${timestamps[i-1]}))
        intervals+=($interval)
    done
    
    # Calculate stats
    local sum=0
    local min=${intervals[0]}
    local max=${intervals[0]}
    for interval in "${intervals[@]}"; do
        sum=$((sum + interval))
        if [ $interval -lt $min ]; then min=$interval; fi
        if [ $interval -gt $max ]; then max=$interval; fi
    done
    
    local count=${#intervals[@]}
    local avg=$((sum / count))
    
    echo "count=$count, avg_s=$avg, min_s=$min, max_s=$max"
}

# Mining control
start_mining() {
    local threads=${1:-2}
    echo "⛏️ Starting mining with $threads threads..."
    din --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setgenerate\",\"params\":[true,$threads]}" "$RPC" | jq .
}

stop_mining() {
    echo "⏹️ Stopping mining..."
    din --data '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[false]}' "$RPC" | jq .
}

# Blockchain info
get_height() {
    din --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' "$RPC" | jq -r .result
}

get_mining_info() {
    din --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' "$RPC" | jq .
}

# Health check
health_check() {
    echo "🏥 Dinero Health Check"
    echo "====================="
    
    # Check if daemon is running
    if ! curl -s --user "$COOKIE" "$RPC" > /dev/null 2>&1; then
        echo "❌ Daemon not responding"
        return 1
    fi
    
    # Get current status
    local info=$(get_mining_info)
    local height=$(echo "$info" | jq -r .result.blocks)
    local hashrate=$(echo "$info" | jq -r .result.hashrate_hps)
    local bits=$(echo "$info" | jq -r .result.difficulty_bits)
    local target=$(echo "$info" | jq -r .result.target)
    local enabled=$(echo "$info" | jq -r .result.mining_enabled)
    
    echo "✅ Daemon responding"
    echo "📊 Height: $height"
    echo "⛏️ Mining: $enabled"
    echo "🚀 Hashrate: $hashrate H/s"
    echo "🎯 Difficulty: 0x$(printf '%x' $bits)"
    echo "🎯 Target: ${target:0:8}..."
    
    # Check if target is canonical
    if [[ "$target" == "00002710"* ]]; then
        echo "✅ Target format: Canonical (CPU-friendly)"
    else
        echo "⚠️ Target format: Non-canonical"
    fi
    
    # Check hashrate
    if (( $(echo "$hashrate > 100000" | bc -l) )); then
        echo "✅ Hashrate: Healthy (>100k H/s)"
    else
        echo "⚠️ Hashrate: Low (<100k H/s)"
    fi
    
    echo "====================="
}

# Main menu
case "$1" in
    "watch")
        watch_mining
        ;;
    "intervals")
        block_intervals "$2"
        ;;
    "start")
        start_mining "$2"
        ;;
    "stop")
        stop_mining
        ;;
    "height")
        get_height
        ;;
    "info")
        get_mining_info
        ;;
    "health")
        health_check
        ;;
    *)
        echo "Dinero Operations Helper"
        echo "Usage: $0 {watch|intervals|start|stop|height|info|health}"
        echo ""
        echo "Commands:"
        echo "  watch      - Real-time mining status"
        echo "  intervals  - Block interval analysis (default: 50 blocks)"
        echo "  start      - Start mining (default: 2 threads)"
        echo "  stop       - Stop mining"
        echo "  height     - Get current block height"
        echo "  info       - Get full mining info"
        echo "  health     - Run health check"
        echo ""
        echo "Examples:"
        echo "  $0 watch                    # Watch mining in real-time"
        echo "  $0 intervals 100            # Analyze last 100 blocks"
        echo "  $0 start 4                  # Start mining with 4 threads"
        echo "  $0 health                   # Run health check"
        ;;
esac
