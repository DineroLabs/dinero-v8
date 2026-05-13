#!/bin/bash
set -e

# Dinero Production Deployment Script
# Automates the complete production deployment process

echo "🚀 Dinero Production Deployment"
echo "==============================="

# Configuration
DINERO_USER="dinero"
DINERO_HOME="/var/lib/dinero"
CONFIG_DIR="/etc/dinero"
BINARY_PATH="/usr/local/bin/dinerod"
SERVICE_NAME="dinerod"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (use sudo)"
        exit 1
    fi
}

check_binary() {
    if [[ ! -f "./build-debug/bin/dinerod" ]]; then
        log_error "dinerod binary not found. Please build first:"
        echo "  cmake --build build-debug -j8 --target dinerod"
        exit 1
    fi
}

create_user() {
    log_info "Creating dinero user and directories..."
    
    # Create user
    if ! id "$DINERO_USER" &>/dev/null; then
        useradd -r -m -d "$DINERO_HOME" -s /usr/sbin/nologin "$DINERO_USER"
        log_info "Created user: $DINERO_USER"
    else
        log_warn "User $DINERO_USER already exists"
    fi
    
    # Create directories
    mkdir -p "$DINERO_HOME"/{data,logs}
    mkdir -p "$CONFIG_DIR"
    chown -R "$DINERO_USER:$DINERO_USER" "$DINERO_HOME"
    
    log_info "Created directories in $DINERO_HOME"
}

install_binary() {
    log_info "Installing dinerod binary..."
    
    # Backup existing binary
    if [[ -f "$BINARY_PATH" ]]; then
        cp "$BINARY_PATH" "${BINARY_PATH}.backup.$(date +%Y%m%d-%H%M%S)"
        log_info "Backed up existing binary"
    fi
    
    # Install new binary
    cp "./build-debug/bin/dinerod" "$BINARY_PATH"
    chmod +x "$BINARY_PATH"
    chown root:root "$BINARY_PATH"
    
    log_info "Installed binary to $BINARY_PATH"
}

create_config() {
    log_info "Creating production configuration..."
    
    cat > "$CONFIG_DIR/dinero.conf" <<'EOF'
# Dinero Production Configuration
datadir=/var/lib/dinero/data
nodeinfo=/var/lib/dinero/nodeinfo.json

# Network Security (localhost only)
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=0
wsport=0

# P2P Settings
p2p=1
port=0
maxpeers=16
timeout=30

# Performance & DoS Protection
blockmaxinflight=128
perpeermax=16
rpcbatchlimit=100

# Database
sqlite_wal=1
sqlite_synchronous=normal

# Wallet
autowallet=default

# Logging
debug=p2p,headers,blocks
logtimestamp=1
printtoconsole=1
EOF
    
    chown root:$DINERO_USER "$CONFIG_DIR/dinero.conf"
    chmod 640 "$CONFIG_DIR/dinero.conf"
    
    log_info "Created configuration at $CONFIG_DIR/dinero.conf"
}

create_service() {
    log_info "Creating systemd service..."
    
    cat > "/etc/systemd/system/$SERVICE_NAME.service" <<EOF
[Unit]
Description=Dinero Cryptocurrency Node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$DINERO_USER
Group=$DINERO_USER
ExecStart=$BINARY_PATH -server=1 -daemon=0 -printtoconsole -conf=$CONFIG_DIR/dinero.conf
WorkingDirectory=$DINERO_HOME
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
EOF
    
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME"
    
    log_info "Created and enabled systemd service"
}

create_tools() {
    log_info "Creating operational tools..."
    
    # Health check script
    cat > "/usr/local/bin/dinero-health" <<'EOF'
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
EOF
    
    chmod +x "/usr/local/bin/dinero-health"
    
    log_info "Created health check tool: dinero-health"
}

start_service() {
    log_info "Starting Dinero service..."
    
    systemctl start "$SERVICE_NAME"
    sleep 3
    
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        log_info "✅ Service started successfully"
    else
        log_error "❌ Service failed to start"
        systemctl status "$SERVICE_NAME"
        exit 1
    fi
}

run_smoke_test() {
    log_info "Running smoke test..."
    
    # Wait for nodeinfo
    local retries=20
    while [[ $retries -gt 0 ]]; do
        if [[ -s "$DINERO_HOME/nodeinfo.json" ]]; then
            break
        fi
        sleep 1
        ((retries--))
    done
    
    if [[ ! -s "$DINERO_HOME/nodeinfo.json" ]]; then
        log_error "nodeinfo.json not created"
        return 1
    fi
    
    # Run health check
    if /usr/local/bin/dinero-health; then
        log_info "✅ Smoke test passed"
    else
        log_error "❌ Smoke test failed"
        return 1
    fi
}

show_status() {
    echo ""
    echo "🎉 Deployment Complete!"
    echo "======================"
    echo ""
    echo "Service Status:"
    systemctl status "$SERVICE_NAME" --no-pager -l
    echo ""
    echo "Node Info:"
    if [[ -s "$DINERO_HOME/nodeinfo.json" ]]; then
        cat "$DINERO_HOME/nodeinfo.json" | jq .
    fi
    echo ""
    echo "Useful Commands:"
    echo "  sudo systemctl status $SERVICE_NAME    # Check service status"
    echo "  sudo journalctl -u $SERVICE_NAME -f   # Follow logs"
    echo "  dinero-health                          # Check node health"
    echo "  sudo systemctl restart $SERVICE_NAME  # Restart service"
    echo ""
    echo "🚀 Your Dinero node is ready for production!"
}

main() {
    log_info "Starting deployment process..."
    
    check_root
    check_binary
    
    create_user
    install_binary
    create_config
    create_service
    create_tools
    start_service
    
    sleep 5  # Give service time to start
    
    if run_smoke_test; then
        show_status
    else
        log_error "Deployment completed but smoke test failed"
        log_info "Check logs: sudo journalctl -u $SERVICE_NAME -f"
        exit 1
    fi
}

# Handle script arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [--help]"
        echo ""
        echo "Deploys Dinero node for production use."
        echo "Requires: sudo privileges and built dinerod binary"
        exit 0
        ;;
    *)
        main "$@"
        ;;
esac
