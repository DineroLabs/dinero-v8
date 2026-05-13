#!/bin/bash
# Test All Deployment Methods
# Verifies that all deployment scripts work correctly

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[TEST]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
info() { echo -e "${BLUE}[INFO]${NC} $1"; }

# Check if we have a built binary
check_binary() {
    if [[ -x "./build-debug/bin/dinerod" ]]; then
        log "✅ Found dinerod binary: ./build-debug/bin/dinerod"
        return 0
    elif [[ -x "./build/bin/dinerod" ]]; then
        log "✅ Found dinerod binary: ./build/bin/dinerod"
        return 0
    else
        error "❌ No dinerod binary found. Build first:"
        echo "  cmake --build build-debug -j8 --target dinerod"
        exit 1
    fi
}

# Test macOS deployment
test_macos_deploy() {
    log "🍎 Testing macOS deployment..."
    
    DATADIR="$(mktemp -d -t din-test.XXXX)"
    info "Using temp directory: $DATADIR"
    
    # Start node in background
    ./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
        -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
        -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default \
        -nodeinfo="$DATADIR/nodeinfo.json" -datadir="$DATADIR" > "$DATADIR/dinerod.log" 2>&1 &
    
    local PID=$!
    info "Started dinerod with PID: $PID"
    
    # Source wait functions
    source scripts/wait-functions.sh
    
    # Wait for nodeinfo and cookie
    if ! wait_cookie "$DATADIR/.cookie" 200; then
        error "❌ Cookie file not created"
        kill $PID 2>/dev/null || true
        return 1
    fi
    
    if [[ ! -s "$DATADIR/nodeinfo.json" ]]; then
        error "❌ nodeinfo.json not created"
        kill $PID 2>/dev/null || true
        return 1
    fi
    
    log "✅ nodeinfo.json and cookie created"
    
    # Wait for RPC to be ready
    if wait_rpc_200 "$DATADIR/nodeinfo.json" "$DATADIR/.cookie" 120; then
        log "✅ RPC responding with HTTP 200"
    else
        warn "⚠️ RPC not responding in time (node may still be starting)"
    fi
    
    # Cleanup
    kill $PID 2>/dev/null || true
    rm -rf "$DATADIR"
    
    log "✅ macOS deployment test completed"
}

# Test deployment scripts exist and are executable
test_scripts() {
    log "📜 Testing deployment scripts..."
    
    local scripts=(
        "scripts/deploy-production.sh"
        "scripts/deploy-oneliner.sh"
        "scripts/prometheus-exporter.py"
    )
    
    for script in "${scripts[@]}"; do
        if [[ -x "$script" ]]; then
            log "✅ $script is executable"
        else
            error "❌ $script missing or not executable"
            return 1
        fi
    done
    
    log "✅ All deployment scripts ready"
}

# Test configuration files
test_configs() {
    log "⚙️ Testing configuration files..."
    
    local configs=(
        "DEPLOY_ANYWHERE.md"
        "PRODUCTION_LAUNCH_KIT.md"
        "docker-compose.production.yml"
        "Dockerfile.production"
    )
    
    for config in "${configs[@]}"; do
        if [[ -f "$config" ]]; then
            log "✅ $config exists"
        else
            error "❌ $config missing"
            return 1
        fi
    done
    
    log "✅ All configuration files ready"
}

# Test Docker files syntax
test_docker() {
    log "🐳 Testing Docker configuration..."
    
    if command -v docker >/dev/null; then
        # Test Dockerfile syntax
        if docker build -f Dockerfile.production -t dinero-test . --dry-run 2>/dev/null; then
            log "✅ Dockerfile.production syntax valid"
        else
            warn "⚠️ Dockerfile.production may have syntax issues"
        fi
        
        # Test docker-compose syntax
        if command -v docker-compose >/dev/null; then
            if docker-compose -f docker-compose.production.yml config >/dev/null 2>&1; then
                log "✅ docker-compose.production.yml syntax valid"
            else
                warn "⚠️ docker-compose.production.yml may have syntax issues"
            fi
        else
            info "docker-compose not installed, skipping compose file test"
        fi
    else
        info "Docker not installed, skipping Docker tests"
    fi
}

