#!/bin/bash
# Dinero Production Node Deployment Script (GreenCloud VPS)
# Run this on your GreenCloud server via SSH to configure firewall and daemon

set -e

echo "════════════════════════════════════════════════════════════"
echo "🚀 Dinero Production Node Setup (GreenCloud VPS)"
echo "════════════════════════════════════════════════════════════"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "❌ Please run as root (use: sudo bash $0)"
    exit 1
fi

# Detect the server location (optional - you can hardcode this)
read -p "Enter server location (CA/VA/OTHER): " SERVER_LOCATION
read -p "Enter the OTHER server IP to peer with: " PEER_IP

echo ""
echo "📋 Configuration Summary:"
echo "  Location: $SERVER_LOCATION"
echo "  Peer IP:  $PEER_IP"
echo ""

# Step 1: Install UFW if not present
echo "═══ Step 1: Installing UFW ═══"
apt-get update -qq
apt-get install -y ufw

# Step 2: Configure UFW firewall rules
echo ""
echo "═══ Step 2: Configuring Firewall Rules ═══"

# Enable UFW (will prompt for confirmation)
ufw --force enable

# Allow SSH (critical - don't lock yourself out!)
ufw allow 22/tcp
echo "✅ Allowed SSH (port 22)"

# Allow Dinero P2P port (19003)
ufw allow 19003/tcp
echo "✅ Allowed P2P port 19003"

# Allow WebSocket port (21001)
ufw allow 21001/tcp
echo "✅ Allowed WebSocket port 21001"

# Allow RPC port (20998) - ONLY from specific IPs for security
echo ""
echo "⚠️  RPC port 20998 should be restricted to trusted IPs only"
echo "   We'll allow it from localhost by default"
ufw allow from 127.0.0.1 to any port 20998 proto tcp
echo "✅ Allowed RPC port 20998 from localhost"

# Optionally allow RPC from your admin IP
read -p "Enter your admin IP for RPC access (or press Enter to skip): " ADMIN_IP
if [ ! -z "$ADMIN_IP" ]; then
    ufw allow from $ADMIN_IP to any port 20998 proto tcp
    echo "✅ Allowed RPC port 20998 from $ADMIN_IP"
fi

# Show firewall status
echo ""
echo "═══ Firewall Configuration Complete ═══"
ufw status verbose

# Step 3: Create Dinero configuration directory
echo ""
echo "═══ Step 3: Creating Dinero Configuration ═══"

DINERO_DIR="/root/.dinero"
mkdir -p "$DINERO_DIR"

# Create dinero.conf
cat > "$DINERO_DIR/dinero.conf" <<EOF
# Dinero Production Node Configuration
# Generated on $(date)

# Network Settings
listen=1
bind=0.0.0.0
port=19003
maxconnections=64

# RPC Settings
rpcbind=0.0.0.0
rpcallowip=127.0.0.1
rpcport=20998

# WebSocket Settings
wsport=21001

# Peer Configuration
addnode=$PEER_IP:19003

# Logging
debug=net
debug=p2p
EOF

if [ ! -z "$ADMIN_IP" ]; then
    echo "rpcallowip=$ADMIN_IP" >> "$DINERO_DIR/dinero.conf"
fi

echo "✅ Created dinero.conf at $DINERO_DIR/dinero.conf"
echo ""
cat "$DINERO_DIR/dinero.conf"

# Step 4: Verify dinerod binary exists
echo ""
echo "═══ Step 4: Verifying Binary ═══"

DINEROD_PATH="/root/DineroCoin/build/dinerod"

if [ ! -f "$DINEROD_PATH" ]; then
    echo "❌ dinerod binary not found at $DINEROD_PATH"
    echo "   Please build the project first:"
    echo "   cd /root/DineroCoin && cmake --build build --target dinerod -j\$(nproc)"
    exit 1
fi

echo "✅ Found dinerod at $DINEROD_PATH"
$DINEROD_PATH --version

# Step 5: Create systemd service (optional but recommended)
echo ""
read -p "Create systemd service for auto-start? (y/n): " CREATE_SERVICE

if [ "$CREATE_SERVICE" = "y" ]; then
    echo "═══ Step 5: Creating systemd Service ═══"

    cat > /etc/systemd/system/dinerod.service <<EOF
[Unit]
Description=Dinero Daemon
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/root/DineroCoin
ExecStart=$DINEROD_PATH -datadir=$DINERO_DIR -printtoconsole
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable dinerod.service
    echo "✅ Created systemd service"
    echo "   Start with: systemctl start dinerod"
    echo "   Check status: systemctl status dinerod"
    echo "   View logs: journalctl -u dinerod -f"
fi

# Step 6: Final instructions
echo ""
echo "════════════════════════════════════════════════════════════"
echo "✅ Deployment Complete!"
echo "════════════════════════════════════════════════════════════"
echo ""
echo "📝 Next Steps:"
echo ""
echo "1. Start the daemon:"
if [ "$CREATE_SERVICE" = "y" ]; then
    echo "   systemctl start dinerod"
else
    echo "   cd /root/DineroCoin"
    echo "   ./build/dinerod -datadir=$DINERO_DIR -printtoconsole"
fi
echo ""
echo "2. Verify it's running:"
echo "   ./build/dinero-cli -datadir=$DINERO_DIR getconnectioncount"
echo "   ./build/dinero-cli -datadir=$DINERO_DIR getpeerinfo"
echo ""
echo "3. Check logs for peer connections:"
echo "   grep 'Connected to peer' $DINERO_DIR/debug.log"
echo "   grep 'Incoming connection' $DINERO_DIR/debug.log"
echo ""
echo "4. Verify ports are open from your local machine:"
echo "   nc -vz $(hostname -I | awk '{print $1}') 19003"
echo ""
echo "════════════════════════════════════════════════════════════"
