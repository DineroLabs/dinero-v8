#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO MINING LAUNCH SCRIPT
# ==============================================================================
#
# This script sets up and launches Dinero mining operations.
# Supports solo mining, pool mining (future), and mining farms.
#
# Usage: ./launch_mining.sh [options]
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Default configuration
MINING_THREADS=0  # 0 = auto-detect
MINING_ADDRESS=""
EXTRA_NONCE_START=$(date +%s)
GENERATE_ADDRESS=false
POOL_URL=""
POOL_USERNAME=""
SOLO_MINING=true

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --threads)
            MINING_THREADS="$2"
            shift 2
            ;;
        --address)
            MINING_ADDRESS="$2"
            shift 2
            ;;
        --generate-address)
            GENERATE_ADDRESS=true
            shift
            ;;
        --pool)
            POOL_URL="$2"
            SOLO_MINING=false
            shift 2
            ;;
        --pool-user)
            POOL_USERNAME="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Mining Options:"
            echo "  --threads N           Number of mining threads (0=auto)"
            echo "  --address ADDR        Mining address (din1...)"
            echo "  --generate-address    Generate new mining address"
            echo ""
            echo "Pool Mining (Future):"
            echo "  --pool URL            Pool stratum URL"
            echo "  --pool-user USER      Pool username"
            echo ""
            echo "Examples:"
            echo "  $0 --generate-address --threads 4"
            echo "  $0 --address din1q... --threads 8"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo "⛏️  DINERO MINING LAUNCH"
echo "======================"
echo "Solo Mining: $SOLO_MINING"
echo "Threads: $MINING_THREADS (0=auto)"
echo ""

# =============================================================================
# SYSTEM CHECKS
# =============================================================================

echo "🔍 System Checks"
echo "---------------"

# Check if dinerod is available
if ! command -v dinerod >/dev/null 2>&1; then
    echo "❌ dinerod not found in PATH"
    echo "Please install Dinero first or add it to PATH"
    exit 1
fi

# Check if dinero-cli is available
if ! command -v dinero-cli >/dev/null 2>&1; then
    echo "❌ dinero-cli not found in PATH"
    echo "Please install Dinero CLI or add it to PATH"
    exit 1
fi

# Auto-detect thread count if not specified
if [[ $MINING_THREADS -eq 0 ]]; then
    MINING_THREADS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    echo "Auto-detected $MINING_THREADS CPU cores"
fi

# Check system resources
TOTAL_CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TOTAL_MEM_GB=$(free -g 2>/dev/null | awk '/^Mem:/ {print $2}' || echo "unknown")

echo "System specs:"
echo "  CPU cores: $TOTAL_CORES"
echo "  Memory: ${TOTAL_MEM_GB}GB"
echo "  Mining threads: $MINING_THREADS"

if [[ $MINING_THREADS -gt $TOTAL_CORES ]]; then
    echo "⚠️  Warning: More mining threads than CPU cores"
fi

echo "✅ System checks passed"
echo ""

# =============================================================================
# WALLET SETUP
# =============================================================================

echo "💳 Wallet Setup"
echo "--------------"

# Generate new address if requested
if [[ "$GENERATE_ADDRESS" == true ]]; then
    echo "Generating new mining address..."
    
    # Check if wallet is available and unlocked
    if ! dinero-cli getblockcount >/dev/null 2>&1; then
        echo "❌ Cannot connect to Dinero daemon"
        echo "Please ensure dinerod is running and RPC is accessible"
        exit 1
    fi
    
    # Generate new address
    NEW_ADDRESS=$(dinero-cli getnewaddress 2>/dev/null || echo "")
    
    if [[ -n "$NEW_ADDRESS" ]] && [[ "$NEW_ADDRESS" =~ ^din1 ]]; then
        MINING_ADDRESS="$NEW_ADDRESS"
        echo "✅ Generated new mining address: $MINING_ADDRESS"
    else
        echo "❌ Failed to generate mining address"
        echo "Error: $NEW_ADDRESS"
        exit 1
    fi
fi

# Validate mining address
if [[ -z "$MINING_ADDRESS" ]]; then
    echo "❌ No mining address specified"
    echo "Use --address din1... or --generate-address"
    exit 1
fi

if [[ ! "$MINING_ADDRESS" =~ ^din1 ]]; then
    echo "❌ Invalid mining address format: $MINING_ADDRESS"
    echo "Address must start with 'din1'"
    exit 1
fi

echo "Mining address: $MINING_ADDRESS"
echo "✅ Wallet setup complete"
echo ""

# =============================================================================
# MINING CONFIGURATION
# =============================================================================

echo "⚙️  Mining Configuration"
echo "-----------------------"

# Create mining config directory
MINING_CONFIG_DIR="$HOME/.dinero/mining"
mkdir -p "$MINING_CONFIG_DIR"

# Create mining configuration file
MINING_CONFIG="$MINING_CONFIG_DIR/mining.conf"
cat > "$MINING_CONFIG" << EOF
# Dinero Mining Configuration
# Generated: $(date)

[mining]
enabled = true
address = $MINING_ADDRESS
threads = $MINING_THREADS
extra_nonce_start = $EXTRA_NONCE_START

