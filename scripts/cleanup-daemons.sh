#!/bin/bash
# Dinero Daemon Cleanup Script
# Kills all running daemons and removes lock files

echo "════════════════════════════════════════════════"
echo "Dinero Daemon Cleanup"
echo "════════════════════════════════════════════════"
echo ""

# Kill all dinerod processes
echo "1. Stopping all dinerod processes..."
killall -9 dinerod 2>/dev/null
pkill -9 dinerod 2>/dev/null
sleep 1

# Kill GUI if running
echo "2. Stopping GUI..."
killall -9 dinero-qt 2>/dev/null
pkill -9 dinero-qt 2>/dev/null
sleep 1

# Remove lock files from all possible locations
echo "3. Removing lock files..."
rm -f ~/.dinero/.lock
rm -f ~/Library/Application\ Support/Dinero/Dinero/.lock
rm -f /var/lib/dinero/.lock

# Verify
echo ""
echo "════════════════════════════════════════════════"
if pgrep -q dinerod || pgrep -q dinero-qt; then
    echo "⚠️  Warning: Some processes may still be running"
    pgrep -fl "dinerod|dinero-qt"
else
    echo "✅ All Dinero processes stopped"
fi

echo ""
echo "Lock files cleaned:"
echo "  ✓ ~/.dinero/.lock"
echo "  ✓ GUI datadir/.lock"
echo ""
echo "You can now restart the daemon or GUI"
echo "════════════════════════════════════════════════"
