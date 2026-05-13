#!/usr/bin/env bash
# Check current status of Ubuntu server
# Paste this into SSH session on your server

echo "🔍 Checking Dinero Daemon Status on Ubuntu Server"
echo "=================================================="
echo ""

echo "1️⃣ Systemd Service Status:"
systemctl status dinerod 2>/dev/null || echo "   ❌ No systemd service found"
echo ""

echo "2️⃣ Running Processes:"
ps aux | grep -E 'dinerod|dinero' | grep -v grep || echo "   ❌ No dinerod processes running"
echo ""

echo "3️⃣ Listening Ports:"
netstat -tuln | grep -E '20998|20999' || ss -tuln | grep -E '20998|20999' || echo "   ❌ No dinero ports listening"
echo ""

echo "4️⃣ Data Directory:"
if [ -d "/var/lib/dinero" ]; then
    echo "   ✅ /var/lib/dinero exists (systemd setup)"
    ls -la /var/lib/dinero/ | head -10
elif [ -d "/root/DineroCoin/data" ]; then
    echo "   ⚠️  /root/DineroCoin/data exists (manual setup)"
    ls -la /root/DineroCoin/data/ | head -10
else
    echo "   ❌ No data directory found"
fi
echo ""

echo "5️⃣ Blockchain Status:"
if [ -f "/usr/local/bin/dinero-cli" ]; then
    /usr/local/bin/dinero-cli -datadir=/var/lib/dinero getblockcount 2>/dev/null || echo "   ⚠️  Can't connect to daemon (systemd)"
elif [ -f "/root/DineroCoin/build-clean/dinerod-cli" ]; then
    /root/DineroCoin/build-clean/dinerod-cli -datadir=/root/DineroCoin/data getblockcount 2>/dev/null || echo "   ⚠️  Can't connect to daemon (manual)"
else
    echo "   ❌ dinero-cli not found"
fi
echo ""

echo "6️⃣ Firewall Rules:"
ufw status 2>/dev/null | grep -E '20998|20999' || echo "   ⚠️  UFW not configured or not installed"
echo ""

echo "=================================================="
echo "✅ = Production setup (systemd)"
echo "⚠️  = Manual setup (needs upgrade)"
echo "❌ = Not configured"

