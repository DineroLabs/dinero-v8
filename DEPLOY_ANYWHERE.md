# 🚀 Deploy Dinero Anywhere - Copy-Paste Commands

**Production-ready Dinero node deployment in under 5 minutes.**

## ⚡ Quick Deploy Options

### 🐧 Linux Production (systemd)

```bash
# 1) Install binary
sudo install -m 0755 build-debug/bin/dinerod /usr/local/bin/dinerod

# 2) Create user, dirs & config
sudo useradd -r -m -d /var/lib/dinero -s /usr/sbin/nologin dinero || true
sudo mkdir -p /var/lib/dinero/{data,logs} /etc/dinero
sudo chown -R dinero:dinero /var/lib/dinero
sudo tee /etc/dinero/dinero.conf >/dev/null <<'CONF'
datadir=/var/lib/dinero/data
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=0
wsport=0
p2p=1
port=0
autowallet=default
maxpeers=16
timeout=30
blockmaxinflight=128
perpeermax=16
sqlite_wal=1
sqlite_synchronous=normal
debug=p2p,headers,blocks,scheduler
logtimestamp=1
CONF

# 3) Create systemd service
sudo tee /etc/systemd/system/dinerod.service >/dev/null <<'UNIT'
[Unit]
Description=Dinero Cryptocurrency Node
After=network-online.target
Wants=network-online.target

[Service]
User=dinero
Group=dinero
ExecStart=/usr/local/bin/dinerod -server=1 -daemon=0 -printtoconsole \
  -nodeinfo=/var/lib/dinero/nodeinfo.json -conf=/etc/dinero/dinero.conf
WorkingDirectory=/var/lib/dinero
Restart=on-failure
RestartSec=3
LimitNOFILE=65535
Nice=5
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
UNIT

# 4) Start & enable
sudo systemctl daemon-reload
sudo systemctl enable --now dinerod

# 5) Verify
sudo systemctl status dinerod
```

### 🍎 macOS Development

```bash
# Quick start with auto-ports
DATADIR="$(mktemp -d -t din-prod.XXXX)"
./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
  -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
  -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default \
  -nodeinfo="$DATADIR/nodeinfo.json" -datadir="$DATADIR"
```

### 🐳 Docker Production

```yaml
# docker-compose.yml
version: '3.8'
services:
  dinerod:
    image: dinero/dinerod:latest
    restart: unless-stopped
    user: "1000:1000"
    command:
      - -server=1
      - -daemon=0 
      - -printtoconsole
      - -rpcbind=127.0.0.1
      - -rpcallowip=127.0.0.1
      - -rpcport=0
      - -wsport=0
      - -p2p
      - -port=0
      - -autowallet=default
      - -nodeinfo=/data/nodeinfo.json
      - -conf=/data/dinero.conf
    volumes:
      - ./data:/data
    network_mode: "host"
    healthcheck:
      test: ["CMD", "/usr/local/bin/healthcheck"]
      interval: 30s
      timeout: 10s
      retries: 3
```

```bash
# Deploy
docker compose up -d

# Check status
docker compose ps
docker compose logs -f dinerod
```

## ✅ Post-Deploy Verification

### Health Check Commands

```bash
# Linux systemd
systemctl status dinerod
journalctl -u dinerod -f

# Get node info
NODEINFO=/var/lib/dinero/nodeinfo.json  # Linux
# NODEINFO="$DATADIR/nodeinfo.json"     # macOS (use your temp dir)

cat "$NODEINFO" | jq .

# Test RPC endpoints
RPC=$(jq -r .rpc "$NODEINFO")
COOKIE=/var/lib/dinero/data/.cookie      # Linux
# COOKIE="$DATADIR/.cookie"              # macOS
AUTH=$(tr -d '\r\n' < "$COOKIE")

# Health check
curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result.p2p'

# Blockchain info
curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result | {blocks, headers, chain}'

# Peer info
curl -s --user "$AUTH" -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"getpeers","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq '.result | length'
```

### 🟢 Success Indicators

**Healthy Node Shows:**
- ✅ `gethealth.p2p.peers ≥ 1`
- ✅ `getblockchaininfo.headers ≥ blocks` (during IBD) → equal at steady state
- ✅ `.cookie` file exists with 0600 perms
- ✅ Logs show steady header/block progress
- ✅ No repeated timeout/ban messages

**Example Healthy Response:**
```json
{
  "p2p": {
    "peers": 3,
    "outbound": 3,
    "inbound": 0,
    "headers": 12350,
    "blocks": 12345,
    "inflight": 16,
    "queued": 24,
    "rate_bps": 2048000,
    "initialblockdownload": true
  }
}
```

## 🔒 Security Hardening

### Firewall (Ubuntu UFW)

```bash
sudo ufw default deny incoming
sudo ufw allow 22/tcp                    # SSH
sudo ufw allow 20999/tcp                 # P2P (only if accepting inbound)
# Block RPC/WS from external access (they're localhost-only by default)
sudo ufw enable
```

### Reverse Proxy (Optional - Nginx)

```nginx
# /etc/nginx/sites-available/dinero-rpc
limit_req_zone $binary_remote_addr zone=dinero_rpc:10m rate=10r/s;

server {
    listen 443 ssl http2;
    server_name your-node.example.com;
    
    # SSL config
    ssl_certificate /etc/ssl/certs/dinero.crt;
    ssl_certificate_key /etc/ssl/private/dinero.key;
    
    location / {
        limit_req zone=dinero_rpc burst=20 nodelay;
        
        # Proxy to local RPC (get port from nodeinfo.json)
        proxy_pass http://127.0.0.1:20998;  # or dynamic port
        proxy_set_header Connection "close";
        proxy_connect_timeout 5s;
        proxy_send_timeout 10s;
        proxy_read_timeout 30s;
    }
}
```

