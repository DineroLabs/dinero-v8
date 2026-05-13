#!/bin/bash

# Dinero Service Installation Script
# Installs dinerod as a user-level service with auto-restart

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
SERVICE_NAME="org.dinero.dinerod"
PLIST_FILE="$HOME/Library/LaunchAgents/$SERVICE_NAME.plist"
BINARY_PATH="/usr/local/bin/dinerod"
DATA_DIR="$HOME/.dinero"
LOG_DIR="$HOME/Library/Logs/Dinero"

echo -e "${GREEN}=== Dinero Service Installation ===${NC}"

# Check if running on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo -e "${RED}Error: This script is for macOS only${NC}"
    exit 1
fi

# Check if dinerod binary exists
if [[ ! -f "$BINARY_PATH" ]]; then
    echo -e "${YELLOW}Warning: dinerod not found at $BINARY_PATH${NC}"
    echo "Please install dinerod to /usr/local/bin/ or update the BINARY_PATH in this script"
    read -p "Continue anyway? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Create directories
echo "Creating directories..."
mkdir -p "$DATA_DIR"
mkdir -p "$LOG_DIR"
mkdir -p "$HOME/Library/LaunchAgents"

# Stop existing service if running
if launchctl list | grep -q "$SERVICE_NAME"; then
    echo "Stopping existing service..."
    launchctl unload "$PLIST_FILE" 2>/dev/null || true
fi

# Create the plist file
echo "Creating service configuration..."
cat > "$PLIST_FILE" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$SERVICE_NAME</string>
    
    <key>ProgramArguments</key>
    <array>
        <string>$BINARY_PATH</string>
        <string>-datadir</string>
        <string>$DATA_DIR</string>
    </array>
    
    <key>RunAtLoad</key>
    <true/>
    
    <key>KeepAlive</key>
    <true/>
    
    <key>StandardOutPath</key>
    <string>$LOG_DIR/dinerod.log</string>
    
    <key>StandardErrorPath</key>
    <string>$LOG_DIR/dinerod.log</string>
    
    <key>ProcessType</key>
    <string>Background</string>
    
    <key>ThrottleInterval</key>
    <integer>10</integer>
    
    <key>ExitTimeOut</key>
    <integer>30</integer>
    
    <key>EnvironmentVariables</key>
    <dict>
        <key>DINERO_DATADIR</key>
        <string>$DATA_DIR</string>
    </dict>
</dict>
</plist>
EOF

# Set proper permissions
chmod 644 "$PLIST_FILE"

# Load the service
echo "Loading service..."
launchctl load "$PLIST_FILE"

# Wait a moment for service to start
sleep 2

# Check if service is running
if launchctl list | grep -q "$SERVICE_NAME"; then
    echo -e "${GREEN}✅ Service installed and started successfully!${NC}"
    echo
    echo "Service details:"
    echo "  Name: $SERVICE_NAME"
    echo "  Data directory: $DATA_DIR"
    echo "  Log file: $LOG_DIR/dinerod.log"
    echo "  Plist file: $PLIST_FILE"
    echo
    echo "Useful commands:"
    echo "  Check status: launchctl list | grep $SERVICE_NAME"
    echo "  View logs: tail -f $LOG_DIR/dinerod.log"
    echo "  Stop service: launchctl unload $PLIST_FILE"
    echo "  Start service: launchctl load $PLIST_FILE"
    echo "  Uninstall: launchctl unload $PLIST_FILE && rm $PLIST_FILE"
else
    echo -e "${RED}❌ Service failed to start${NC}"
    echo "Check the log file: $LOG_DIR/dinerod.log"
    exit 1
fi

echo
echo -e "${GREEN}Installation complete!${NC}"
