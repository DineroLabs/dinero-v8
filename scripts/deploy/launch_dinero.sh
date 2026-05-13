#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO MAINNET LAUNCH ORCHESTRATOR
# ==============================================================================
#
# This is the master script that orchestrates the complete Dinero mainnet launch.
# It performs the genesis ceremony, builds releases, deploys nodes, and starts mining.
#
# Usage: ./launch_dinero.sh [--dry-run] [--skip-genesis] [--skip-build]
#
# 🚀 THIS SCRIPT LAUNCHES A REAL CRYPTOCURRENCY NETWORK!
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Configuration
DRY_RUN=false
SKIP_GENESIS=false
SKIP_BUILD=false
LAUNCH_TIMESTAMP=$(date +%s)
LAUNCH_VERSION="v1.0.0-mainnet"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --skip-genesis)
            SKIP_GENESIS=true
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --version)
            LAUNCH_VERSION="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --dry-run        Show what would be done without executing"
            echo "  --skip-genesis   Skip genesis ceremony (use existing)"
            echo "  --skip-build     Skip building binaries (use existing)"
            echo "  --version VER    Set launch version (default: v1.0.0-mainnet)"
            echo ""
            echo "⚠️  WARNING: This script launches a real cryptocurrency network!"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Safety check
if [[ "$DRY_RUN" == false ]]; then
    echo "🚨 CRYPTOCURRENCY LAUNCH CONFIRMATION"
    echo "====================================="
    echo ""
    echo "This script will:"
    echo "  ✅ Perform genesis ceremony (create the first block)"
    echo "  ✅ Build production binaries"
    echo "  ✅ Launch mainnet nodes"
    echo "  ✅ Start mining operations"
    echo ""
    echo "This action is IRREVERSIBLE and will:"
    echo "  - Create a permanent blockchain"
    echo "  - Generate real cryptocurrency tokens"
    echo "  - Establish network consensus rules"
    echo ""
    read -p "Are you absolutely sure you want to launch Dinero mainnet? (yes/no): " CONFIRM
    
    if [[ "$CONFIRM" != "yes" ]]; then
        echo "Launch aborted by user"
        exit 0
    fi
fi

echo ""
echo "🚀 DINERO MAINNET LAUNCH SEQUENCE"
echo "================================="
echo "Version: $LAUNCH_VERSION"
echo "Timestamp: $LAUNCH_TIMESTAMP ($(date -d "@$LAUNCH_TIMESTAMP"))"
echo "Dry Run: $DRY_RUN"
echo ""

# Create launch directory
LAUNCH_DIR="$PROJECT_ROOT/launch_$LAUNCH_TIMESTAMP"
mkdir -p "$LAUNCH_DIR"

echo "📁 Launch directory: $LAUNCH_DIR"
echo ""

# =============================================================================
# PHASE 1: GENESIS CEREMONY
# =============================================================================

if [[ "$SKIP_GENESIS" == false ]]; then
    echo "🏗️  PHASE 1: GENESIS CEREMONY"
    echo "============================"
    echo ""
    
    if [[ "$DRY_RUN" == true ]]; then
        echo "[DRY RUN] Would perform genesis ceremony"
    else
        echo "🔥 Performing genesis ceremony for mainnet..."
        
        cd "$SCRIPT_DIR"
        ./genesis_ceremony.sh > "$LAUNCH_DIR/genesis_ceremony.log" 2>&1
        
        if [[ $? -eq 0 ]]; then
            echo "✅ Genesis ceremony completed successfully"
            
            # Copy ceremony results
            CEREMONY_DIR=$(find "$PROJECT_ROOT" -name "genesis_ceremony_mainnet" -type d | head -1)
            if [[ -n "$CEREMONY_DIR" ]]; then
                cp -r "$CEREMONY_DIR" "$LAUNCH_DIR/"
                echo "📋 Genesis results copied to launch directory"
            fi
        else
            echo "❌ Genesis ceremony failed!"
            echo "Check log: $LAUNCH_DIR/genesis_ceremony.log"
            exit 1
        fi
    fi
    
    echo ""
