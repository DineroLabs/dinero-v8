#!/bin/bash
# Dinero One-Liner Production Deploy
# Usage: curl -sSL https://raw.githubusercontent.com/dinero/dinero/main/scripts/deploy-oneliner.sh | sudo bash
# Or: sudo ./scripts/deploy-oneliner.sh

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[DINERO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check requirements
[[ $EUID -eq 0 ]] || error "Must run as root (use sudo)"
command -v systemctl >/dev/null || error "systemd required"
command -v jq >/dev/null || { log "Installing jq..."; apt-get update && apt-get install -y jq; }

# Find binary
BINARY=""
for path in "./build-debug/bin/dinerod" "./build/bin/dinerod" "/usr/local/bin/dinerod"; do
    [[ -x "$path" ]] && { BINARY="$path"; break; }
done
[[ -n "$BINARY" ]] || error "dinerod binary not found. Build first: cmake --build build-debug -j8 --target dinerod"

log "🚀 Deploying Dinero Production Node"
log "Binary: $BINARY"

# 1. Install binary
log "Installing binary..."
install -m 0755 "$BINARY" /usr/local/bin/dinerod

# 2. Create user & directories
log "Creating user and directories..."
useradd -r -m -d /var/lib/dinero -s /usr/sbin/nologin dinero 2>/dev/null || true
mkdir -p /var/lib/dinero/{data,logs} /etc/dinero
chown -R dinero:dinero /var/lib/dinero

# 3. Create production config
log "Creating configuration..."
cat > /etc/dinero/dinero.conf <<'CONF'
# Dinero Production Configuration
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
chown root:dinero /etc/dinero/dinero.conf
chmod 640 /etc/dinero/dinero.conf

# 4. Create systemd service
log "Creating systemd service..."
cat > /etc/systemd/system/dinerod.service <<'UNIT'
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

# 5. Create health check tool
log "Creating health check tool..."
cat > /usr/local/bin/dinero-health <<'HEALTH'
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
chmod +x /usr/local/bin/dinero-health

# 6. Start service
log "Starting Dinero service..."
systemctl daemon-reload
systemctl enable dinerod
systemctl start dinerod

# 7. Wait and verify
log "Waiting for startup..."
sleep 5

if systemctl is-active --quiet dinerod; then
    log "✅ Service started successfully"
else
    error "❌ Service failed to start. Check: journalctl -u dinerod"
fi

# 8. Wait for nodeinfo and run health check
log "Waiting for node initialization..."
for i in {1..30}; do
    if [[ -s /var/lib/dinero/nodeinfo.json ]]; then
        log "✅ Node initialized"
        break
    fi
    sleep 1
done

if [[ ! -s /var/lib/dinero/nodeinfo.json ]]; then
    warn "nodeinfo.json not created yet. Node may still be starting."
else
    log "Running health check..."
    if /usr/local/bin/dinero-health 2>/dev/null; then
        log "✅ Health check passed"
    else
        warn "Health check failed. Node may still be syncing."
    fi
fi

# 9. Show status and next steps
echo ""
echo -e "${BLUE}🎉 Dinero Node Deployed Successfully!${NC}"
echo "=================================="
echo ""
echo "📊 Status:"
systemctl status dinerod --no-pager -l | head -10
echo ""
echo "📋 Node Info:"
[[ -s /var/lib/dinero/nodeinfo.json ]] && cat /var/lib/dinero/nodeinfo.json | jq . || echo "Still initializing..."
echo ""
echo "🔧 Useful Commands:"
echo "  systemctl status dinerod          # Check service status"
echo "  journalctl -u dinerod -f          # Follow logs"
echo "  dinero-health                     # Check node health"
echo "  systemctl restart dinerod         # Restart service"
echo ""
echo "🔒 Security Notes:"
echo "  • RPC is localhost-only (127.0.0.1)"
echo "  • Configure firewall: ufw allow 20999/tcp (P2P only)"
echo "  • Use reverse proxy for remote RPC access"
echo ""
echo -e "${GREEN}🚀 Your Dinero node is ready for production!${NC}"
echo ""
