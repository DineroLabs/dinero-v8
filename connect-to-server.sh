#!/usr/bin/env bash
# Connect GUI to remote Ubuntu server node

set -euo pipefail

SERVER="96.9.226.98"
REMOTE_DATADIR="/root/DineroCoin/data"
LOCAL_PORT="20998"
REMOTE_PORT="20998"

echo "🌐 Connecting to Ubuntu server at $SERVER..."
echo ""
echo "Step 1: Setting up SSH tunnel..."
echo "  Local:  127.0.0.1:$LOCAL_PORT"
echo "  Remote: $SERVER:$REMOTE_PORT"
echo ""

# Kill any existing tunnel
pkill -f "ssh.*$SERVER.*$REMOTE_PORT" 2>/dev/null || true
sleep 1

# Create SSH tunnel (run in background)
ssh -N -L $LOCAL_PORT:127.0.0.1:$REMOTE_PORT root@$SERVER &
SSH_PID=$!
echo "✅ SSH tunnel established (PID: $SSH_PID)"
echo ""

# Wait for tunnel
sleep 2

echo "Step 2: Copying cookie from server..."
# Copy the cookie file from server
scp root@$SERVER:$REMOTE_DATADIR/.cookie ./server-cookie.txt
echo "✅ Cookie downloaded"
echo ""

echo "Step 3: Launching GUI..."
# Launch GUI - it will use the SSH tunnel
./build-gui/dinero-qt &
GUI_PID=$!
echo "✅ GUI launched (PID: $GUI_PID)"
echo ""

echo "═══════════════════════════════════════════════════════"
echo "🎉 CONNECTED TO SERVER!"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "The GUI is now talking to your Ubuntu server node."
echo "You should see the actual block height from the server."
echo ""
echo "To disconnect:"
echo "  kill $SSH_PID  # Stop SSH tunnel"
echo "  kill $GUI_PID  # Stop GUI"
echo ""
echo "Or just close the GUI window and run:"
echo "  pkill -f 'ssh.*$SERVER'"
echo ""

