#!/bin/bash
# DineroCoin Node Diagnostic Script
# Run this on your Dell tower to identify issues

set -e

echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║         DineroCoin Node Diagnostic Tool v1.0                 ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

function check_ok() {
    echo -e "${GREEN}✓${NC} $1"
}

function check_warn() {
    echo -e "${YELLOW}⚠${NC} $1"
}

function check_fail() {
    echo -e "${RED}✗${NC} $1"
}

# ==============================================================================
# 1. SYSTEM INFORMATION
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1. System Information"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "OS: $(uname -s) $(uname -r)"
echo "Architecture: $(uname -m)"

if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "Distribution: $NAME $VERSION"
fi

echo "Hostname: $(hostname)"
echo "Uptime: $(uptime -p 2>/dev/null || uptime)"
echo ""

# CPU
echo "CPU:"
grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | xargs echo "  Model:"
echo "  Cores: $(nproc)"
echo "  Load: $(uptime | awk -F'load average:' '{print $2}')"
echo ""

# Memory
echo "Memory:"
free -h | grep "Mem:" | awk '{print "  Total: "$2" | Used: "$3" | Free: "$4" | Available: "$7}'
echo ""

# Disk
echo "Disk Space:"
df -h / | tail -1 | awk '{print "  Root: "$2" total, "$3" used, "$4" available ("$5" used)"}'
if [ -d "$HOME/.dinero" ]; then
    DINERO_SIZE=$(du -sh "$HOME/.dinero" 2>/dev/null | cut -f1)
    echo "  ~/.dinero size: $DINERO_SIZE"
fi
echo ""

# ==============================================================================
# 2. BUILD CHECK
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "2. Build Check"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check for binaries
if [ -f "./build/dinerod" ]; then
    check_ok "dinerod binary exists"
    echo "   Path: $(pwd)/build/dinerod"
    echo "   Size: $(ls -lh build/dinerod | awk '{print $5}')"
    echo "   Date: $(ls -l build/dinerod | awk '{print $6" "$7" "$8}')"
else
    check_fail "dinerod binary NOT found at ./build/dinerod"
    echo "   Run: cmake -B build && cmake --build build -j$(nproc)"
fi

if [ -f "./build/dinero-cli" ]; then
    check_ok "dinero-cli binary exists"
else
    check_fail "dinero-cli binary NOT found"
fi

if [ -f "./build/dinero-miner" ]; then
    check_ok "dinero-miner binary exists"
else
    check_warn "dinero-miner binary NOT found (optional)"
fi

echo ""

# ==============================================================================
# 3. DEPENDENCIES CHECK
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3. Dependencies Check"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check for required libraries
if command -v ldd >/dev/null 2>&1 && [ -f "./build/dinerod" ]; then
    echo "Checking shared library dependencies..."
    MISSING_LIBS=$(ldd ./build/dinerod 2>/dev/null | grep "not found" || true)
    if [ -z "$MISSING_LIBS" ]; then
        check_ok "All shared libraries found"
    else
        check_fail "Missing libraries:"
        echo "$MISSING_LIBS"
    fi
else
    check_warn "Cannot check dependencies (ldd not available or binary missing)"
fi

echo ""

# ==============================================================================
# 4. NETWORK CHECK
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "4. Network Check"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check internet connectivity
if ping -c 1 8.8.8.8 >/dev/null 2>&1; then
    check_ok "Internet connectivity (ping 8.8.8.8)"
else
    check_fail "No internet connectivity"
fi

# Check DNS
if ping -c 1 google.com >/dev/null 2>&1; then
    check_ok "DNS resolution working"
else
    check_fail "DNS resolution not working"
fi

# Check default ports
DEFAULT_RPC_PORT=20998
DEFAULT_P2P_PORT=20999

echo ""
echo "Checking if ports are available:"
if command -v netstat >/dev/null 2>&1; then
    if netstat -ln | grep ":$DEFAULT_RPC_PORT " >/dev/null; then
        check_warn "Port $DEFAULT_RPC_PORT already in use (RPC port)"
        netstat -lnp 2>/dev/null | grep ":$DEFAULT_RPC_PORT " || true
    else
        check_ok "Port $DEFAULT_RPC_PORT available (RPC)"
    fi

    if netstat -ln | grep ":$DEFAULT_P2P_PORT " >/dev/null; then
        check_warn "Port $DEFAULT_P2P_PORT already in use (P2P port)"
        netstat -lnp 2>/dev/null | grep ":$DEFAULT_P2P_PORT " || true
    else
        check_ok "Port $DEFAULT_P2P_PORT available (P2P)"
    fi
