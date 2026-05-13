#!/bin/bash
# Dinero Seed Node Deployment Script
# Deploys and configures production seed nodes

set -euo pipefail

# Configuration
SEED_REGIONS=(
    "us-east-1:seed1.dinero.org"
    "us-west-2:seed2.dinero.org" 
    "eu-west-1:seed3.dinero.org"
    "eu-central-1:seed4.dinero.org"
    "ap-southeast-1:seed5.dinero.org"
    "ap-northeast-1:seed6.dinero.org"
    "sa-east-1:seed7.dinero.org"
    "ca-central-1:seed8.dinero.org"
)

INSTANCE_TYPE="t3.medium"  # 2 vCPU, 4GB RAM
DISK_SIZE="100"            # GB SSD

echo "🌱 Dinero Seed Node Deployment"
echo "=================================="

# Function to deploy single seed node
deploy_seed_node() {
    local region=$1
    local hostname=$2
    
    echo "🚀 Deploying seed node: $hostname in $region"
    
    # Create cloud instance (example for AWS)
    cat > seed-node-userdata.sh <<EOF
#!/bin/bash
# Seed node initialization script

# System updates and hardening
apt-get update && apt-get upgrade -y
apt-get install -y fail2ban ufw htop iotop curl wget git build-essential

# Configure firewall
ufw default deny incoming
ufw default allow outgoing
ufw allow ssh
ufw allow 8333/tcp  # Dinero P2P
ufw --force enable

# Configure fail2ban
systemctl enable fail2ban
systemctl start fail2ban

# Create dinero user
useradd -m -s /bin/bash dinero
usermod -aG sudo dinero

# Install Dinero daemon
# (This would download and install your compiled dinerod)
wget -O /tmp/dinerod https://github.com/Trucker2827/dinero-cli/releases/latest/download/dinerod
chmod +x /tmp/dinerod
mv /tmp/dinerod /usr/local/bin/

# Create data directory
mkdir -p /home/dinero/.dinero
chown -R dinero:dinero /home/dinero/.dinero

# Install configuration
cat > /home/dinero/.dinero/dinero.conf <<'CONF'
$(cat seed-node.conf)
CONF

chown dinero:dinero /home/dinero/.dinero/dinero.conf
chmod 600 /home/dinero/.dinero/dinero.conf

# Create systemd service
cat > /etc/systemd/system/dinerod.service <<'SERVICE'
[Unit]
Description=Dinero Seed Node
After=network-online.target
Wants=network-online.target

[Service]
Type=forking
User=dinero
Group=dinero
ExecStart=/usr/local/bin/dinerod -daemon -conf=/home/dinero/.dinero/dinero.conf
ExecStop=/usr/local/bin/dinero-cli stop
Restart=on-failure
RestartSec=5
LimitNOFILE=8192

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=false

[Install]
WantedBy=multi-user.target
SERVICE

# Enable and start service
systemctl daemon-reload
systemctl enable dinerod
systemctl start dinerod

# Configure log rotation
cat > /etc/logrotate.d/dinero <<'LOGROTATE'
/home/dinero/.dinero/debug.log {
    daily
    missingok
    rotate 52
    compress
    delaycompress
    copytruncate
    notifempty
    create 644 dinero dinero
}
LOGROTATE

# Install monitoring agent (node_exporter)
wget -O /tmp/node_exporter.tar.gz https://github.com/prometheus/node_exporter/releases/latest/download/node_exporter-*-linux-amd64.tar.gz
tar -xzf /tmp/node_exporter.tar.gz -C /tmp
mv /tmp/node_exporter-*/node_exporter /usr/local/bin/
rm -rf /tmp/node_exporter*

# Create node_exporter service
cat > /etc/systemd/system/node_exporter.service <<'NODEEXP'
[Unit]
Description=Node Exporter
After=network.target

[Service]
User=nobody
Group=nobody
Type=simple
ExecStart=/usr/local/bin/node_exporter --web.listen-address=:9100
Restart=on-failure

[Install]
WantedBy=multi-user.target
NODEEXP

systemctl daemon-reload
systemctl enable node_exporter
systemctl start node_exporter

# Configure connection limits (DDoS protection)
cat >> /etc/sysctl.conf <<'SYSCTL'
# Dinero seed node optimizations
net.core.somaxconn = 1024
net.ipv4.tcp_max_syn_backlog = 2048
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_fin_timeout = 30
net.ipv4.tcp_keepalive_time = 120
net.ipv4.tcp_keepalive_intvl = 15
net.ipv4.tcp_keepalive_probes = 5
SYSCTL

sysctl -p

echo "✅ Seed node $hostname deployed successfully"
EOF

    # Deploy the instance (pseudo-code - adapt for your cloud provider)
    # aws ec2 run-instances \
    #     --region $region \
    #     --image-id ami-ubuntu-22.04 \
    #     --instance-type $INSTANCE_TYPE \
    #     --key-name your-key \
    #     --security-group-ids sg-seed-nodes \
    #     --user-data file://seed-node-userdata.sh \
    #     --block-device-mappings DeviceName=/dev/sda1,Ebs="{VolumeSize=$DISK_SIZE,VolumeType=gp3}" \
    #     --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$hostname},{Key=Role,Value=seed-node}]"
    
    echo "📋 Instance deployment command prepared for $hostname"
}

# Deploy all seed nodes
for region_host in "${SEED_REGIONS[@]}"; do
    IFS=':' read -r region hostname <<< "$region_host"
    deploy_seed_node "$region" "$hostname"
done

echo ""
echo "🎉 All seed nodes deployment prepared!"
echo ""
echo "📝 Next steps:"
echo "1. Execute the cloud deployment commands"
echo "2. Configure DNS for seed hostnames"
echo "3. Set up monitoring dashboards"
echo "4. Test connectivity between nodes"
echo ""
echo "🔍 Monitor deployment:"
echo "   ssh ubuntu@seed1.dinero.org 'sudo systemctl status dinerod'"
echo "   dinero-cli -rpcconnect=seed1.dinero.org getnetworkinfo"
