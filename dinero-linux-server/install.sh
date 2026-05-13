#!/bin/bash
# Dinero Node Linux Server Installation Script

set -e

echo "🚀 Installing Dinero Node on Linux Server..."
echo "📊 Server Specs: 1GB RAM, 1 CPU Core, 15GB Storage"
echo ""

# Update system
echo "📦 Updating system packages..."
sudo apt update && sudo apt upgrade -y

# Install dependencies
echo "🔧 Installing dependencies..."
sudo apt install -y \
    curl \
    wget \
    unzip \
    htop \
    ufw \
    fail2ban \
    logrotate

# Create dinero user (security best practice)
echo "👤 Creating dinero user..."
if ! id "dinero" &>/dev/null; then
    sudo useradd -m -s /bin/bash dinero
    sudo usermod -aG sudo dinero
fi

# Setup directories
echo "📁 Setting up directories..."
sudo mkdir -p /opt/dinero
sudo mkdir -p /var/log/dinero
sudo mkdir -p /etc/dinero
sudo chown -R dinero:dinero /opt/dinero
sudo chown -R dinero:dinero /var/log/dinero

# Copy configuration
echo "⚙️  Installing configuration..."
sudo cp dinero.conf /etc/dinero/
sudo chown dinero:dinero /etc/dinero/dinero.conf

# Setup firewall
echo "🔒 Configuring firewall..."
sudo ufw allow ssh
sudo ufw allow 23999/tcp  # P2P
sudo ufw allow 20998/tcp  # RPC (be careful with this in production)
sudo ufw allow 22999/tcp  # WebSocket
sudo ufw --force enable

# Setup systemd service
echo "🔄 Creating systemd service..."
sudo tee /etc/systemd/system/dinerod.service > /dev/null <<EOF
[Unit]
Description=Dinero Cryptocurrency Node
After=network.target

[Service]
Type=forking
User=dinero
Group=dinero
WorkingDirectory=/opt/dinero
ExecStart=/opt/dinero/dinerod -conf=/etc/dinero/dinero.conf -datadir=/opt/dinero/data -daemon
ExecStop=/opt/dinero/dinero-cli -conf=/etc/dinero/dinero.conf -datadir=/opt/dinero/data stop
Restart=always
RestartSec=10
TimeoutStopSec=60

# Security settings
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF

# Setup log rotation
echo "📋 Setting up log rotation..."
sudo tee /etc/logrotate.d/dinero > /dev/null <<EOF
/var/log/dinero/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    create 644 dinero dinero
    postrotate
        systemctl reload dinerod || true
    endscript
}
EOF

# Enable and start service
echo "▶️  Enabling Dinero service..."
sudo systemctl daemon-reload
sudo systemctl enable dinerod

echo ""
echo "✅ Installation complete!"
echo ""
echo "📋 Next steps:"
echo "1. Copy dinerod and dinero-cli binaries to /opt/dinero/"
echo "2. Start the service: sudo systemctl start dinerod"
echo "3. Check status: sudo systemctl status dinerod"
echo "4. View logs: sudo journalctl -u dinerod -f"
echo ""
echo "🌐 Your server will be accessible at:"
echo "   P2P: 96.9.226.98:23999"
echo "   RPC: 96.9.226.98:20998"
echo "   WebSocket: 96.9.226.98:22999"