else
    check_warn "netstat not available, cannot check ports"
fi

# Check firewall status
echo ""
echo "Firewall status:"
if command -v ufw >/dev/null 2>&1; then
    UFW_STATUS=$(sudo ufw status 2>/dev/null | head -1)
    echo "  UFW: $UFW_STATUS"
    if echo "$UFW_STATUS" | grep -q "active"; then
        check_warn "UFW firewall is active - may need to allow P2P port"
        echo "  To allow P2P: sudo ufw allow $DEFAULT_P2P_PORT/tcp"
    fi
elif command -v firewall-cmd >/dev/null 2>&1; then
    if sudo firewall-cmd --state 2>/dev/null | grep -q "running"; then
        check_warn "firewalld is running - may need to allow P2P port"
        echo "  To allow P2P: sudo firewall-cmd --permanent --add-port=$DEFAULT_P2P_PORT/tcp"
    fi
else
    echo "  No common firewall detected (ufw/firewalld)"
fi

echo ""

# ==============================================================================
# 5. DAEMON STATUS CHECK
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "5. Daemon Status Check"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Check if dinerod is running
if pgrep -x dinerod >/dev/null; then
    check_ok "dinerod is running"
    ps aux | grep "[d]inerod" | head -5

    echo ""
    echo "Process info:"
    DINEROD_PID=$(pgrep -x dinerod | head -1)
    echo "  PID: $DINEROD_PID"
    echo "  Memory: $(ps -p $DINEROD_PID -o rss= | awk '{print $1/1024 " MB"}')"
    echo "  CPU: $(ps -p $DINEROD_PID -o %cpu= | xargs)%"
    echo "  Runtime: $(ps -p $DINEROD_PID -o etime= | xargs)"
else
    check_warn "dinerod is NOT running"
    echo "  To start: ./build/dinerod -daemon"
fi

echo ""

# ==============================================================================
# 6. DATA DIRECTORY CHECK
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "6. Data Directory Check"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

DATADIR="$HOME/.dinero"

if [ -d "$DATADIR" ]; then
    check_ok "Data directory exists: $DATADIR"

    # Check important files
    if [ -f "$DATADIR/.cookie" ]; then
        check_ok "RPC cookie exists"
    else
        check_warn "RPC cookie NOT found (daemon may not be running)"
    fi

    if [ -f "$DATADIR/debug.log" ]; then
        check_ok "Debug log exists"
        LOG_SIZE=$(ls -lh "$DATADIR/debug.log" | awk '{print $5}')
        echo "  Size: $LOG_SIZE"
        echo "  Last modified: $(ls -l "$DATADIR/debug.log" | awk '{print $6" "$7" "$8}')"
    else
        check_warn "Debug log NOT found"
    fi

    if [ -f "$DATADIR/dinero.conf" ]; then
        check_ok "Configuration file exists"
        echo "  Config:"
        grep -v "^#" "$DATADIR/dinero.conf" 2>/dev/null | grep -v "^$" | sed 's/^/    /'
    else
        check_warn "No dinero.conf found (using defaults)"
    fi

    # Check blockchain data
    if [ -d "$DATADIR/blocks" ]; then
        BLOCKS_SIZE=$(du -sh "$DATADIR/blocks" 2>/dev/null | cut -f1)
        echo "  Blocks directory: $BLOCKS_SIZE"
    fi

    if [ -d "$DATADIR/chainstate" ]; then
        CHAINSTATE_SIZE=$(du -sh "$DATADIR/chainstate" 2>/dev/null | cut -f1)
        echo "  Chainstate directory: $CHAINSTATE_SIZE"
    fi
else
    check_warn "Data directory does NOT exist: $DATADIR"
    echo "  Will be created on first run"
fi

echo ""

