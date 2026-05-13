#!/bin/bash
# Quick guide to connect GUI to testnet servers

echo "════════════════════════════════════════════════════════════"
echo "  🎯 DINERO GUI - TESTNET CONNECTION SETUP GUIDE"
echo "════════════════════════════════════════════════════════════"
echo ""

echo "📋 YOUR TESTNET SERVERS:"
echo "   • Server 1: 173.249.195.59:20998"
echo "   • Server 2: 96.9.226.98:20998"
echo ""

echo "✅ CHANGES MADE TO GUI:"
echo "   1. ✓ Remote servers enabled in rpcclient.cpp"
echo "   2. ✓ Cookie loading paths updated"
echo "   3. ✓ Automatic server failover configured"
echo ""

echo "📌 WHAT YOU NEED TO DO NOW:"
echo ""
echo "OPTION 1 - Automatic (SSH access required):"
echo "   Run: ./fetch-testnet-cookies.sh"
echo ""
echo "OPTION 2 - Manual:"
echo "   1. SSH to each server"
echo "   2. Get the cookie: cat ~/.dinero-testnet/.cookie"
echo "   3. Save to local files:"
echo "      • Server 1: ~/.dinero/testnet-cookies/server1.cookie"
echo "      • Server 2: ~/.dinero/testnet-cookies/server2.cookie"
echo ""

echo "🚀 THEN LAUNCH THE GUI:"
echo "   cd /Users/haydarevich/Documents/DineroCoin"
echo "   open launch-gui.command"
echo ""

echo "🔍 HOW TO VERIFY CONNECTION:"
echo "   • Look for green connection status"
echo "   • Status should show: '✅ Connected: http://...'"
echo "   • If localhost fails, GUI auto-switches to Server 1"
echo "   • If Server 1 fails, GUI auto-switches to Server 2"
echo ""

echo "💡 TROUBLESHOOTING:"
echo "   • Red 'Unauthorized' = missing/invalid cookie"
echo "   • Connection timeout = server down or firewall"
echo "   • Check cookie format: username:password"
echo ""

echo "════════════════════════════════════════════════════════════"
