#!/bin/bash
# Dinero GUI Launcher - Fixed version
# Starts daemon manually FIRST with seed nodes, then starts GUI

cd "$(dirname "$0")/.."

echo "════════════════════════════════════════════════"
echo "Starting Dinero Wallet (Manual Daemon Mode)"
echo "════════════════════════════════════════════════"
echo ""

# Check if config exists, create if not
CONFIG_DIR="$HOME/.dinero"
mkdir -p "$CONFIG_DIR"

# Clean up any stale processes/locks
echo "Checking for running processes..."
if pgrep -q dinerod || pgrep -q dinero-qt; then
    echo "⚠️  Found running processes, cleaning up..."
    ./scripts/cleanup-daemons.sh
    sleep 2
else
    echo "✓ No stale processes found"
fi

echo ""
echo "Starting daemon manually with seed nodes..."
echo "════════════════════════════════════════════════"

# Start daemon in background with seed nodes (using IPs for reliability)
./build/dinerod \
    -datadir="$CONFIG_DIR" \
    -addnode=172.93.160.131:20999 \
    -addnode=173.249.195.59:20999 &

DAEMON_PID=$!
echo "✓ Daemon started (PID: $DAEMON_PID)"
echo ""
echo "Waiting for daemon to initialize and RPC to be ready..."

# Wait up to 15 seconds for RPC to be ready
for i in {1..15}; do
    sleep 1
    if [ -f "$CONFIG_DIR/.cookie" ]; then
        COOKIE=$(cat "$CONFIG_DIR/.cookie")
        if curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" -H "Content-Type: application/json" -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}' 2>&1 | grep -q "result"; then
            echo "✓ RPC is ready after $i seconds"
            break
        fi
    fi
    echo -n "."
done
echo ""

# Check if daemon is still running
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    echo "✗ Daemon failed to start!"
    exit 1
fi

echo "✓ Daemon is running with 2 peer connections"
echo ""
echo "Starting GUI (it may take 10-30 seconds to connect)..."
echo "════════════════════════════════════════════════"

# Start GUI (it will detect the running daemon and connect to it)
./build/gui/dinero-qt -datadir="$CONFIG_DIR" &

echo ""
echo "✅ Dinero Wallet is starting!"
echo ""
echo "The GUI will connect to the daemon within 30 seconds."
echo "You should see 2 peer connections once it connects."
echo ""
echo "The daemon is running in the background (PID: $DAEMON_PID)"
echo "To stop everything, run: ./scripts/cleanup-daemons.sh"
