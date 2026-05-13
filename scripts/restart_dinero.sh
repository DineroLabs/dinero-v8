#!/bin/bash
# restart_dinero.sh - Safe restart script for production Dinero daemons
# Prevents ghost daemons by ensuring clean shutdown and restart
#
# Usage:
#   restart_dinero.sh [datadir] [rpcport] [port] [wsport]
#
# Example:
#   restart_dinero.sh /root/.dinero 20998 20999 21001

set -e

# Configuration (can be overridden via command line)
DATA_DIR="${1:-/root/.dinero}"
RPC_PORT="${2:-20998}"
P2P_PORT="${3:-20999}"
WS_PORT="${4:-21001}"
DAEMON_BINARY="${DAEMON_BINARY:-/root/DineroCoin/build/bin/dinerod}"
LOG_FILE="${LOG_FILE:-/root/dinero.log}"

echo "========================================="
echo "  Dinero Daemon Restart Script"
echo "========================================="
echo ""

# Step 1: Stop all old daemons
echo "🛑 Stopping all existing dinerod processes..."
pkill -9 dinerod || true
sleep 3

# Verify no processes remain
if pgrep -f dinerod > /dev/null; then
    echo "⚠️  Warning: Some dinerod processes may still be running"
    pkill -9 dinerod || true
    sleep 2
fi

# Step 2: Clean up stale sockets and locks
echo "🧹 Cleaning up stale locks and sockets..."
rm -f "${DATA_DIR}/.lock" || true
rm -f "${DATA_DIR}/mainnet/.lock" || true
rm -f "${DATA_DIR}/regtest/.lock" || true
rm -f "${DATA_DIR}/testnet/.lock" || true

# Step 3: Verify new binary exists
echo "🔍 Verifying daemon binary..."
if [[ ! -x "${DAEMON_BINARY}" ]]; then
    echo "❌ Error: Daemon binary not found or not executable at ${DAEMON_BINARY}"
    echo "   Please build the daemon first:"
    echo "   cd /root/DineroCoin && cmake --build build -j8"
    exit 1
fi

# Get version info for logging
DAEMON_VERSION=$("${DAEMON_BINARY}" --version 2>&1 | head -1 || echo "unknown")
echo "✅ Binary found: ${DAEMON_BINARY}"
echo "✅ Version: ${DAEMON_VERSION}"

# Step 4: Verify data directory exists
if [[ ! -d "${DATA_DIR}" ]]; then
    echo "📁 Creating data directory: ${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
fi

# Step 5: Start new daemon
echo ""
echo "🚀 Starting daemon..."
echo "   Data dir: ${DATA_DIR}"
echo "   RPC port: ${RPC_PORT}"
echo "   P2P port: ${P2P_PORT}"
echo "   WS port: ${WS_PORT}"
echo ""

# Start daemon in background with logging
nohup "${DAEMON_BINARY}" \
    --datadir="${DATA_DIR}" \
    --rpcport="${RPC_PORT}" \
    --port="${P2P_PORT}" \
    --wsport="${WS_PORT}" \
    --printtoconsole \
    2>&1 | tee "${LOG_FILE}" &

# Store PID for verification
DAEMON_PID=$!
echo "   PID: ${DAEMON_PID}"

# Step 6: Wait for daemon to start
echo ""
echo "⏳ Waiting for daemon to initialize..."
sleep 5

# Step 7: Verify daemon is running
if pgrep -f dinerod > /dev/null; then
    echo "✅ Daemon started successfully"
    
    # Try to verify RPC is responding
    echo "🔍 Verifying RPC endpoint..."
    sleep 2
    if curl -s --user "__cookie__:$(cat "${DATA_DIR}/mainnet/.cookie" 2>/dev/null || echo '')" \
        --data-binary '{"jsonrpc":"1.0","id":"test","method":"getblockchaininfo","params":[]}' \
        -H 'content-type: text/plain;' \
        "http://127.0.0.1:${RPC_PORT}/" > /dev/null 2>&1; then
        echo "✅ RPC endpoint responding"
    else
        echo "⚠️  Warning: RPC endpoint not responding yet (may need more time)"
    fi
    
    echo ""
    echo "✅ Restart complete!"
    echo "   Log file: ${LOG_FILE}"
    echo "   Check status: curl http://127.0.0.1:${RPC_PORT}/metrics"
    exit 0
else
    echo "❌ Error: Daemon failed to start"
    echo "   Check log file: ${LOG_FILE}"
    exit 1
fi
