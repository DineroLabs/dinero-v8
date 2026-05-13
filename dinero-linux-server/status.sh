#!/bin/bash
# Dinero Node Status Script - Linux Server

echo "📊 Dinero Node Status - Server 96.9.226.98"
echo "============================================"

# Check systemd service status
if command -v systemctl &> /dev/null; then
    echo "🔄 Service Status:"
    sudo systemctl is-active dinerod --quiet && echo "✅ Service: Running" || echo "❌ Service: Stopped"
    echo ""
fi

# Check if daemon process is running
if pgrep -f "dinerod" > /dev/null; then
    echo "✅ Daemon: Running"
    
    # Get blockchain info
    echo ""
    echo "🔗 Blockchain Information:"
    ./dinero-cli -datadir="./data" getblockchaininfo 2>/dev/null | grep -E "(chain|blocks|bestblockhash|difficulty)" || echo "❌ RPC not responding"
    
    # Get network info
    echo ""
    echo "🌐 Network Information:"
    ./dinero-cli -datadir="./data" getnetworkinfo 2>/dev/null | grep -E "(connections|localaddresses)" || echo "❌ Network RPC not responding"
    
    # Get mining info
    echo ""
    echo "⛏️  Mining Information:"
    ./dinero-cli -datadir="./data" getmininginfo 2>/dev/null | grep -E "(mining|hashrate|difficulty)" || echo "❌ Mining RPC not responding"
    
    # System resources
    echo ""
    echo "💻 System Resources:"
    echo "  Memory: $(free -h | awk '/^Mem:/ {print $3 "/" $2}')"
    echo "  Disk:   $(df -h . | awk 'NR==2 {print $3 "/" $2 " (" $5 " used)"}')"
    echo "  Load:   $(uptime | awk -F'load average:' '{print $2}')"
    
else
    echo "❌ Daemon: Not running"
fi

echo ""
echo "🌐 Network Endpoints:"
echo "  P2P:       96.9.226.98:23999"
echo "  RPC:       96.9.226.98:20998"
echo "  WebSocket: 96.9.226.98:22999"
echo ""
echo "📋 Management Commands:"
echo "  Start:     sudo systemctl start dinerod"
echo "  Stop:      sudo systemctl stop dinerod"
echo "  Restart:   sudo systemctl restart dinerod"
echo "  Logs:      sudo journalctl -u dinerod -f"
echo "  RPC Test:  ./dinero-cli -datadir=./data getblockchaininfo"
