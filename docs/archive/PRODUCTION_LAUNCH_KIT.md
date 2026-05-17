# Dinero Production Launch Kit 🚀

Complete deployment guide for production-ready Dinero nodes.

## 📋 Pre-Flight Checklist

- [ ] Build tested: `cmake --build build-debug -j8 --target dinerod`
- [ ] Smoke tests pass: `./scripts/test-p2p-dynamic.sh`
- [ ] Binary ready: `./build-debug/bin/dinerod`
- [ ] Target host accessible with sudo

## 🏗️ Host Setup (Run Once Per Host)

### 1. Create Dedicated User & Directories

```bash
# Create dinero user
sudo useradd -r -m -d /var/lib/dinero -s /usr/sbin/nologin dinero || true

# Create directory structure
sudo mkdir -p /var/lib/dinero/{data,logs}
sudo mkdir -p /etc/dinero
sudo chown -R dinero:dinero /var/lib/dinero
```

### 2. Install Binary

```bash
# Copy binary to system location
sudo cp ./build-debug/bin/dinerod /usr/local/bin/
sudo chmod +x /usr/local/bin/dinerod
sudo chown root:root /usr/local/bin/dinerod
```

### 3. Production Configuration

```bash
# Create production config
sudo tee /etc/dinero/dinero.conf >/dev/null <<'CONF'
# Dinero Production Configuration
# ==============================

# Core Settings
datadir=/var/lib/dinero/data
nodeinfo=/var/lib/dinero/nodeinfo.json

# Network Security (localhost only)
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=0          # auto-select
wsport=0           # auto-select

# P2P Settings
p2p=1
port=0             # auto-select
maxpeers=16
timeout=30

# Performance & DoS Protection
blockmaxinflight=128
perpeermax=16
rpcbatchlimit=100

# Database Durability
# Note: Switch to sqlite_synchronous=full after IBD
sqlite_wal=1
sqlite_synchronous=normal

# Wallet
autowallet=default

# Logging (reduce after IBD)
debug=p2p,headers,blocks,scheduler
logtimestamp=1
printtoconsole=1

# Security
rpcauth=dinero:7d9ba5ae63c3d4dc30583ff4ca7bda05$d5b9b8cd98f00b204e9800998ecf8427e
CONF

# Secure config file
sudo chown root:dinero /etc/dinero/dinero.conf
sudo chmod 640 /etc/dinero/dinero.conf
```

### 4. Systemd Service

```bash
# Create systemd service
sudo tee /etc/systemd/system/dinerod.service >/dev/null <<'SERVICE'
[Unit]
Description=Dinero Cryptocurrency Node
Documentation=https://github.com/dinero/dinero
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=dinero
Group=dinero
ExecStart=/usr/local/bin/dinerod \
  -server=1 \
  -daemon=0 \
  -printtoconsole \
  -conf=/etc/dinero/dinero.conf
WorkingDirectory=/var/lib/dinero
Restart=on-failure
RestartSec=3
TimeoutStopSec=30

# Resource Limits
LimitNOFILE=65535
Nice=5

# Security Hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictRealtime=true
RestrictSUIDSGID=true

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=dinerod

[Install]
WantedBy=multi-user.target
SERVICE

# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable dinerod
```

## 🚀 Launch & Verification

### 1. Start the Node

```bash
# Start Dinero node
sudo systemctl start dinerod

# Check status
sudo systemctl status dinerod

# Follow logs
sudo journalctl -u dinerod -f
```

### 2. Smoke Test

```bash
# Wait for nodeinfo.json
sleep 5

# Check nodeinfo
NODEINFO=/var/lib/dinero/nodeinfo.json
if [ -s "$NODEINFO" ]; then
    echo "✅ nodeinfo.json created:"
    sudo cat "$NODEINFO" | jq .
else
    echo "❌ nodeinfo.json missing or empty"
    exit 1
fi

# Test RPC endpoints
COOKIE="/var/lib/dinero/data/.cookie"
AUTH=$(sudo tr -d '\r\n' < "$COOKIE")
RPC=$(sudo jq -r .rpc "$NODEINFO")

echo "🧪 Testing RPC endpoints..."

# Test gethealth
echo "Testing gethealth..."
curl -s --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result.p2p'

# Test getblockchaininfo
echo "Testing getblockchaininfo..."
curl -s --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result | {blocks, headers, chain}'

# Test getpeers
echo "Testing getpeers..."
curl -s --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getpeers","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result | length'

echo "✅ Smoke test complete!"
```

## 🔒 Security Hardening

### 1. Firewall Rules

