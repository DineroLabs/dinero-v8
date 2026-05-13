#!/usr/bin/env bash
set -euo pipefail

# Start permanent Dinero mainnet node with persistent configuration
# Usage: ./scripts/start-permanent-node.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DATADIR="$HOME/Documents/DineroCoin/node-mainnet"
DAEMON="$PROJECT_ROOT/build-sqlite/bin/dinerod"

echo "🚀 Starting permanent Dinero mainnet node..."
echo "📁 Data directory: $DATADIR"
echo "⚙️  Configuration: $DATADIR/dinero.conf"

# Check if daemon exists
if [[ ! -f "$DAEMON" ]]; then
    echo "❌ Daemon not found: $DAEMON"
    echo "💡 Run: cmake --build build-sqlite --target dinerod -j8"
    exit 1
fi

# Check if config exists
if [[ ! -f "$DATADIR/dinero.conf" ]]; then
    echo "❌ Configuration not found: $DATADIR/dinero.conf"
    echo "💡 Create the config file first"
    exit 1
fi

# Stop any existing daemon
echo "🛑 Stopping any existing daemon..."
pkill -f "dinerod.*$DATADIR" 2>/dev/null || true
sleep 1

# Start daemon with persistent config
echo "▶️  Starting daemon..."
"$DAEMON" \
    -datadir="$DATADIR" \
    -rpcport=26680 \
    -wsport=26682 \
    -wsbind=0.0.0.0 \
    -wspath=/api/ws \
    -daemon

echo "⏳ Waiting for startup..."
sleep 3

# Verify it's running
if pgrep -f "dinerod.*$DATADIR" >/dev/null; then
    echo "✅ Daemon is running!"
    
    # Check ports
    echo "🔍 Checking port bindings..."
    if lsof -nP -iTCP:26680 -sTCP:LISTEN >/dev/null 2>&1; then
        echo "✅ RPC listening on port 26680"
    else
        echo "❌ RPC not listening on port 26680"
    fi
    
    if lsof -nP -iTCP:26682 -sTCP:LISTEN >/dev/null 2>&1; then
        echo "✅ WebSocket listening on port 26682"
        if lsof -nP -iTCP:26682 -sTCP:LISTEN | grep -q "TCP \*:26682"; then
            echo "✅ WebSocket bound to all interfaces (0.0.0.0)"
        fi
    else
        echo "❌ WebSocket not listening on port 26682"
    fi
    
    echo ""
    echo "🎉 Permanent node is running!"
    echo "🌐 RPC: http://127.0.0.1:26680/"
    echo "🔌 WebSocket: ws://127.0.0.1:26682/api/ws"
    echo "📊 Logs: $DATADIR/ (when logging is implemented)"
    echo ""
    echo "💡 To stop: pkill -f 'dinerod.*$DATADIR'"
    echo "💡 To check status: pgrep -f 'dinerod.*$DATADIR'"
    
else
    echo "❌ Daemon failed to start"
    exit 1
fi
