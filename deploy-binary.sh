#!/bin/bash
set -e

echo "🚀 Deploying Linux binary to Canada VPS..."

# Upload the working binary
echo "📤 Uploading binary..."
scp -i ~/.ssh/dinero_vps_id dinerod-linux2 root@96.9.226.98:/tmp/dinerod

# Deploy and restart service
echo "🔧 Installing and restarting service..."
ssh -i ~/.ssh/dinero_vps_id root@96.9.226.98 << 'EOF'
# Stop the broken service
sudo systemctl stop dinerod

# Install the working binary
sudo install -m0755 /tmp/dinerod /usr/local/bin/dinerod

# Start the service
sudo systemctl start dinerod

# Check status
echo "📊 Service status:"
sudo systemctl status dinerod --no-pager

echo "📜 Recent logs:"
sudo journalctl -u dinerod -n 10 --no-pager

echo "🎉 Deployment complete!"
EOF
