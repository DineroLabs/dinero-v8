#!/bin/bash
# Dinero Network Monitor - Real-time status of both servers

SERVER1="96.9.226.98"
SERVER2="173.249.195.59"
KEY1="$HOME/Documents/DineroCoin/.server-key"
KEY2="$HOME/Documents/DineroCoin/.server2-key"

clear
echo "═══════════════════════════════════════════════════════"
echo "🌐 DINERO NETWORK MONITOR"
echo "═══════════════════════════════════════════════════════"
echo ""

# Check Server 1
echo "📡 Server 1 (Primary) - $SERVER1"
if ssh -i "$KEY1" -o ConnectTimeout=3 root@$SERVER1 "exit" 2>/dev/null; then
    STATUS=$(ssh -i "$KEY1" root@$SERVER1 "systemctl is-active dinerod 2>/dev/null")
    PEERS=$(ssh -i "$KEY1" root@$SERVER1 "journalctl -u dinerod --no-pager -n 200 | grep -c 'Peer connected'")
    HEIGHT=$(ssh -i "$KEY1" root@$SERVER1 "journalctl -u dinerod --no-pager -n 100 | grep 'Blockchain initialized at height' | tail -1 | grep -oE 'height [0-9]+' | awk '{print \$2}'")
    MEMORY=$(ssh -i "$KEY1" root@$SERVER1 "ps aux | grep 'dinerod' | grep -v grep | awk '{print \$6}'")
    UPTIME=$(ssh -i "$KEY1" root@$SERVER1 "ps -p \$(pgrep dinerod) -o etime= 2>/dev/null")
    
    if [ "$STATUS" = "active" ]; then
        echo "  Status:  ✅ ACTIVE"
    else
        echo "  Status:  ❌ DOWN"
    fi
    echo "  Height:  ${HEIGHT:-0}"
    echo "  Peers:   $PEERS connections"
    echo "  Memory:  $((${MEMORY:-0}/1024)) MB"
    echo "  Uptime:  ${UPTIME:-N/A}"
else
    echo "  Status:  ❌ UNREACHABLE"
fi

echo ""

# Check Server 2
echo "📡 Server 2 (Secondary) - $SERVER2"
if ssh -i "$KEY2" -o ConnectTimeout=3 root@$SERVER2 "exit" 2>/dev/null; then
    STATUS=$(ssh -i "$KEY2" root@$SERVER2 "systemctl is-active dinerod 2>/dev/null")
    PEERS=$(ssh -i "$KEY2" root@$SERVER2 "journalctl -u dinerod --no-pager -n 200 | grep -c 'Peer connected'")
    HEIGHT=$(ssh -i "$KEY2" root@$SERVER2 "journalctl -u dinerod --no-pager -n 100 | grep 'Blockchain initialized at height' | tail -1 | grep -oE 'height [0-9]+' | awk '{print \$2}'")
    MEMORY=$(ssh -i "$KEY2" root@$SERVER2 "ps aux | grep 'dinerod' | grep -v grep | awk '{print \$6}'")
    UPTIME=$(ssh -i "$KEY2" root@$SERVER2 "ps -p \$(pgrep dinerod) -o etime= 2>/dev/null")
    
    if [ "$STATUS" = "active" ]; then
        echo "  Status:  ✅ ACTIVE"
    else
        echo "  Status:  ❌ DOWN"
    fi
    echo "  Height:  ${HEIGHT:-0}"
    echo "  Peers:   $PEERS connections"
    echo "  Memory:  $((${MEMORY:-0}/1024)) MB"
    echo "  Uptime:  ${UPTIME:-N/A}"
else
    echo "  Status:  ❌ UNREACHABLE"
fi

echo ""
echo "═══════════════════════════════════════════════════════"
echo "🔄 Auto-refresh: ./monitor-network.sh"
echo "📊 Live watch: watch -n 5 ./monitor-network.sh"
echo "═══════════════════════════════════════════════════════"
echo "Last Updated: $(date '+%Y-%m-%d %H:%M:%S')"

