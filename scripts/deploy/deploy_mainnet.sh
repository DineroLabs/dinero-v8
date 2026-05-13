#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO MAINNET DEPLOYMENT SCRIPT
# ==============================================================================
#
# This script deploys a production Dinero node with full monitoring,
# security hardening, and operational best practices.
#
# Usage: ./deploy_mainnet.sh [--seed|--miner|--full]
#
# Node Types:
#   --seed   : Public seed node (P2P + monitoring)
#   --miner  : Mining node (private, optimized for mining)
#   --full   : Full node (all services, recommended)
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Default configuration
NODE_TYPE="full"
DINERO_USER="dinerod"
DINERO_GROUP="dinerod"
DINERO_HOME="/var/lib/dinerod"
DINERO_CONFIG="/etc/dinero"
DINERO_LOGS="/var/log/dinerod"
DINERO_BIN="/usr/local/bin"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --seed)
            NODE_TYPE="seed"
            shift
            ;;
        --miner)
            NODE_TYPE="miner"
            shift
            ;;
        --full)
            NODE_TYPE="full"
            shift
            ;;
        --help)
            echo "Usage: $0 [--seed|--miner|--full]"
            echo ""
            echo "Node Types:"
            echo "  --seed   : Public seed node (P2P + monitoring)"
            echo "  --miner  : Mining node (private, optimized for mining)"
            echo "  --full   : Full node (all services, recommended)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo "🚀 DINERO MAINNET DEPLOYMENT"
echo "================================="
echo "Node Type: $NODE_TYPE"
echo "Timestamp: $(date)"
echo ""

# Require root for system setup
if [[ $EUID -ne 0 ]]; then
   echo "❌ This script must be run as root for system setup"
   echo "Usage: sudo $0 [options]"
   exit 1
fi

# =============================================================================
# STEP 1: SYSTEM PREPARATION
# =============================================================================

echo "🔧 Step 1: System Preparation"
echo "-----------------------------"

# Update system
echo "Updating system packages..."
if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq
    apt-get install -y curl wget jq htop iotop build-essential cmake git
elif command -v yum >/dev/null 2>&1; then
    yum update -y -q
    yum install -y curl wget jq htop iotop gcc-c++ cmake3 git
else
    echo "⚠️  Unknown package manager. Please install dependencies manually."
fi

# Create dinero user and directories
echo "Creating dinero user and directories..."
if ! id "$DINERO_USER" >/dev/null 2>&1; then
    useradd -r -s /bin/false -d "$DINERO_HOME" "$DINERO_USER"
fi

mkdir -p "$DINERO_HOME" "$DINERO_CONFIG" "$DINERO_LOGS" "$DINERO_BIN"
chown -R "$DINERO_USER:$DINERO_GROUP" "$DINERO_HOME" "$DINERO_LOGS"
chmod 755 "$DINERO_HOME" "$DINERO_CONFIG"
chmod 750 "$DINERO_LOGS"

echo "✅ System preparation complete"
echo ""

# =============================================================================
# STEP 2: BUILD AND INSTALL BINARIES
# =============================================================================

echo "🔨 Step 2: Build and Install Binaries"
echo "-------------------------------------"

cd "$PROJECT_ROOT"

# Build Release version
echo "Building Dinero (Release + ASAN)..."
cmake -S . -B build-release \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SANITIZERS=OFF \
    -DCMAKE_INSTALL_PREFIX="$DINERO_BIN"

cmake --build build-release -j$(nproc)

# Build Debug+ASAN version for testing
echo "Building Dinero (Debug + ASAN)..."
cmake -S . -B build-debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_SANITIZERS=ON

cmake --build build-debug -j$(nproc)

# Install binaries
echo "Installing binaries..."
install -m 755 build-release/bin/dinerod "$DINERO_BIN/"
install -m 755 build-release/bin/dinero-cli "$DINERO_BIN/"

# Create debug symlinks
ln -sf build-debug/bin/dinerod "$DINERO_BIN/dinerod-debug"
ln -sf build-debug/bin/dinero-cli "$DINERO_BIN/dinero-cli-debug"