```bash
# Allow P2P port (if using fixed port)
# sudo ufw allow 20999/tcp comment "Dinero P2P"

# Block RPC/WS ports from external access
sudo ufw deny 20998/tcp comment "Dinero RPC - localhost only"
sudo ufw deny 20997/tcp comment "Dinero WS - localhost only"

# Enable firewall
sudo ufw --force enable
```

### 2. Reverse Proxy (Optional - Only if Remote RPC Access Needed)

```bash
# Install nginx
sudo apt update && sudo apt install -y nginx

# Create rate limiting config
sudo tee /etc/nginx/conf.d/dinero-rpc.conf >/dev/null <<'NGINX'
# Rate limiting for Dinero RPC
limit_req_zone $binary_remote_addr zone=dinero_rpc:10m rate=10r/s;

server {
    listen 443 ssl http2;
    server_name your-dinero-node.example.com;
    
    # SSL Configuration (replace with your certs)
    ssl_certificate /etc/ssl/certs/dinero-node.crt;
    ssl_certificate_key /etc/ssl/private/dinero-node.key;
    
    # Security headers
    add_header X-Frame-Options DENY;
    add_header X-Content-Type-Options nosniff;
    add_header X-XSS-Protection "1; mode=block";
    
    location / {
        # Rate limiting
        limit_req zone=dinero_rpc burst=20 nodelay;
        
        # Proxy to local RPC
        set $rpc_port $(cat /var/lib/dinero/nodeinfo.json | jq -r .rpc);
        proxy_pass http://127.0.0.1:$rpc_port;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header Connection "close";
        
        # Timeouts
        proxy_connect_timeout 5s;
        proxy_send_timeout 10s;
        proxy_read_timeout 30s;
    }
}
NGINX

sudo nginx -t && sudo systemctl reload nginx
```

## 📊 Monitoring & Operations

### 1. Health Check Script

```bash
# Create health check script
sudo tee /usr/local/bin/dinero-health >/dev/null <<'HEALTH'
#!/bin/bash
set -e

NODEINFO=/var/lib/dinero/nodeinfo.json
COOKIE=/var/lib/dinero/data/.cookie

if [ ! -s "$NODEINFO" ]; then
    echo "❌ nodeinfo.json missing"
    exit 1
fi

if [ ! -s "$COOKIE" ]; then
    echo "❌ cookie file missing"
    exit 1
fi

AUTH=$(tr -d '\r\n' < "$COOKIE")
RPC=$(jq -r .rpc "$NODEINFO")

# Test RPC connectivity
HEALTH=$(curl -s --max-time 5 --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq -r '.result // empty')

if [ -z "$HEALTH" ]; then
    echo "❌ RPC not responding"
    exit 1
fi

PEERS=$(echo "$HEALTH" | jq -r '.p2p.peers // 0')
BLOCKS=$(echo "$HEALTH" | jq -r '.p2p.blocks // 0')
HEADERS=$(echo "$HEALTH" | jq -r '.p2p.headers // 0')

echo "✅ Node healthy:"
echo "  Peers: $PEERS"
echo "  Blocks: $BLOCKS"
echo "  Headers: $HEADERS"
echo "  IBD: $([ $HEADERS -gt $BLOCKS ] && echo "true" || echo "false")"

exit 0
HEALTH

sudo chmod +x /usr/local/bin/dinero-health
```

### 2. Log Rotation

```bash
# Configure log rotation
sudo tee /etc/logrotate.d/dinerod >/dev/null <<'LOGROTATE'
/var/lib/dinero/logs/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    create 0640 dinero dinero
    postrotate
        systemctl reload dinerod
    endscript
}
LOGROTATE
```

### 3. Prometheus Metrics (Optional)

```bash
# Create simple metrics exporter
sudo tee /usr/local/bin/dinero-metrics >/dev/null <<'METRICS'
#!/bin/bash
# Simple Prometheus metrics exporter for Dinero

NODEINFO=/var/lib/dinero/nodeinfo.json
COOKIE=/var/lib/dinero/data/.cookie

if [ ! -s "$NODEINFO" ] || [ ! -s "$COOKIE" ]; then
    exit 1
fi

AUTH=$(tr -d '\r\n' < "$COOKIE")
RPC=$(jq -r .rpc "$NODEINFO")

HEALTH=$(curl -s --max-time 5 --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq -r '.result // empty')

if [ -n "$HEALTH" ]; then
    echo "# HELP dinero_peers Number of connected peers"
    echo "# TYPE dinero_peers gauge"
    echo "dinero_peers $(echo "$HEALTH" | jq -r '.p2p.peers // 0')"
    
    echo "# HELP dinero_blocks Current block height"
    echo "# TYPE dinero_blocks gauge"
    echo "dinero_blocks $(echo "$HEALTH" | jq -r '.p2p.blocks // 0')"
    
    echo "# HELP dinero_headers Current header height"
    echo "# TYPE dinero_headers gauge"
    echo "dinero_headers $(echo "$HEALTH" | jq -r '.p2p.headers // 0')"
    
    echo "# HELP dinero_inflight Blocks currently downloading"
    echo "# TYPE dinero_inflight gauge"
    echo "dinero_inflight $(echo "$HEALTH" | jq -r '.p2p.inflight // 0')"
    
    echo "# HELP dinero_queued Blocks queued for download"
    echo "# TYPE dinero_queued gauge"
    echo "dinero_queued $(echo "$HEALTH" | jq -r '.p2p.queued // 0')"
fi
METRICS

sudo chmod +x /usr/local/bin/dinero-metrics
```