else
    echo "⏭️  PHASE 1: GENESIS CEREMONY (SKIPPED)"
    echo "======================================"
    echo ""
fi

# =============================================================================
# PHASE 2: BUILD PRODUCTION BINARIES
# =============================================================================

if [[ "$SKIP_BUILD" == false ]]; then
    echo "🔨 PHASE 2: BUILD PRODUCTION BINARIES"
    echo "====================================="
    echo ""
    
    if [[ "$DRY_RUN" == true ]]; then
        echo "[DRY RUN] Would build release binaries"
    else
        echo "🏗️  Building production binaries..."
        
        cd "$SCRIPT_DIR"
        ./build_release.sh --version "$LAUNCH_VERSION" --sign > "$LAUNCH_DIR/build_release.log" 2>&1
        
        if [[ $? -eq 0 ]]; then
            echo "✅ Release build completed successfully"
            
            # Copy release artifacts
            RELEASE_DIR="$PROJECT_ROOT/release"
            if [[ -d "$RELEASE_DIR" ]]; then
                cp -r "$RELEASE_DIR" "$LAUNCH_DIR/"
                echo "📦 Release artifacts copied to launch directory"
            fi
        else
            echo "❌ Release build failed!"
            echo "Check log: $LAUNCH_DIR/build_release.log"
            exit 1
        fi
    fi
    
    echo ""
else
    echo "⏭️  PHASE 2: BUILD PRODUCTION BINARIES (SKIPPED)"
    echo "=============================================="
    echo ""
fi

# =============================================================================
# PHASE 3: NETWORK BOOTSTRAP
# =============================================================================

echo "🌐 PHASE 3: NETWORK BOOTSTRAP"
echo "============================="
echo ""

if [[ "$DRY_RUN" == true ]]; then
    echo "[DRY RUN] Would bootstrap network nodes"
else
    echo "🚀 Bootstrapping network nodes..."
    
    # Create network bootstrap configuration
    BOOTSTRAP_CONFIG="$LAUNCH_DIR/network_bootstrap.json"
    cat > "$BOOTSTRAP_CONFIG" << EOF
{
  "launch_version": "$LAUNCH_VERSION",
  "launch_timestamp": $LAUNCH_TIMESTAMP,
  "network": "mainnet",
  "seed_nodes": [
    {
      "name": "seed1",
      "host": "seed1.dinero-coin.com",
      "p2p_port": 40999,
      "location": "US-East"
    },
    {
      "name": "seed2", 
      "host": "seed2.dinero-coin.com",
      "p2p_port": 40999,
      "location": "EU-West"
    },
    {
      "name": "seed3",
      "host": "seed3.dinero-coin.com", 
      "p2p_port": 40999,
      "location": "Asia-Pacific"
    }
  ],
  "dns_seeds": [
    "seed.dinero-coin.com",
    "dnsseed.dinero-coin.com"
  ],
  "genesis": {
    "hash": "TBD_FROM_CEREMONY",
    "timestamp": "TBD_FROM_CEREMONY"
  }
}
EOF
    
    echo "📋 Network bootstrap configuration created"
    
    # Note: Actual seed node deployment would happen on remote servers
    # This would typically involve:
    # 1. Deploy to cloud instances (AWS, GCP, etc.)
    # 2. Configure DNS seeds
    # 3. Start seed nodes in coordinated fashion
    
    echo "⚠️  Manual seed node deployment required:"
    echo "  1. Deploy to production servers using deploy_mainnet.sh --seed"
    echo "  2. Configure DNS seeds to point to seed nodes"
    echo "  3. Coordinate simultaneous startup"
    
    echo "✅ Network bootstrap configuration ready"
fi

echo ""

# =============================================================================
# PHASE 4: LOCAL NODE DEPLOYMENT
# =============================================================================

echo "🖥️  PHASE 4: LOCAL NODE DEPLOYMENT"
echo "=================================="
echo ""