echo "✅ Binaries installed to $DINERO_BIN"
echo ""

# =============================================================================
# STEP 3: CONFIGURATION
# =============================================================================

echo "⚙️  Step 3: Configuration"
echo "------------------------"

# Copy base configuration
cp "$SCRIPT_DIR/nodeinfo-mainnet.json" "$DINERO_CONFIG/nodeinfo.json"

# Customize configuration based on node type
case $NODE_TYPE in
    seed)
        echo "Configuring as seed node..."
        # Enable P2P, disable mining
        jq '.p2p.enabled = true | .mining.enabled = false | .p2p.max_peers = 128' \
            "$DINERO_CONFIG/nodeinfo.json" > "$DINERO_CONFIG/nodeinfo.tmp"
        mv "$DINERO_CONFIG/nodeinfo.tmp" "$DINERO_CONFIG/nodeinfo.json"
        ;;
    miner)
        echo "Configuring as mining node..."
        # Enable mining, limit P2P
        jq '.mining.enabled = true | .mining.threads = 0 | .p2p.max_peers = 32' \
            "$DINERO_CONFIG/nodeinfo.json" > "$DINERO_CONFIG/nodeinfo.tmp"
        mv "$DINERO_CONFIG/nodeinfo.tmp" "$DINERO_CONFIG/nodeinfo.json"
        
        echo ""
        echo "⚠️  MINING SETUP REQUIRED:"
        echo "1. Generate a mining address:"
        echo "   $DINERO_BIN/dinero-cli getnewaddress"
        echo "2. Set mining address in config:"
        echo "   jq '.mining.address = \"din1...\"' $DINERO_CONFIG/nodeinfo.json"
        echo ""
        ;;
    full)
        echo "Configuring as full node..."
        # Default configuration is already suitable
        ;;
esac

# Set proper permissions
chown root:$DINERO_GROUP "$DINERO_CONFIG/nodeinfo.json"
chmod 640 "$DINERO_CONFIG/nodeinfo.json"

echo "✅ Configuration installed"
echo ""

# =============================================================================
# STEP 4: SYSTEMD SERVICE
# =============================================================================

echo "🔄 Step 4: Systemd Service"
echo "-------------------------"

# Install systemd service
cp "$SCRIPT_DIR/dinerod.service" /etc/systemd/system/
systemctl daemon-reload

# Enable but don't start yet
systemctl enable dinerod

echo "✅ Systemd service installed and enabled"
echo ""

# =============================================================================
# STEP 5: MONITORING SETUP
# =============================================================================

echo "📊 Step 5: Monitoring Setup"
echo "---------------------------"

# Check if Prometheus is available
if command -v prometheus >/dev/null 2>&1; then
    echo "Prometheus detected, installing configuration..."
    
    PROMETHEUS_CONFIG="/etc/prometheus"
    mkdir -p "$PROMETHEUS_CONFIG"
    
    cp "$SCRIPT_DIR/prometheus.yml" "$PROMETHEUS_CONFIG/"
    cp "$SCRIPT_DIR/dinero_alerts.yml" "$PROMETHEUS_CONFIG/"
    
    # Restart Prometheus if running
    if systemctl is-active prometheus >/dev/null 2>&1; then
        systemctl reload prometheus
        echo "✅ Prometheus configuration updated"
    else
        echo "⚠️  Prometheus not running. Start with: systemctl start prometheus"
    fi
else
    echo "⚠️  Prometheus not installed. Monitoring configuration skipped."
    echo "Install Prometheus for full monitoring capabilities."
fi

echo ""

# =============================================================================
# STEP 6: SECURITY HARDENING
# =============================================================================

echo "🔒 Step 6: Security Hardening"
echo "-----------------------------"

# Firewall rules (if ufw is available)
if command -v ufw >/dev/null 2>&1; then
    echo "Configuring firewall..."
    
    # Allow SSH (assuming it's needed)
    ufw allow ssh
    
    # Allow P2P port
    ufw allow 40999/tcp comment "Dinero P2P"
    
    # Allow RPC only from localhost (already bound to 127.0.0.1)
    # Allow monitoring from localhost (already bound to 127.0.0.1)
    
    # Enable firewall if not already enabled
    ufw --force enable
    
    echo "✅ Firewall configured"
