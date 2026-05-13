#!/bin/bash
# Install DineroCoin systemd service for auto-restart and boot startup
# Run this script on the Dell tower as root or with sudo

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_FILE="$SCRIPT_DIR/dinerod.service"
SYSTEMD_DIR="/etc/systemd/system"

echo "════════════════════════════════════════════════════════════════"
echo "  DineroCoin Systemd Service Installer"
echo "════════════════════════════════════════════════════════════════"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "❌ ERROR: This script must be run as root or with sudo"
    echo "Usage: sudo ./install_systemd_service.sh"
    exit 1
fi

# Check if service file exists
if [ ! -f "$SERVICE_FILE" ]; then
    echo "❌ ERROR: Service file not found at $SERVICE_FILE"
    exit 1
fi

echo "[1/6] Checking prerequisites..."
echo "  Service file: $SERVICE_FILE"
echo "  Target: $SYSTEMD_DIR/dinerod.service"
echo ""

# Stop existing service if running
echo "[2/6] Stopping existing dinerod service (if running)..."
if systemctl is-active --quiet dinerod 2>/dev/null; then
    systemctl stop dinerod
    echo "  ✓ Stopped running service"
else
    echo "  ℹ No existing service running"
fi
echo ""

# Copy service file
echo "[3/6] Installing service file..."
cp "$SERVICE_FILE" "$SYSTEMD_DIR/dinerod.service"
chmod 644 "$SYSTEMD_DIR/dinerod.service"
echo "  ✓ Service file installed"
echo ""

# Reload systemd
echo "[4/6] Reloading systemd daemon..."
systemctl daemon-reload
echo "  ✓ Systemd daemon reloaded"
echo ""

# Enable service for boot startup
echo "[5/6] Enabling service for automatic startup on boot..."
systemctl enable dinerod
echo "  ✓ Service enabled"
echo ""

# Start service
echo "[6/6] Starting dinerod service..."
systemctl start dinerod
echo "  ✓ Service started"
echo ""

# Wait a moment for startup
sleep 3

# Show status
echo "════════════════════════════════════════════════════════════════"
echo "  Installation Complete!"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "Service Status:"
systemctl status dinerod --no-pager || true
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  Useful Commands:"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "  Check status:     sudo systemctl status dinerod"
echo "  View logs:        sudo journalctl -u dinerod -f"
echo "  Restart:          sudo systemctl restart dinerod"
echo "  Stop:             sudo systemctl stop dinerod"
echo "  Start:            sudo systemctl start dinerod"
echo "  Disable startup:  sudo systemctl disable dinerod"
echo ""
echo "  View live logs:   sudo journalctl -u dinerod -f --since today"
echo "  View errors:      sudo journalctl -u dinerod -p err"
echo ""
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "✅ DineroCoin daemon will now auto-start on boot and restart on failure"
echo ""