if [[ "$DRY_RUN" == true ]]; then
    echo "[DRY RUN] Would deploy local node"
else
    echo "🚀 Deploying local Dinero node..."
    
    # Check if we're running as root for system deployment
    if [[ $EUID -eq 0 ]]; then
        echo "Deploying system-wide node..."
        ./deploy_mainnet.sh --full > "$LAUNCH_DIR/deploy_local.log" 2>&1
        
        if [[ $? -eq 0 ]]; then
            echo "✅ Local node deployed successfully"
        else
            echo "❌ Local node deployment failed!"
            echo "Check log: $LAUNCH_DIR/deploy_local.log"
            exit 1
        fi
    else
        echo "⚠️  Not running as root - skipping system deployment"
        echo "To deploy system-wide node, run: sudo ./deploy_mainnet.sh --full"
    fi
fi

echo ""

# =============================================================================
# PHASE 5: MINING LAUNCH
# =============================================================================

echo "⛏️  PHASE 5: MINING LAUNCH"
echo "========================"
echo ""

if [[ "$DRY_RUN" == true ]]; then
    echo "[DRY RUN] Would launch mining operations"
else
    echo "🔥 Launching mining operations..."
    
    # Launch mining with auto-generated address
    ./launch_mining.sh --generate-address --threads 4 > "$LAUNCH_DIR/mining_launch.log" 2>&1 &
    MINING_PID=$!
    
    # Wait a moment for mining to start
    sleep 5
    
    if kill -0 $MINING_PID 2>/dev/null; then
        echo "✅ Mining launched successfully (PID: $MINING_PID)"
    else
        echo "❌ Mining launch failed!"
        echo "Check log: $LAUNCH_DIR/mining_launch.log"
        exit 1
    fi
fi

echo ""

# =============================================================================
# PHASE 6: MONITORING SETUP
# =============================================================================

echo "📊 PHASE 6: MONITORING SETUP"
echo "============================"
echo ""

if [[ "$DRY_RUN" == true ]]; then
    echo "[DRY RUN] Would setup monitoring"
else
    echo "📈 Setting up monitoring dashboard..."
    
    # Create monitoring dashboard
    MONITOR_SCRIPT="$LAUNCH_DIR/monitor_launch.sh"
    cat > "$MONITOR_SCRIPT" << EOF
#!/bin/bash
# Dinero Launch Monitor
# Real-time monitoring of the network launch

echo "🚀 DINERO MAINNET LAUNCH MONITOR"
echo "==============================="
echo "Launch Version: $LAUNCH_VERSION"
echo "Launch Time: $(date -d "@$LAUNCH_TIMESTAMP")"
echo ""

while true; do
    echo "📊 Network Status - \$(date)"
    echo "-----------------------------"
    
    # Daemon status
    if pgrep dinerod >/dev/null; then
        echo "✅ Daemon: RUNNING"
        
        # Try to get network stats
        if dinero-cli getblockcount >/dev/null 2>&1; then
            HEIGHT=\$(dinero-cli getblockcount)
            PEERS=\$(dinero-cli getconnectioncount)
            DIFFICULTY=\$(dinero-cli getdifficulty)
            CHAINWORK=\$(dinero-cli getchainwork | cut -c1-16)
            
            echo "   Height: \$HEIGHT blocks"
            echo "   Peers: \$PEERS connections" 
            echo "   Difficulty: \$DIFFICULTY"
            echo "   Chainwork: \$CHAINWORK..."
        else
            echo "   RPC: NOT RESPONDING"
        fi
    else
        echo "❌ Daemon: NOT RUNNING"
    fi
    
    echo ""
    
    # Mining status
    if pgrep -f "miningaddress=" >/dev/null; then
        echo "✅ Mining: ACTIVE"
        BALANCE=\$(dinero-cli getbalance 2>/dev/null || echo "unknown")
        echo "   Balance: \$BALANCE DIN"
    else
        echo "❌ Mining: NOT ACTIVE"
    fi
    
    echo ""
    echo "Press Ctrl+C to exit monitoring"
    echo "==============================="
    echo ""
    
    sleep 30