else
    echo "⚠️  UFW not available. Configure firewall manually:"
    echo "   - Allow port 40999/tcp for P2P"
    echo "   - Block ports 20999, 22001 from external access"
fi

# Secure log directory
echo "Securing log directory..."
chmod 750 "$DINERO_LOGS"
chown -R "$DINERO_USER:adm" "$DINERO_LOGS"

# Logrotate configuration
cat > /etc/logrotate.d/dinerod << EOF
$DINERO_LOGS/*.log {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    create 640 $DINERO_USER adm
    postrotate
        systemctl reload dinerod
    endscript
}
EOF

echo "✅ Security hardening applied"
echo ""

# =============================================================================
# STEP 7: HEALTH CHECKS
# =============================================================================

echo "🏥 Step 7: Health Check Scripts"
echo "------------------------------"

# Create health check script
cat > "$DINERO_BIN/dinero-health" << 'EOF'
#!/bin/bash
# Dinero node health check script

DINERO_CLI="/usr/local/bin/dinero-cli"
RPC_URL="http://127.0.0.1:20999"
HEALTH_URL="http://127.0.0.1:22001/healthz"

echo "🏥 DINERO NODE HEALTH CHECK"
echo "=========================="
echo "Timestamp: $(date)"
echo ""

# Check if daemon is running
if ! pgrep -f dinerod >/dev/null; then
    echo "❌ dinerod process not running"
    exit 1
fi

# Check HTTP health endpoint
echo "📡 HTTP Health Check:"
if curl -s -f "$HEALTH_URL" >/dev/null; then
    echo "✅ HTTP endpoint healthy"
else
    echo "❌ HTTP endpoint not responding"
fi

# Check RPC connectivity
echo ""
echo "🔌 RPC Connectivity:"
if $DINERO_CLI getblockcount >/dev/null 2>&1; then
    BLOCK_COUNT=$($DINERO_CLI getblockcount)
    echo "✅ RPC responding (height: $BLOCK_COUNT)"
else
    echo "❌ RPC not responding"
fi

# Check peer count
echo ""
echo "🌐 Network Status:"
PEER_COUNT=$($DINERO_CLI getconnectioncount 2>/dev/null || echo "unknown")
echo "Peers: $PEER_COUNT"

# Check recent block
LAST_BLOCK_TIME=$($DINERO_CLI getblock $($DINERO_CLI getbestblockhash) | jq -r '.time' 2>/dev/null || echo "0")
CURRENT_TIME=$(date +%s)
BLOCK_AGE=$((CURRENT_TIME - LAST_BLOCK_TIME))

if [[ $BLOCK_AGE -lt 300 ]]; then
    echo "✅ Recent block (${BLOCK_AGE}s ago)"
elif [[ $BLOCK_AGE -lt 1800 ]]; then
    echo "⚠️  Block age: ${BLOCK_AGE}s"
else
    echo "❌ Stale block: ${BLOCK_AGE}s ago"
fi

echo ""
echo "📊 Quick Stats:"
echo "  Chain work: $($DINERO_CLI getchainwork 2>/dev/null | cut -c1-16)..."
echo "  Mempool: $($DINERO_CLI getmempoolinfo 2>/dev/null | jq -r '.size // "unknown"') txs"

# System resources
echo ""
echo "💻 System Resources:"
echo "  CPU: $(top -bn1 | grep "Cpu(s)" | awk '{print $2}' | cut -d'%' -f1)% used"
echo "  Memory: $(free -h | awk '/^Mem:/ {print $3 "/" $2}')"
echo "  Disk: $(df -h /var/lib/dinerod | awk 'NR==2 {print $3 "/" $2 " (" $5 " used)"}')"

echo ""
echo "🎯 Node Status: $(systemctl is-active dinerod)"
EOF

chmod +x "$DINERO_BIN/dinero-health"

echo "✅ Health check script installed: $DINERO_BIN/dinero-health"
echo ""

# =============================================================================
# STEP 8: FINAL VALIDATION
# =============================================================================

echo "✅ Step 8: Final Validation"
echo "--------------------------"

# Validate configuration
echo "Validating configuration..."
if jq . "$DINERO_CONFIG/nodeinfo.json" >/dev/null 2>&1; then
    echo "✅ Configuration file is valid JSON"
else
    echo "❌ Configuration file is invalid"
    exit 1
fi

# Check binary permissions
if [[ -x "$DINERO_BIN/dinerod" ]]; then
    echo "✅ dinerod binary is executable"
else
    echo "❌ dinerod binary is not executable"
    exit 1
fi

# Test binary (quick version check)
if "$DINERO_BIN/dinerod" --version >/dev/null 2>&1; then
    echo "✅ dinerod binary works"
else
    echo "❌ dinerod binary test failed"
    exit 1
fi

echo ""

# =============================================================================
# DEPLOYMENT COMPLETE
# =============================================================================

echo "🎉 DINERO MAINNET DEPLOYMENT COMPLETE!"
echo "====================================="
echo ""
echo "📋 DEPLOYMENT SUMMARY:"
echo "  Node Type: $NODE_TYPE"
echo "  User: $DINERO_USER"
echo "  Data Directory: $DINERO_HOME"
echo "  Configuration: $DINERO_CONFIG/nodeinfo.json"
echo "  Logs: $DINERO_LOGS"
echo "  Binaries: $DINERO_BIN"
echo ""
echo "🚀 NEXT STEPS:"
echo ""
echo "1. Start the node:"
echo "   systemctl start dinerod"
echo ""
echo "2. Check status:"
echo "   systemctl status dinerod"
echo "   $DINERO_BIN/dinero-health"
echo ""
echo "3. View logs:"
echo "   journalctl -u dinerod -f"
echo "   tail -f $DINERO_LOGS/dinerod.log"
echo ""
echo "4. Monitor the node:"
echo "   # RPC commands"
echo "   $DINERO_BIN/dinero-cli getblockcount"
echo "   $DINERO_BIN/dinero-cli getchaintips"
echo "   $DINERO_BIN/dinero-cli getreorgstatus"
echo ""
echo "   # HTTP endpoints"
echo "   curl http://127.0.0.1:22001/healthz"
echo "   curl http://127.0.0.1:22001/metrics"
echo ""

if [[ $NODE_TYPE == "miner" ]]; then
    echo "5. Configure mining:"
    echo "   # Generate mining address"
    echo "   $DINERO_BIN/dinero-cli getnewaddress"
    echo ""
    echo "   # Update config with mining address"
    echo "   jq '.mining.address = \"din1...\" | .mining.threads = $(nproc)' \\"
    echo "     $DINERO_CONFIG/nodeinfo.json > /tmp/nodeinfo.json"
    echo "   mv /tmp/nodeinfo.json $DINERO_CONFIG/nodeinfo.json"
    echo ""
    echo "   # Restart to apply mining config"
    echo "   systemctl restart dinerod"
    echo ""
fi

echo "6. Security reminders:"
echo "   - RPC is bound to localhost only (secure)"
echo "   - P2P port 40999 is open to internet"
echo "   - Monitor logs for any issues"
echo "   - Keep system and binaries updated"
echo ""
echo "🎯 Your Dinero node is ready for mainnet!"
echo ""
echo "📞 Support:"
echo "   - Documentation: https://docs.dinero-coin.com"
echo "   - Community: https://discord.gg/dinerocoin"
echo "   - Issues: https://github.com/dinerocoin/dinero/issues"
echo ""

# Final system status
echo "💻 Current System Status:"
systemctl is-enabled dinerod && echo "✅ Service enabled" || echo "❌ Service not enabled"
systemctl is-active dinerod && echo "✅ Service running" || echo "⏸️  Service stopped"
echo ""
echo "Ready to launch! 🚀"
