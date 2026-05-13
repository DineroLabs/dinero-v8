#!/bin/bash
# Dinero Node Stop Script - Linux Server

echo "🛑 Stopping Dinero Node..."

# Stop via systemd (recommended)
if command -v systemctl &> /dev/null; then
    echo "📋 Stopping via systemd..."
    sudo systemctl stop dinerod
    echo "✅ Dinero node stopped!"
else
    # Fallback: RPC stop
    echo "📋 Stopping via RPC..."
    ./dinero-cli -datadir="./data" stop
    echo "✅ Dinero node stopped!"
fi
