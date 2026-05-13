#!/bin/bash
# Dinero GUI Launcher
# Automatically cleans up and starts the GUI with proper configuration

cd "$(dirname "$0")/.."

echo "════════════════════════════════════════════════"
echo "Starting Dinero Wallet"
echo "════════════════════════════════════════════════"
echo ""

# Check if config exists, create if not
CONFIG_DIR="$HOME/.dinero"
mkdir -p "$CONFIG_DIR"

if [ ! -f "$CONFIG_DIR/dinero.conf" ]; then
    echo "Creating default configuration with seed nodes..."
    cat > "$CONFIG_DIR/dinero.conf" << 'EOF'
# Dinero Configuration
addnode=172.93.160.131:20999
addnode=173.249.195.59:20999
addnode=rpc.dinero-coin.com:20999
addnode=rpc1.dinero-coin.com:20999
port=20999
maxconnections=125
listen=1
EOF
    echo "✓ Configuration created at $CONFIG_DIR/dinero.conf"
fi

# Clean up any stale processes/locks
echo ""
echo "Checking for running processes..."
if pgrep -q dinerod || pgrep -q dinero-qt; then
    echo "⚠️  Found running processes, cleaning up..."
    ./scripts/cleanup-daemons.sh
    sleep 2
else
    echo "✓ No stale processes found"
fi

# Start GUI with correct datadir and seed nodes
echo ""
echo "Starting GUI with datadir: $CONFIG_DIR"
echo "════════════════════════════════════════════════"
exec ./build/gui/dinero-qt \
    -datadir="$CONFIG_DIR" \
    -addnode=172.93.160.131:20999 \
    -addnode=173.249.195.59:20999 \
    -addnode=rpc.dinero-coin.com:20999 \
    -addnode=rpc1.dinero-coin.com:20999
