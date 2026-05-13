#!/bin/bash
# Dinero Server Update Script
# Deploys all recent changes to production Linux servers
# Usage: ./deploy_updates.sh <server_name> [server_ip]

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
REPO_DIR="${REPO_DIR:-$HOME/DineroCoin}"
SERVER_NAME="${1:-Unknown}"
SERVER_IP="${2:-auto}"
SKIP_BUILD="${SKIP_BUILD:-0}"

# Functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Banner
echo ""
echo "======================================"
echo "  Dinero Server Update Script"
echo "======================================"
echo ""

# Validate inputs
if [ "$SERVER_NAME" = "Unknown" ]; then
    log_error "Please provide server name: ./deploy_updates.sh <server_name> [server_ip]"
    log_info "Example: ./deploy_updates.sh Virginia 173.249.195.59"
    exit 1
fi

# Auto-detect IP if not provided
if [ "$SERVER_IP" = "auto" ]; then
    log_info "Auto-detecting server IP..."
    SERVER_IP=$(curl -s ifconfig.me 2>/dev/null || echo "127.0.0.1")
fi

log_info "Server Name: $SERVER_NAME"
log_info "Server IP: $SERVER_IP"
log_info "Repository: $REPO_DIR"
echo ""

# Check if repo exists
if [ ! -d "$REPO_DIR" ]; then
    log_error "Repository not found at: $REPO_DIR"
    log_info "Please clone first: git clone https://github.com/dinerocoin/dinero.git $REPO_DIR"
    exit 1
fi

# Step 1: Backup current installation
echo ""
log_info "Step 1: Backing up current installation..."
BACKUP_DIR="$HOME/dinero_backup_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$BACKUP_DIR"

if [ -d "$REPO_DIR" ]; then
    cp -r "$REPO_DIR" "$BACKUP_DIR/" 2>/dev/null && \
        log_success "Backup created: $BACKUP_DIR" || \
        log_warn "Could not backup (first install?)"
fi

# Backup config
if [ -f "$HOME/.dinero/dinero.conf" ]; then
    cp "$HOME/.dinero/dinero.conf" "$HOME/.dinero/dinero.conf.backup.$(date +%Y%m%d_%H%M%S)"
    log_success "Config backed up"
fi

# Step 2: Pull latest code
echo ""
log_info "Step 2: Pulling latest code from repository..."
cd "$REPO_DIR"

# Stash any local changes
git stash save "Auto-stash before update $(date)" 2>/dev/null || true

# Pull latest
if git pull origin master; then
    log_success "Code updated successfully"
else
    log_error "Git pull failed. Check network/credentials."
    log_info "Attempting to continue with existing code..."
fi

# Show what changed
log_info "Recent commits:"
git log --oneline -5 || true

# Step 3: Rebuild daemon
if [ "$SKIP_BUILD" = "0" ]; then
    echo ""
    log_info "Step 3: Rebuilding daemon..."
    cd "$REPO_DIR"

    # Clean previous build
    make clean 2>/dev/null || true

    # Check if we need to run autogen
    if [ ! -f "configure" ]; then
        log_info "Running autogen.sh..."
        ./autogen.sh || {
            log_error "autogen.sh failed"
            exit 1
        }
    fi

    # Configure
    log_info "Running configure..."
    ./configure || {
        log_error "configure failed"
        exit 1
    }

    # Build
    log_info "Building (this may take a few minutes)..."
    make -j$(nproc) || {
        log_error "Build failed"
        exit 1
    }

    log_success "Build completed successfully"
else
    log_warn "Skipping build (SKIP_BUILD=1)"
fi

# Step 4: Stop running daemon
echo ""
log_info "Step 4: Stopping current daemon..."
if [ -f "$REPO_DIR/build/dinero-cli" ]; then
    $REPO_DIR/build/dinero-cli stop 2>/dev/null && \
        log_success "Daemon stopped" || \
        log_warn "Daemon was not running"
    sleep 5
else
    log_warn "dinero-cli not found, skipping stop"
fi

# Step 5: Update dinero.conf
echo ""
log_info "Step 5: Updating dinero.conf..."
CONF_FILE="$HOME/.dinero/dinero.conf"
CONF_DIR="$HOME/.dinero"

# Create config directory if needed
mkdir -p "$CONF_DIR"

# Check if HTTP server settings already exist
if [ -f "$CONF_FILE" ] && grep -q "httpserver" "$CONF_FILE"; then
    log_warn "HTTP server settings already exist in config"
else
    log_info "Adding new configuration options..."

    cat >> "$CONF_FILE" <<EOF

# === HTTP Server Settings (Added $(date +%Y-%m-%d)) ===
httpserver=1
httpserverport=21999
httpcors=*

# === Server Info Settings ===
serverinfo.enabled=1
serverinfo.name=$SERVER_NAME
serverinfo.network=mainnet

# === External IP ===
externalip=$SERVER_IP

# === Registry Settings (Uncomment to enable auto-registration) ===
# registry=1
# registryurl=http://173.249.195.59:8080/api/register

EOF

    log_success "Configuration updated"
fi

# Step 6: Restart daemon
echo ""
log_info "Step 6: Starting updated daemon..."
cd "$REPO_DIR"

if [ -f "build/dinerod" ]; then
    ./build/dinerod -daemon && \
        log_success "Daemon started" || {
        log_error "Failed to start daemon"
        exit 1
    }
else
    log_error "dinerod binary not found at build/dinerod"
    exit 1
fi

log_info "Waiting for daemon to initialize (15 seconds)..."
sleep 15

# Step 7: Run tests
echo ""
log_info "Step 7: Running verification tests..."
echo ""