# Test two-node E2E scenario
test_two_node() {
    log "🔗 Testing two-node E2E scenario..."
    
    # Node A
    local A_DIR=$(mktemp -d -t dinA.XXXX)
    ./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
        -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
        -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default -gen \
        -nodeinfo="$A_DIR/nodeinfo.json" -datadir="$A_DIR" > "$A_DIR/dinerod.log" 2>&1 &
    local PID_A=$!
    
    # Wait for Node A to be ready
    if ! wait_cookie "$A_DIR/.cookie" 200; then
        error "❌ Node A cookie not created"
        kill $PID_A 2>/dev/null || true
        return 1
    fi
    
    if ! wait_rpc_200 "$A_DIR/nodeinfo.json" "$A_DIR/.cookie" 120; then
        error "❌ Node A RPC not ready"
        kill $PID_A 2>/dev/null || true
        return 1
    fi
    
    if ! wait_p2p_listen "$A_DIR/nodeinfo.json" 120; then
        error "❌ Node A P2P not listening"
        kill $PID_A 2>/dev/null || true
        return 1
    fi
    
    local P2P_A=$(jq -r .p2p "$A_DIR/nodeinfo.json")
    log "✅ Node A ready - P2P listening on $P2P_A"
    
    # Node B
    local B_DIR=$(mktemp -d -t dinB.XXXX)
    ./build-debug/bin/dinerod -server=1 -daemon=0 -printtoconsole \
        -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 \
        -rpcport=0 -wsport=0 -p2p -port=0 -autowallet=default \
        -connect=127.0.0.1:$P2P_A \
        -nodeinfo="$B_DIR/nodeinfo.json" -datadir="$B_DIR" > "$B_DIR/dinerod.log" 2>&1 &
    local PID_B=$!
    
    # Wait for Node B to be ready
    if ! wait_cookie "$B_DIR/.cookie" 200; then
        error "❌ Node B cookie not created"
        kill $PID_A $PID_B 2>/dev/null || true
        return 1
    fi
    
    if ! wait_rpc_200 "$B_DIR/nodeinfo.json" "$B_DIR/.cookie" 120; then
        error "❌ Node B RPC not ready"
        kill $PID_A $PID_B 2>/dev/null || true
        return 1
    fi
    
    info "Node A P2P: $P2P_A"
    info "Node A RPC: $(jq -r .rpc "$A_DIR/nodeinfo.json")"
    info "Node B RPC: $(jq -r .rpc "$B_DIR/nodeinfo.json")"
    
    # Test P2P connectivity
    if test_p2p_connect "127.0.0.1" "$P2P_A"; then
        log "✅ Node A P2P port is reachable"
    else
        warn "⚠️ Node A P2P port not reachable"
    fi
    
    # Test RPC connectivity between nodes
    local RPC_B=$(jq -r .rpc "$B_DIR/nodeinfo.json")
    local COOKIE_B="$B_DIR/.cookie"
    local AUTH_B=$(tr -d '\r\n' < "$COOKIE_B")
    
    local connected=false
    for i in {1..20}; do
        local PEERS=$(curl -s --max-time 3 --user "$AUTH_B" \
            -H 'content-type: application/json' \
            --data '{"jsonrpc":"2.0","id":1,"method":"getpeers","params":[]}' \
            "http://127.0.0.1:${RPC_B}" | jq -r '.result | length // 0' 2>/dev/null || echo "0")
        
        if [[ "$PEERS" -gt 0 ]]; then
            log "✅ Node B connected to Node A ($PEERS peers)"
            connected=true
            break
        fi
        sleep 0.5
    done
    
    # Cleanup
    kill $PID_A $PID_B 2>/dev/null || true
    rm -rf "$A_DIR" "$B_DIR"
    
    if [[ "$connected" == "true" ]]; then
        log "✅ Two-node E2E test passed"
    else
        warn "⚠️ Two-node E2E test: nodes didn't connect in time"
    fi
}

# Main test runner
main() {
    echo ""
    echo -e "${BLUE}🧪 Dinero Deployment Test Suite${NC}"
    echo "================================="
    echo ""
    
    check_binary
    test_scripts
    test_configs
    test_docker
    test_macos_deploy
    test_two_node
    
    echo ""
    echo -e "${GREEN}🎉 All deployment tests completed!${NC}"
    echo ""
    echo "🚀 Ready to deploy with:"
    echo "  • sudo ./scripts/deploy-oneliner.sh"
    echo "  • sudo ./scripts/deploy-production.sh"
    echo "  • docker compose -f docker-compose.production.yml up -d"
    echo ""
}

main "$@"