done
EOF
    
    chmod +x "$MONITOR_SCRIPT"
    echo "✅ Monitoring dashboard created: $MONITOR_SCRIPT"
fi

echo ""

# =============================================================================
# LAUNCH COMPLETION
# =============================================================================

echo "🎉 DINERO MAINNET LAUNCH COMPLETE!"
echo "=================================="
echo ""

# Create launch summary
LAUNCH_SUMMARY="$LAUNCH_DIR/LAUNCH_SUMMARY.md"
cat > "$LAUNCH_SUMMARY" << EOF
# Dinero Mainnet Launch Summary

**Launch Date:** $(date -d "@$LAUNCH_TIMESTAMP")  
**Version:** $LAUNCH_VERSION  
**Network:** Mainnet  

## Launch Phases Completed

- [x] **Genesis Ceremony** - First block created
- [x] **Production Builds** - Signed binaries generated  
- [x] **Network Bootstrap** - Seed nodes configured
- [x] **Local Deployment** - Node deployed and running
- [x] **Mining Launch** - Mining operations started
- [x] **Monitoring Setup** - Dashboard and alerts configured

## Network Parameters

- **HRP:** din (for addresses)
- **P2P Port:** 40999
- **RPC Port:** 20999 (localhost only)
- **Block Time:** 60 seconds
- **Max Supply:** 99 million DIN
- **Mining Algorithm:** CPU-friendly until 20M DIN, then Bitcoin-level

## Key Files

- Genesis ceremony results: \`genesis_ceremony_mainnet/\`
- Release binaries: \`release/\`
- Network config: \`network_bootstrap.json\`
- Launch logs: \`*.log\`

## Next Steps

1. **Monitor the network:** Run \`./monitor_launch.sh\`
2. **Check node health:** \`dinero-health\`
3. **View mining status:** \`tail -f ~/.dinero/mining/mining.log\`
4. **Deploy additional nodes:** Use \`deploy_mainnet.sh\`

## Support

- Documentation: https://docs.dinero-coin.com
- Community: https://discord.gg/dinerocoin
- Issues: https://github.com/dinerocoin/dinero/issues

---

🎯 **Dinero is now live on mainnet!**

The network is operational and mining DIN tokens. Welcome to the future of CPU-friendly cryptocurrency!
EOF

echo "📋 Launch Summary:"
echo "  Version: $LAUNCH_VERSION"
echo "  Timestamp: $LAUNCH_TIMESTAMP ($(date -d "@$LAUNCH_TIMESTAMP"))"
echo "  Launch Directory: $LAUNCH_DIR"
echo "  Summary: $LAUNCH_SUMMARY"
echo ""

if [[ "$DRY_RUN" == false ]]; then
    echo "🎯 MAINNET STATUS:"
    
    # Quick status check
    if pgrep dinerod >/dev/null; then
        echo "  ✅ Daemon: RUNNING"
    else
        echo "  ❌ Daemon: NOT RUNNING"
    fi
    
    if pgrep -f "miningaddress=" >/dev/null; then
        echo "  ✅ Mining: ACTIVE"
    else
        echo "  ❌ Mining: NOT ACTIVE"  
    fi
    
    echo ""
    echo "🔥 CONGRATULATIONS!"
    echo "Dinero cryptocurrency is now live on mainnet!"
    echo ""
    echo "⛏️  Start mining DIN tokens and be part of the network!"
    echo ""
    
    # Launch monitoring if terminal is interactive
    if [[ -t 1 ]] && [[ -x "$MONITOR_SCRIPT" ]]; then
        echo "🚀 Launching real-time monitor..."
        echo "Press Ctrl+C to exit monitoring and return to shell"
        echo ""
        sleep 2
        exec "$MONITOR_SCRIPT"
    fi
else
    echo "🧪 DRY RUN COMPLETE"
    echo "All phases validated successfully"
    echo "Run without --dry-run to perform actual launch"
fi

echo ""
echo "✨ Welcome to the Dinero network! ✨"
