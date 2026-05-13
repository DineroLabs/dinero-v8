#!/bin/bash

# Dinero Service Installation Script - Linux
# Installs dinerod as a user-level systemd service

set -e

echo "🐧 Installing Dinero daemon service for Linux..."

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Default paths
DINERO_BINARY="$PROJECT_ROOT/build/bin/dinerod"
DEFAULT_DATADIR="$HOME/.local/share/DineroCoin"
SERVICE_FILE="$HOME/.config/systemd/user/dinerod.service"

# Check if binary exists
if [ ! -f "$DINERO_BINARY" ]; then
    echo "❌ Dinero binary not found at: $DINERO_BINARY"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

# Check if systemd is available
if ! command -v systemctl &> /dev/null; then
    echo "❌ systemd not found. This script requires systemd."
    exit 1
fi

# Create systemd user directory
echo "📁 Creating systemd user directory..."
mkdir -p "$HOME/.config/systemd/user"

# Create data directory
echo "📁 Creating data directory..."
mkdir -p "$DEFAULT_DATADIR"

# Create secure config
echo "🔒 Creating secure configuration..."
cat > "$DEFAULT_DATADIR/dinero.conf" << EOF
# Dinero Core Configuration
# Generated with secure defaults

server=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=20998
port=20999
rpccookiefile=$DEFAULT_DATADIR/.cookie

# Security: localhost-only RPC binding
# Cookie authentication enabled
# No external network access by default
EOF

# Set secure permissions
chmod 600 "$DEFAULT_DATADIR/dinero.conf"

# Create the systemd service file
echo "📝 Creating systemd service file..."
cat > "$SERVICE_FILE" << EOF
[Unit]
Description=Dinero Core Daemon
Documentation=https://github.com/dinero/dinero
After=network.target

[Service]
Type=simple
ExecStart=$DINERO_BINARY -datadir=$DEFAULT_DATADIR
Restart=always
RestartSec=10
User=%i
Group=%i

# Security settings
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$DEFAULT_DATADIR

# Resource limits
LimitNOFILE=65536
LimitNPROC=32768

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=dinerod

[Install]
WantedBy=default.target
EOF

# Reload systemd user daemon
echo "🔄 Reloading systemd user daemon..."
systemctl --user daemon-reload

# Enable the service
echo "🔧 Enabling Dinero daemon service..."
systemctl --user enable dinerod.service

# Start the service
echo "▶️  Starting Dinero daemon..."
systemctl --user start dinerod.service

# Wait for startup
sleep 2

# Check service status
if systemctl --user is-active --quiet dinerod.service; then
    echo "✅ Dinero daemon service installed and started successfully!"
    echo ""
    echo "📊 Service Status:"
    systemctl --user status dinerod.service --no-pager -l
    echo ""
    echo "🔧 Management Commands:"
    echo "  Stop:     systemctl --user stop dinerod.service"
    echo "  Start:    systemctl --user start dinerod.service"
    echo "  Restart:  systemctl --user restart dinerod.service"
    echo "  Status:   systemctl --user status dinerod.service"
    echo "  Logs:     journalctl --user -u dinerod.service -f"
    echo "  Disable:  systemctl --user disable dinerod.service"
    echo ""
    echo "🌐 RPC Endpoint: http://127.0.0.1:20998"
    echo "📁 Data Directory: $DEFAULT_DATADIR"
    echo "📝 Config File: $DEFAULT_DATADIR/dinero.conf"
    echo ""
    echo "💡 Note: User services start automatically on login."
    echo "   To start on boot, enable lingering: sudo loginctl enable-linger \$USER"
else
    echo "❌ Failed to start Dinero daemon service"
    echo "Check logs: journalctl --user -u dinerod.service"
    exit 1
fi