[performance]
# CPU affinity (optional)
# cpu_affinity = 0,1,2,3

# Priority (optional)
# nice_level = -10

[logging]
log_level = info
log_shares = true
log_blocks = true
EOF

echo "Mining configuration:"
echo "  Address: $MINING_ADDRESS"
echo "  Threads: $MINING_THREADS"
echo "  Config: $MINING_CONFIG"
echo ""

# =============================================================================
# PERFORMANCE OPTIMIZATION
# =============================================================================

echo "🚀 Performance Optimization"
echo "---------------------------"

# CPU governor check (Linux only)
if [[ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
    CURRENT_GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
    echo "Current CPU governor: $CURRENT_GOVERNOR"
    
    if [[ "$CURRENT_GOVERNOR" != "performance" ]]; then
        echo "⚠️  Recommend setting CPU governor to 'performance' for mining:"
        echo "   sudo cpupower frequency-set -g performance"
    fi
fi

# Memory optimization
echo "Checking memory availability..."
AVAILABLE_MEM_GB=$(free -g 2>/dev/null | awk '/^Mem:/ {print $7}' || echo "unknown")
echo "Available memory: ${AVAILABLE_MEM_GB}GB"

if [[ "$AVAILABLE_MEM_GB" != "unknown" ]] && [[ $AVAILABLE_MEM_GB -lt 2 ]]; then
    echo "⚠️  Low available memory. Consider closing other applications."
fi

# Hugepages optimization (Linux only)
if [[ -f /proc/sys/vm/nr_hugepages ]]; then
    HUGEPAGES=$(cat /proc/sys/vm/nr_hugepages)
    echo "Hugepages configured: $HUGEPAGES"
    
    if [[ $HUGEPAGES -eq 0 ]]; then
        echo "💡 Tip: Enable hugepages for better mining performance:"
        echo "   sudo sysctl -w vm.nr_hugepages=128"
    fi
fi

echo "✅ Performance optimization complete"
echo ""

# =============================================================================
# MINING LAUNCH
# =============================================================================

echo "⛏️  Launching Mining"
echo "------------------"

# Check daemon status
echo "Checking daemon status..."
if ! dinero-cli getblockcount >/dev/null 2>&1; then
    echo "❌ Dinero daemon not responding"
    echo "Please start dinerod first"
    exit 1
fi

CURRENT_HEIGHT=$(dinero-cli getblockcount)
PEER_COUNT=$(dinero-cli getconnectioncount 2>/dev/null || echo "0")

echo "Network status:"
echo "  Block height: $CURRENT_HEIGHT"
echo "  Peer count: $PEER_COUNT"

if [[ $PEER_COUNT -eq 0 ]]; then
    echo "⚠️  Warning: No peers connected. Mining on isolated chain."
fi

# Get current difficulty
CURRENT_DIFFICULTY=$(dinero-cli getdifficulty 2>/dev/null || echo "unknown")
echo "  Difficulty: $CURRENT_DIFFICULTY"

# Estimate hash rate needed
if [[ "$CURRENT_DIFFICULTY" != "unknown" ]]; then
    # Very rough estimate - actual calculation depends on difficulty representation
    echo "  Est. network hashrate: ~$(echo "scale=2; $CURRENT_DIFFICULTY * 1000" | bc 2>/dev/null || echo "unknown") H/s"
fi

echo ""

# Create mining startup script
MINING_SCRIPT="$MINING_CONFIG_DIR/start_mining.sh"
cat > "$MINING_SCRIPT" << EOF
#!/bin/bash
# Dinero Mining Startup Script
# Generated: $(date)

set -euo pipefail

echo "🚀 Starting Dinero Mining"
echo "========================"
echo "Address: $MINING_ADDRESS"
echo "Threads: $MINING_THREADS"
echo "Started: \$(date)"
echo ""

# Set process priority (if running as root)
if [[ \$EUID -eq 0 ]]; then
    echo "Setting high priority for mining process..."
    renice -10 \$\$ >/dev/null 2>&1 || true
fi

# Launch mining
exec dinerod \\
    --datadir=\$HOME/.dinero \\
    --miningaddress=$MINING_ADDRESS \\
    --gen \\
    --genthreads=$MINING_THREADS \\
    --extranoncestart=$EXTRA_NONCE_START \\
    --printtoconsole \\
    --logthreadnames \\
    --logtimestamps \\
    --shrinkdebugfile
EOF

chmod +x "$MINING_SCRIPT"

# Launch mining in background with logging
MINING_LOG="$MINING_CONFIG_DIR/mining.log"

echo "🔥 Starting mining process..."
echo "Mining log: $MINING_LOG"
echo ""

# Start mining
nohup "$MINING_SCRIPT" > "$MINING_LOG" 2>&1 &
MINING_PID=$!

echo "✅ Mining started!"
echo "  PID: $MINING_PID"
echo "  Address: $MINING_ADDRESS"
echo "  Threads: $MINING_THREADS"
echo ""

# Wait a moment and check if process is still running
sleep 3

if kill -0 $MINING_PID 2>/dev/null; then
    echo "✅ Mining process is running"
else
    echo "❌ Mining process failed to start"
    echo "Check log: $MINING_LOG"
    exit 1
fi

# =============================================================================
# MONITORING SETUP
# =============================================================================

echo ""
echo "📊 Mining Monitoring"
echo "-------------------"

# Create monitoring script
MONITOR_SCRIPT="$MINING_CONFIG_DIR/monitor_mining.sh"
cat > "$MONITOR_SCRIPT" << EOF
#!/bin/bash
# Dinero Mining Monitor
# Check mining status and performance

echo "⛏️  DINERO MINING STATUS"
echo "======================"
echo "Timestamp: \$(date)"
echo ""

# Check if mining process is running
if pgrep -f "miningaddress=$MINING_ADDRESS" >/dev/null; then
    echo "✅ Mining process: RUNNING"
    MINING_PID=\$(pgrep -f "miningaddress=$MINING_ADDRESS")
    echo "   PID: \$MINING_PID"
    
    # Get CPU usage
    CPU_USAGE=\$(ps -p \$MINING_PID -o %cpu --no-headers 2>/dev/null || echo "unknown")
    echo "   CPU usage: \${CPU_USAGE}%"
    
    # Get memory usage  
    MEM_USAGE=\$(ps -p \$MINING_PID -o %mem --no-headers 2>/dev/null || echo "unknown")
    echo "   Memory usage: \${MEM_USAGE}%"
else
    echo "❌ Mining process: NOT RUNNING"
fi

echo ""

# Check daemon status
if dinero-cli getblockcount >/dev/null 2>&1; then
    HEIGHT=\$(dinero-cli getblockcount)
    PEERS=\$(dinero-cli getconnectioncount)
    DIFFICULTY=\$(dinero-cli getdifficulty)
    
    echo "📡 Network Status:"
    echo "   Block height: \$HEIGHT"
    echo "   Peers: \$PEERS"
    echo "   Difficulty: \$DIFFICULTY"
else
    echo "❌ Daemon: NOT RESPONDING"
fi

echo ""

# Check recent mining activity from log
if [[ -f "$MINING_LOG" ]]; then
    echo "📈 Recent Mining Activity:"
    echo "   Log size: \$(du -h $MINING_LOG | cut -f1)"
    echo ""
    echo "Last 5 log entries:"
    tail -5 "$MINING_LOG" | sed 's/^/   /'
else
    echo "❌ Mining log not found"
fi

echo ""
echo "💰 Wallet Status:"
BALANCE=\$(dinero-cli getbalance 2>/dev/null || echo "unknown")
echo "   Balance: \$BALANCE DIN"

UNCONFIRMED=\$(dinero-cli getunconfirmedbalance 2>/dev/null || echo "unknown")
echo "   Unconfirmed: \$UNCONFIRMED DIN"

echo ""
echo "🎯 Mining Address: $MINING_ADDRESS"
echo "📁 Config Dir: $MINING_CONFIG_DIR"
echo "📄 Mining Log: $MINING_LOG"
EOF

chmod +x "$MONITOR_SCRIPT"

echo "Monitor script: $MONITOR_SCRIPT"
echo ""

# Show initial status
"$MONITOR_SCRIPT"

# =============================================================================
# COMPLETION
# =============================================================================

echo ""
echo "🎉 MINING LAUNCH COMPLETE!"
echo "========================="
echo ""
echo "📋 Mining Summary:"
echo "  Status: RUNNING"
echo "  PID: $MINING_PID"
echo "  Address: $MINING_ADDRESS"
echo "  Threads: $MINING_THREADS"
echo "  Log: $MINING_LOG"
echo ""
echo "🛠️  Management Commands:"
echo "  Monitor: $MONITOR_SCRIPT"
echo "  Stop mining: kill $MINING_PID"
echo "  View log: tail -f $MINING_LOG"
echo "  Restart: $MINING_SCRIPT"
echo ""
echo "📊 Useful RPC Commands:"
echo "  dinero-cli getblockcount"
echo "  dinero-cli getbalance"
echo "  dinero-cli getmininginfo"
echo "  dinero-cli getnewaddress"
echo ""
echo "💡 Tips:"
echo "  - Monitor CPU temperature during mining"
echo "  - Check log regularly for errors or blocks found"
echo "  - Blocks need 100 confirmations to mature"
echo "  - Consider pool mining for steady rewards (future feature)"
echo ""
echo "⛏️  Happy mining! May you find many blocks! 💎"
echo ""

# Keep script running to show live updates
if [[ -t 1 ]]; then
    echo "Press Ctrl+C to exit monitoring..."
    echo ""
    
    while true; do
        sleep 30
        echo "$(date): Mining status check..."
        
        if ! kill -0 $MINING_PID 2>/dev/null; then
            echo "❌ Mining process stopped unexpectedly!"
            break
        fi
        
        # Show brief status
        HEIGHT=$(dinero-cli getblockcount 2>/dev/null || echo "unknown")
        BALANCE=$(dinero-cli getbalance 2>/dev/null || echo "unknown")
        echo "   Height: $HEIGHT | Balance: $BALANCE DIN"
    done
fi