# Test 1: RPC
log_info "Test 1/5: RPC Server"
if $REPO_DIR/build/dinero-cli getblockcount >/dev/null 2>&1; then
    log_success "RPC is working"
else
    log_error "RPC test failed"
fi

# Test 2: HTTP Server (local)
log_info "Test 2/5: HTTP Server (localhost)"
if curl -sf http://localhost:21999/ >/dev/null 2>&1; then
    log_success "HTTP server is running"
else
    log_error "HTTP server test failed"
fi

# Test 3: serverinfo.json (local)
log_info "Test 3/5: serverinfo.json endpoint (localhost)"
if curl -sf http://localhost:21999/serverinfo.json >/dev/null 2>&1; then
    log_success "serverinfo.json is accessible"
    echo ""
    log_info "Server info:"
    curl -s http://localhost:21999/serverinfo.json | python3 -m json.tool 2>/dev/null || \
        curl -s http://localhost:21999/serverinfo.json
    echo ""
else
    log_error "serverinfo.json test failed"
fi

# Test 4: External access
log_info "Test 4/5: External access (${SERVER_IP}:21999)"
if curl -sf --max-time 5 http://$SERVER_IP:21999/serverinfo.json >/dev/null 2>&1; then
    log_success "External access is working"
else
    log_warn "External access test failed (may need firewall configuration)"
    log_info "To allow external access, run:"
    log_info "  sudo ufw allow 21999/tcp"
fi

# Test 5: CORS headers
log_info "Test 5/5: CORS headers"
if curl -sI http://localhost:21999/serverinfo.json | grep -q "Access-Control-Allow-Origin"; then
    log_success "CORS headers are present"
else
    log_warn "CORS headers not found"
fi

# Step 8: Display current status
echo ""
log_info "Step 8: Current daemon status"
echo ""
echo "======================================"
$REPO_DIR/build/dinero-cli getinfo 2>/dev/null || log_warn "Daemon still starting up..."
echo "======================================"
echo ""

# Summary
echo ""
echo "======================================"
log_success "Deployment Complete!"
echo "======================================"
echo ""
echo "📋 Summary:"
echo "  Server Name: $SERVER_NAME"
echo "  Server IP: $SERVER_IP"
echo "  Backup: $BACKUP_DIR"
echo "  Config: $CONF_FILE"
echo ""
echo "🔗 Quick Links:"
echo "  Local HTTP: http://localhost:21999/"
echo "  serverinfo: http://localhost:21999/serverinfo.json"
echo "  External: http://$SERVER_IP:21999/serverinfo.json"
echo ""
echo "📊 Next Steps:"
echo "  1. Verify daemon is syncing: $REPO_DIR/build/dinero-cli getinfo"
echo "  2. Check logs: tail -f ~/.dinero/debug.log"
echo "  3. Test externally: curl http://$SERVER_IP:21999/serverinfo.json"
echo "  4. Configure firewall (if needed): sudo ufw allow 21999/tcp"
echo ""

# If this is Virginia, offer to install registry
if [[ "$SERVER_NAME" =~ ^[Vv]irginia$ ]] || [[ "$SERVER_IP" == "173.249.195.59" ]]; then
    echo ""
    log_info "🌐 Registry Server Deployment"
    echo ""
    echo "This appears to be the Virginia server."
    echo "Would you like to deploy the global node registry? (y/N)"
    read -r response

    if [[ "$response" =~ ^[Yy]$ ]]; then
        echo ""
        log_info "Deploying registry server..."

        # Install Python dependencies
        log_info "Installing Python dependencies..."
        pip3 install requests || {
            log_error "Failed to install Python dependencies"
            exit 1
        }

        # Make scripts executable
        chmod +x "$REPO_DIR/registry/dinero_registry.py"
        chmod +x "$REPO_DIR/registry/dinero_registry_extended.py"

        # Copy systemd service
        if [ -d "/etc/systemd/system" ]; then
            log_info "Installing systemd service..."
            sudo cp "$REPO_DIR/registry/dinero-registry.service" /etc/systemd/system/

            # Update paths in service file
            sudo sed -i "s|/opt/dinero/registry|$REPO_DIR/registry|g" /etc/systemd/system/dinero-registry.service
            sudo sed -i "s|User=dinero|User=$USER|g" /etc/systemd/system/dinero-registry.service
            sudo sed -i "s|Group=dinero|Group=$USER|g" /etc/systemd/system/dinero-registry.service

            # Reload systemd
            sudo systemctl daemon-reload

            # Enable and start service
            sudo systemctl enable dinero-registry
            sudo systemctl start dinero-registry

            # Check status
            sleep 2
            if sudo systemctl is-active --quiet dinero-registry; then
                log_success "Registry service started successfully"
                echo ""
                log_info "Registry URLs:"
                echo "  Dashboard: http://localhost:8080/"
                echo "  API: http://localhost:8080/nodes.json"
                echo "  External: http://$SERVER_IP:8080/nodes.json"
                echo ""
                log_info "View logs: sudo journalctl -u dinero-registry -f"
            else
                log_error "Registry service failed to start"
                log_info "Check logs: sudo journalctl -u dinero-registry -xe"
            fi
        else
            log_warn "systemd not available, manual start required"
            log_info "To start registry manually:"
            echo "  cd $REPO_DIR/registry"
            echo "  ./dinero_registry_extended.py"
        fi
    fi
fi

echo ""
log_success "All done! 🚀"
echo ""

# Save deployment info
DEPLOY_LOG="$HOME/.dinero/deployment.log"
cat >> "$DEPLOY_LOG" <<EOF
=== Deployment: $(date) ===
Server: $SERVER_NAME ($SERVER_IP)
Backup: $BACKUP_DIR
Status: Success
EOF

log_info "Deployment logged to: $DEPLOY_LOG"
