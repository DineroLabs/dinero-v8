#!/bin/bash

# Dinero Service Installation Script - macOS
# Installs dinerod as a user-level launchd service

set -e

echo "🍎 Installing Dinero daemon service for macOS..."

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Default paths
DINERO_BINARY="$PROJECT_ROOT/build/bin/dinerod"
DEFAULT_DATADIR="$HOME/.dinero"
PLIST_FILE="$HOME/Library/LaunchAgents/org.dinero.dinerod.plist"

# Check if binary exists
if [ ! -f "$DINERO_BINARY" ]; then
    echo "❌ Dinero binary not found at: $DINERO_BINARY"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

# Create LaunchAgents directory if it doesn't exist
mkdir -p "$HOME/Library/LaunchAgents"

# Create the plist file
echo "📝 Creating launchd plist file..."
cat > "$PLIST_FILE" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>org.dinero.dinerod</string>
    <key>ProgramArguments</key>
    <array>
        <string>$DINERO_BINARY</string>
        <string>-datadir</string>
        <string>$DEFAULT_DATADIR</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$HOME/Library/Logs/dinerod.log</string>
    <key>StandardErrorPath</key>
    <string>$HOME/Library/Logs/dinerod-error.log</string>
    <key>ProcessType</key>
    <string>Background</string>
    <key>ThrottleInterval</key>
    <integer>10</integer>
</dict>
</plist>
EOF

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

# Load the service
echo "🚀 Loading launchd service..."
launchctl load "$PLIST_FILE"

# Start the service
echo "▶️  Starting Dinero daemon..."
launchctl start org.dinero.dinerod

# Wait a moment for startup
sleep 2

# Check if service is running
if launchctl list | grep -q "org.dinero.dinerod"; then
    echo "✅ Dinero daemon service installed and started successfully!"
    echo ""
    echo "📊 Service Status:"
    launchctl list | grep "org.dinero.dinerod"
    echo ""
    echo "📝 Logs:"
    echo "  Standard: $HOME/Library/Logs/dinerod.log"
    echo "  Errors:   $HOME/Library/Logs/dinerod-error.log"
    echo ""
    echo "🔧 Management Commands:"
    echo "  Stop:     launchctl stop org.dinero.dinerod"
    echo "  Start:    launchctl start org.dinero.dinerod"
    echo "  Unload:   launchctl unload $PLIST_FILE"
    echo "  Status:   launchctl list | grep dinerod"
    echo ""
    echo "🌐 RPC Endpoint: http://127.0.0.1:20998"
    echo "📁 Data Directory: $DEFAULT_DATADIR"
else
    echo "❌ Failed to start Dinero daemon service"
    echo "Check logs: $HOME/Library/Logs/dinerod-error.log"
    exit 1
fi