## 🔧 Day-2 Operations

### Common Commands

```bash
# Check node status
sudo systemctl status dinerod
dinero-health

# View logs
sudo journalctl -u dinerod -f
sudo journalctl -u dinerod --since "1 hour ago"

# Restart node
sudo systemctl restart dinerod

# Stop node gracefully
sudo systemctl stop dinerod

# Check configuration
sudo dinerod -conf=/etc/dinero/dinero.conf -checkconfig

# Manual RPC calls
COOKIE="/var/lib/dinero/data/.cookie"
AUTH=$(sudo tr -d '\r\n' < "$COOKIE")
RPC=$(sudo jq -r .rpc /var/lib/dinero/nodeinfo.json)

curl -s --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq .
```

### Upgrade Process

```bash
# 1. Stop service
sudo systemctl stop dinerod

# 2. Backup current binary
sudo cp /usr/local/bin/dinerod /usr/local/bin/dinerod.backup

# 3. Install new binary
sudo cp ./build-debug/bin/dinerod /usr/local/bin/
sudo chmod +x /usr/local/bin/dinerod

# 4. Start service
sudo systemctl start dinerod

# 5. Verify
dinero-health
```

### Database Maintenance

```bash
# Switch to full sync after IBD (edit config)
sudo sed -i 's/sqlite_synchronous=normal/sqlite_synchronous=full/' /etc/dinero/dinero.conf
sudo systemctl restart dinerod

# Cold backup (stops service)
sudo systemctl stop dinerod
sudo tar -czf /backup/dinero-$(date +%Y%m%d).tar.gz -C /var/lib/dinero data
sudo systemctl start dinerod
```

## 🧪 Two-Node E2E Validation

### Host A (Mining Node)

```bash
# Get connection info
RPC_A=$(sudo jq -r .rpc /var/lib/dinero/nodeinfo.json)
P2P_A=$(sudo jq -r .p2p /var/lib/dinero/nodeinfo.json)
COOKIE_A="/var/lib/dinero/data/.cookie"
AUTH_A=$(sudo tr -d '\r\n' < "$COOKIE_A")

echo "Host A - RPC: $RPC_A, P2P: $P2P_A"

# Enable mining
curl -s --user "$AUTH_A" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"setmining","params":[true]}' \
  "http://127.0.0.1:${RPC_A}"

# Check mining status
curl -s --user "$AUTH_A" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getmininginfo","params":[]}' \
  "http://127.0.0.1:${RPC_A}" | jq '.result.mining_enabled'
```

### Host B (Syncing Node)

```bash
# Connect to Host A
HOST_A_IP="192.168.1.100"  # Replace with actual IP
P2P_A="20999"              # Replace with actual port

# Add to config or use CLI
echo "connect=$HOST_A_IP:$P2P_A" | sudo tee -a /etc/dinero/dinero.conf
sudo systemctl restart dinerod

# Monitor sync progress
watch 'dinero-health'
```

## 📈 Success Metrics

Your node is production-ready when:

- ✅ **Service**: `systemctl status dinerod` shows active (running)
- ✅ **Health**: `dinero-health` shows peers > 0, blocks advancing
- ✅ **Sync**: headers == blocks (IBD complete)
- ✅ **Security**: RPC only on localhost, firewall configured
- ✅ **Monitoring**: Logs clean, metrics available
- ✅ **Resilience**: Survives restart, handles network issues

## 🎉 Congratulations!

You now have a **production-grade Dinero node** with:

- 🔒 **Security**: Hardened systemd service, localhost-only RPC
- 📊 **Observability**: Health checks, metrics, structured logging  
- 🚀 **Performance**: Adaptive P2P, optimized defaults
- 🔧 **Operations**: Easy upgrades, backups, monitoring
- 🧪 **Testing**: Smoke tests, E2E validation

**Your node is ready for mainnet! Deploy with confidence.** 🚀⛏️
