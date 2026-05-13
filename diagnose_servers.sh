#!/usr/bin/env bash
# Server Connectivity Troubleshooting (bash 3.x compatible)

echo "🔍 DineroCoin Server Diagnostics"
echo "================================="
echo ""

# Server list: name|ip|key
SERVERS=(
    "DineroCA|206.188.199.122|~/.ssh/dinero_key"
    "DineroVA|206.188.199.123|~/.ssh/dinero_va.key"
    "DineroLA|206.188.199.131|~/.ssh/dinerola.key"
    "OldServer|96.9.226.98|~/.ssh/id_ed25519"
)

echo "📋 Testing connectivity to all servers..."
echo ""

WORKING=0
WORKING_SERVERS=""

for server_info in "${SERVERS[@]}"; do
    name=$(echo "$server_info" | cut -d'|' -f1)
    ip=$(echo "$server_info" | cut -d'|' -f2)
    key=$(echo "$server_info" | cut -d'|' -f3)
    
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "🖥️  $name ($ip)"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    
    # Test 1: Ping
    echo -n "  [1/5] Ping test... "
    if ping -c 1 -W 2 "$ip" > /dev/null 2>&1; then
        echo "✅ Reachable"
    else
        echo "❌ Unreachable"
    fi
    
    # Test 2: Port 22
    echo -n "  [2/5] SSH port (22)... "
    if nc -z -w 2 "$ip" 22 2>/dev/null; then
        echo "✅ Open"
    else
        echo "❌ Closed"
    fi
    
    # Test 3: SSH key
    echo -n "  [3/5] SSH key ($key)... "
    if [ -f "$key" ]; then
        echo "✅ Found"
    else
        echo "❌ Not found"
        echo ""
        continue
    fi
    
    # Test 4: SSH connection
    echo -n "  [4/5] SSH authentication... "
    if timeout 5 ssh -i "$key" -o ConnectTimeout=3 -o StrictHostKeyChecking=no -o BatchMode=yes "root@$ip" "echo 'OK'" > /dev/null 2>&1; then
        echo "✅ Success"
        
        # Test 5: Daemon status
        echo -n "  [5/5] Daemon status... "
        if timeout 5 ssh -i "$key" -o ConnectTimeout=3 -o StrictHostKeyChecking=no "root@$ip" "pgrep -f dinerod" > /dev/null 2>&1; then
            PID=$(timeout 5 ssh -i "$key" -o ConnectTimeout=3 -o StrictHostKeyChecking=no "root@$ip" "pgrep -f dinerod" 2>/dev/null)
            echo "✅ Running (PID: $PID)"
        else
            echo "⚪ Not running"
        fi
        
        WORKING=$((WORKING + 1))
        WORKING_SERVERS="$WORKING_SERVERS\n  ✅ $name ($ip)"
    else
        echo "❌ Failed"
        echo "  [5/5] Daemon status... ⚪ Cannot check"
    fi
    
    echo ""
done

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📊 Summary"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if [ $WORKING -eq 0 ]; then
    echo "⚠️  NO SERVERS ACCESSIBLE"
    echo ""
    echo "Troubleshooting:"
    echo "  1. Check hosting provider dashboard"
    echo "  2. Verify servers are powered on"
    echo "  3. Check firewall settings"
    echo "  4. Confirm IP addresses"
else
    echo -e "$WORKING_SERVERS"
    echo ""
    echo "✅ $WORKING server(s) accessible!"
    echo ""
    if [ $WORKING -ge 3 ]; then
        echo "🚀 Ready to deploy! Run:"
        echo "   ./deploy_now.sh"
    else
        echo "⚠️  Some servers unreachable"
        echo "   Can deploy to working servers only"
    fi
fi