## 📊 Monitoring & Metrics

### Health Check Script

```bash
# Create health checker
sudo tee /usr/local/bin/dinero-health >/dev/null <<'HEALTH'
#!/bin/bash
NODEINFO=/var/lib/dinero/nodeinfo.json
COOKIE=/var/lib/dinero/data/.cookie

[ -s "$NODEINFO" ] || { echo "❌ nodeinfo.json missing"; exit 1; }
[ -s "$COOKIE" ] || { echo "❌ cookie missing"; exit 1; }

AUTH=$(tr -d '\r\n' < "$COOKIE")
RPC=$(jq -r .rpc "$NODEINFO")

HEALTH=$(curl -s --max-time 5 --user "$AUTH" \
  -H 'content-type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
  "http://127.0.0.1:${RPC}" | jq -r '.result // empty')

[ -n "$HEALTH" ] || { echo "❌ RPC not responding"; exit 1; }

PEERS=$(echo "$HEALTH" | jq -r '.p2p.peers // 0')
BLOCKS=$(echo "$HEALTH" | jq -r '.p2p.blocks // 0')  
HEADERS=$(echo "$HEALTH" | jq -r '.p2p.headers // 0')

echo "✅ Node healthy:"
echo "  Peers: $PEERS"
echo "  Blocks: $BLOCKS" 
echo "  Headers: $HEADERS"
echo "  IBD: $([ $HEADERS -gt $BLOCKS ] && echo "true" || echo "false")"
HEALTH

sudo chmod +x /usr/local/bin/dinero-health

# Usage
dinero-health
```

### Prometheus Metrics

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'dinero'
    static_configs:
      - targets: ['127.0.0.1:9100']  # if using prometheus exporter
    scrape_interval: 30s
```

## 🧪 Two-Node E2E Test

```bash
# Node A (mining)
A_DIR=$(mktemp -d -t dinA.XXXX)
./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
  -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
  -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default -gen \
  -nodeinfo="$A_DIR/nodeinfo.json" -datadir="$A_DIR" &
PID_A=$!

sleep 3

# Node B (syncing)
P2P_A=$(jq -r .p2p "$A_DIR/nodeinfo.json")
B_DIR=$(mktemp -d -t dinB.XXXX)
./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
  -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
  -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default \
  -connect=127.0.0.1:$P2P_A \
  -nodeinfo="$B_DIR/nodeinfo.json" -datadir="$B_DIR" &
PID_B=$!

echo "Node A P2P: $P2P_A"
echo "Node A RPC: $(jq -r .rpc "$A_DIR/nodeinfo.json")"
echo "Node B RPC: $(jq -r .rpc "$B_DIR/nodeinfo.json")"

# Watch sync progress
echo "Watching Node B sync..."
RPC_B=$(jq -r .rpc "$B_DIR/nodeinfo.json")
COOKIE_B="$B_DIR/.cookie"

for i in {1..30}; do
    if [ -s "$COOKIE_B" ]; then
        AUTH_B=$(tr -d '\r\n' < "$COOKIE_B")
        BLOCKS=$(curl -s --user "$AUTH_B" \
          -H 'content-type: application/json' \
          --data '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}' \
          "http://127.0.0.1:${RPC_B}" | jq -r '.result.blocks // 0')
        echo "Node B blocks: $BLOCKS"
        [ "$BLOCKS" -ge 5 ] && { echo "✅ Sync successful!"; break; }
    fi
    sleep 2
done

# Cleanup
kill $PID_A $PID_B 2>/dev/null || true
```

## 🔧 Operations Commands

### Daily Operations

```bash
# Check status
systemctl status dinerod
dinero-health

# View logs  
journalctl -u dinerod -f
journalctl -u dinerod --since "1 hour ago"

# Restart
sudo systemctl restart dinerod

# Stop gracefully
sudo systemctl stop dinerod
```

### Maintenance

```bash
# Switch to full sync after IBD
sudo sed -i 's/sqlite_synchronous=normal/sqlite_synchronous=full/' /etc/dinero/dinero.conf
sudo systemctl restart dinerod

# Cold backup
sudo systemctl stop dinerod
sudo tar -czf /backup/dinero-$(date +%Y%m%d).tar.gz -C /var/lib/dinero data
sudo systemctl start dinerod

# Upgrade binary
sudo systemctl stop dinerod
sudo cp build-debug/bin/dinerod /usr/local/bin/dinerod
sudo systemctl start dinerod
dinero-health
```

## 🎯 One-Command Deploy

**Use the automated script:**

```bash
# Complete automated deployment
sudo ./scripts/deploy-production.sh
```

**Or Docker:**

```bash
# Container deployment
docker compose -f docker-compose.production.yml up -d
```

## 🏆 Success Checklist

Your node is production-ready when:

- ✅ **Service**: `systemctl status dinerod` shows active (running)
- ✅ **Health**: `dinero-health` shows peers > 0, blocks advancing  
- ✅ **Sync**: headers == blocks (IBD complete)
- ✅ **Security**: RPC localhost-only, firewall configured
- ✅ **Monitoring**: Clean logs, metrics available
- ✅ **Resilience**: Survives restart, handles network issues

## 🚀 Deploy Now!

**Pick your platform and run the commands above. Your Dinero node will be live in under 5 minutes!**

**Ready for mainnet. Ready for production. Ready to mine!** ⛏️🔥