# ==============================================================================
# 7. RPC CONNECTION TEST
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "7. RPC Connection Test"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ -f "./build/dinero-cli" ] && pgrep -x dinerod >/dev/null; then
    echo "Testing RPC connection..."

    # Test getblockchaininfo
    if RPC_RESULT=$(./build/dinero-cli getblockchaininfo 2>&1); then
        check_ok "RPC connection successful"
        echo ""
        echo "Blockchain Info:"
        echo "$RPC_RESULT" | python3 -m json.tool 2>/dev/null | grep -E '"chain"|"blocks"|"headers"|"bestblockhash"' | sed 's/^/  /'
    else
        check_fail "RPC connection failed"
        echo "  Error: $RPC_RESULT"
    fi

    echo ""

    # Test peer info
    if PEER_RESULT=$(./build/dinero-cli getconnectioncount 2>&1); then
        PEER_COUNT=$(echo "$PEER_RESULT" | grep -oE '[0-9]+' | head -1)
        if [ "$PEER_COUNT" -gt 0 ]; then
            check_ok "Connected to $PEER_COUNT peer(s)"
        else
            check_warn "No peer connections (isolated node)"
        fi
    fi
else
    check_warn "Cannot test RPC (daemon not running or dinero-cli missing)"
fi

echo ""

# ==============================================================================
# 8. LOG ANALYSIS
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "8. Recent Log Analysis"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ -f "$DATADIR/debug.log" ]; then
    echo "Last 20 lines of debug.log:"
    echo "----------------------------------------"
    tail -20 "$DATADIR/debug.log"
    echo "----------------------------------------"
    echo ""

    # Check for errors
    ERROR_COUNT=$(grep -i "error" "$DATADIR/debug.log" 2>/dev/null | wc -l)
    if [ "$ERROR_COUNT" -gt 0 ]; then
        check_warn "Found $ERROR_COUNT error(s) in log"
        echo "  Recent errors:"
        grep -i "error" "$DATADIR/debug.log" | tail -5 | sed 's/^/    /'
    else
        check_ok "No errors in log"
    fi

    # Check for warnings
    WARN_COUNT=$(grep -i "warning" "$DATADIR/debug.log" 2>/dev/null | wc -l)
    if [ "$WARN_COUNT" -gt 0 ]; then
        check_warn "Found $WARN_COUNT warning(s) in log"
    fi
else
    check_warn "No debug.log found"
fi

echo ""

# ==============================================================================
# 9. RESOURCE USAGE
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "9. Resource Usage"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if pgrep -x dinerod >/dev/null; then
    DINEROD_PID=$(pgrep -x dinerod | head -1)

    echo "dinerod resource usage:"
    ps -p $DINEROD_PID -o pid,ppid,%cpu,%mem,vsz,rss,tty,stat,start,time,cmd | head -2

    echo ""
    echo "File descriptors:"
    if [ -d "/proc/$DINEROD_PID/fd" ]; then
        FD_COUNT=$(ls /proc/$DINEROD_PID/fd 2>/dev/null | wc -l)
        FD_LIMIT=$(ulimit -n)
        echo "  Open: $FD_COUNT / $FD_LIMIT"
        if [ "$FD_COUNT" -gt $((FD_LIMIT * 80 / 100)) ]; then
            check_warn "File descriptor usage is high (${FD_COUNT}/${FD_LIMIT})"
        else
            check_ok "File descriptor usage OK (${FD_COUNT}/${FD_LIMIT})"
        fi
    fi
else
    echo "dinerod not running, cannot check resource usage"
fi

echo ""

# ==============================================================================
# 10. SUMMARY & RECOMMENDATIONS
# ==============================================================================
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "10. Summary & Recommendations"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo "Common Issues & Solutions:"
echo ""
echo "1. If daemon won't start:"
echo "   - Check logs: tail -f ~/.dinero/debug.log"
echo "   - Check permissions: ls -la ~/.dinero"
echo "   - Try: ./build/dinerod -daemon -debug=net -debug=rpc"
echo ""
echo "2. If no peer connections:"
echo "   - Check firewall: sudo ufw status"
echo "   - Check ports: netstat -ln | grep 20999"
echo "   - Manually add peer: ./build/dinero-cli addnode <IP>:20999 add"
echo ""
echo "3. If RPC doesn't work:"
echo "   - Check cookie: cat ~/.dinero/.cookie"
echo "   - Check config: cat ~/.dinero/dinero.conf"
echo "   - Try explicit auth: dinero-cli -rpcuser=user -rpcpassword=pass"
echo ""
echo "4. If syncing is slow:"
echo "   - Check disk I/O: iostat -x 1"
echo "   - Check network: iftop or nethogs"
echo "   - Increase dbcache: add 'dbcache=4096' to dinero.conf"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Diagnostic complete!"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "To share results with support, run:"
echo "  ./tools/diagnose_node.sh > diagnostic_report.txt 2>&1"
echo ""
