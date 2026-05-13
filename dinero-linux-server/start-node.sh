#!/bin/bash
# Dinero Node Start Script - Linux Server

echo "🚀 Starting Dinero Node..."

# Check if running as dinero user
if [ "$USER" != "dinero" ]; then
    echo "⚠️  Switching to dinero user..."
    sudo -u dinero $0
    exit $?
fi

# Start via systemd (recommended)
if command -v systemctl &> /dev/null; then
    echo "📋 Starting via systemd..."
    sudo systemctl start dinerod
    sleep 3
    sudo systemctl status dinerod --no-pager
else
    # Fallback: direct start
    echo "📋 Starting directly..."
    mkdir -p data/mainnet/logs
    ./dinerod \
        -conf=/etc/dinero/dinero.conf \
        -datadir="$(pwd)/data" \
        -daemon=1
fi

echo ""
echo "✅ Dinero node started!"
echo ""
echo "📊 Monitor with:"
echo "  Status: sudo systemctl status dinerod"
echo "  Logs:   sudo journalctl -u dinerod -f"
echo "  RPC:    ./dinero-cli -datadir=./data getblockchaininfo"
