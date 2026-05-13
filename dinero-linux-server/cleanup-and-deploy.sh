#!/bin/bash
# Clean up old cryptocurrency installations and deploy Dinero

set -e

echo "🧹 Cleaning up old cryptocurrency installations..."

# Stop any running crypto services
sudo systemctl stop hlcoin || true
sudo systemctl stop bitcoin || true
sudo systemctl stop litecoin || true
sudo systemctl disable hlcoin || true
sudo systemctl disable bitcoin || true
sudo systemctl disable litecoin || true

# Remove old service files
sudo rm -f /etc/systemd/system/hlcoin.service
sudo rm -f /etc/systemd/system/bitcoin.service
sudo rm -f /etc/systemd/system/litecoin.service

# Clean up old users and directories
sudo userdel -r hlcoin 2>/dev/null || true
sudo userdel -r bitcoin 2>/dev/null || true
sudo rm -rf /opt/hlcoin /opt/bitcoin /opt/litecoin || true
sudo rm -rf /etc/hlcoin /etc/bitcoin /etc/litecoin || true

# Clean up old data (be careful with this!)
echo "⚠️  Do you want to remove old blockchain data? (y/N)"
read -r response
if [[ "$response" =~ ^[Yy]$ ]]; then
    sudo rm -rf /home/*/.*coin* || true
    sudo rm -rf /root/.*coin* || true
    echo "🗑️  Old blockchain data removed"
fi

# Update hostname if needed
current_hostname=$(hostname)
if [[ "$current_hostname" == *"hlcoin"* ]] || [[ "$current_hostname" == *"bitcoin"* ]]; then
    echo "🏷️  Updating hostname to dinero-node..."
    sudo hostnamectl set-hostname dinero-node
    echo "127.0.0.1 dinero-node" | sudo tee -a /etc/hosts
fi

# Clean up bash prompt
if grep -q "hlcoin\|bitcoin" ~/.bashrc 2>/dev/null; then
    echo "🎨 Cleaning up bash prompt..."
    sed -i 's/hlcoin/dinero/g' ~/.bashrc || true
    sed -i 's/bitcoin/dinero/g' ~/.bashrc || true
fi

# Reload systemd
sudo systemctl daemon-reload

echo "✅ Cleanup complete!"
echo ""
echo "🚀 Now installing Dinero node..."

# Run the main installation
./install.sh

echo ""
echo "✅ Dinero node installation complete!"
echo "🔄 Please logout and login again to see the updated hostname"
echo "📋 Next step: Run ./build-on-server.sh to compile Dinero"
