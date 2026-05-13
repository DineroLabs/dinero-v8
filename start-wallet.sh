#!/bin/bash
# Dinero Wallet - One-Click Launcher
# Just run this script to start the wallet

cd "$(dirname "$0")"

echo "🚀 Starting Dinero Wallet..."
echo ""

# Kill any stale processes
killall -9 dinerod dinero-qt 2>/dev/null
rm -f ~/.dinero/.lock 2>/dev/null

# Launch GUI (it will auto-start daemon with seed nodes)
./build/gui/dinero-qt &

echo "✅ Wallet starting..."
echo ""
echo "The wallet will:"
echo "  • Auto-start the daemon with 4 seed nodes"
echo "  • Connect to the network automatically"
echo "  • Show 2-4 peer connections when ready"
echo ""
echo "If you see 'daemon offline', wait 10-15 seconds for initialization."
