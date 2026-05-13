#!/bin/bash
# check_dinero_version.sh - Watchdog script to detect and fix ghost daemons
# Checks consensus checksum and version, auto-restarts if mismatch detected
#
# Usage:
#   check_dinero_version.sh [expected_checksum] [rpc_port]
#
# Example:
#   check_dinero_version.sh ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430 20998
#
# To run via cron:
#   */5 * * * * /usr/local/bin/check_dinero_version.sh >> /root/dinero_watchdog.log 2>&1

set -e

# Configuration
EXPECTED_CHECKSUM="${1:-}"
RPC_PORT="${2:-20998}"
METRICS_URL="http://127.0.0.1:${RPC_PORT}/"
RESTART_SCRIPT="${RESTART_SCRIPT:-/usr/local/bin/restart_dinero.sh}"
LOG_FILE="${LOG_FILE:-/root/dinero_watchdog.log}"
DATA_DIR="${DATA_DIR:-/root/.dinero}"

# Timestamp for logging
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# Function to log with timestamp
log() {
    echo "[${TIMESTAMP}] $*" | tee -a "${LOG_FILE}"
}

log "========================================="
log "  Dinero Version Watchdog Check"
log "========================================="

# Step 1: Check if daemon is running
if ! pgrep -f dinerod > /dev/null; then
    log "⚠️  Warning: No dinerod process found"
    log "   Attempting to restart daemon..."
    if [[ -x "${RESTART_SCRIPT}" ]]; then
        "${RESTART_SCRIPT}" "${DATA_DIR}" "${RPC_PORT}" || true
    else
        log "❌ Error: Restart script not found at ${RESTART_SCRIPT}"
        exit 1
    fi
    exit 0
fi

log "✅ Daemon process found"

# Step 2: Check if RPC endpoint is responding
if ! curl -s --max-time 5 "${METRICS_URL}" > /dev/null 2>&1; then
    log "⚠️  Warning: RPC endpoint not responding"
    log "   Daemon may be stuck or starting up"
    log "   Waiting 10 seconds and retrying..."
    sleep 10
    
    if ! curl -s --max-time 5 "${METRICS_URL}" > /dev/null 2>&1; then
        log "❌ Error: RPC endpoint still not responding"
        log "   Restarting daemon..."
        if [[ -x "${RESTART_SCRIPT}" ]]; then
            "${RESTART_SCRIPT}" "${DATA_DIR}" "${RPC_PORT}" || true
        fi
        exit 1
    fi
fi

log "✅ RPC endpoint responding"

# Step 3: Get metrics and extract checksum
log "🔍 Checking consensus checksum..."

# Try to get metrics (may need cookie auth for some endpoints)
METRICS_RESPONSE=$(curl -s --max-time 10 \
    --user "__cookie__:$(cat "${DATA_DIR}/mainnet/.cookie" 2>/dev/null || echo '')" \
    --data-binary '{"jsonrpc":"1.0","id":"test","method":"getmetrics","params":[]}' \
    -H 'content-type: text/plain;' \
    "${METRICS_URL}" 2>&1 || echo "")

if [[ -z "${METRICS_RESPONSE}" ]]; then
    log "⚠️  Warning: Failed to get metrics response"
    exit 1
fi

# Extract consensus checksum from metrics (OpenMetrics format)
# Look for: dinero_consensus_info{checksum="..."}
CURRENT_CHECKSUM=$(echo "${METRICS_RESPONSE}" | grep -oP 'dinero_consensus_info{checksum="\K[^"]+' || echo "")

if [[ -z "${CURRENT_CHECKSUM}" ]]; then
    # Fallback: try to extract from dinero_build_info
    CURRENT_CHECKSUM=$(echo "${METRICS_RESPONSE}" | grep -oP 'dinero_build_info{[^}]*checksum="\K[^"]+' || echo "")
fi

if [[ -z "${CURRENT_CHECKSUM}" ]]; then
    log "⚠️  Warning: Could not extract checksum from metrics"
    log "   Response preview: ${METRICS_RESPONSE:0:200}..."
    exit 1
fi

log "   Current checksum: ${CURRENT_CHECKSUM:0:16}..."

# Step 4: Compare checksums if expected checksum provided
if [[ -n "${EXPECTED_CHECKSUM}" ]]; then
    log "   Expected checksum: ${EXPECTED_CHECKSUM:0:16}..."
    
    if [[ "${CURRENT_CHECKSUM}" != "${EXPECTED_CHECKSUM}" ]]; then
        log "❌ ERROR: Checksum mismatch detected!"
        log "   Current:  ${CURRENT_CHECKSUM}"
        log "   Expected: ${EXPECTED_CHECKSUM}"
        log "   This indicates a ghost daemon or wrong build is running"
        log ""
        log "🔄 Restarting daemon with correct build..."
        
        if [[ -x "${RESTART_SCRIPT}" ]]; then
            "${RESTART_SCRIPT}" "${DATA_DIR}" "${RPC_PORT}" || true
        else
            log "❌ Error: Restart script not found at ${RESTART_SCRIPT}"
            exit 1
        fi
        
        exit 1
    else
        log "✅ Checksum matches expected value"
    fi
else
    log "⚠️  No expected checksum provided (skipping validation)"
    log "   To enable validation, set expected checksum as first argument"
    log "   Example: check_dinero_version.sh ${CURRENT_CHECKSUM}"
fi

# Step 5: Extract and log build info
BUILD_INFO=$(echo "${METRICS_RESPONSE}" | grep -oP 'dinero_build_info{[^}]+' || echo "")
if [[ -n "${BUILD_INFO}" ]]; then
    log "📊 Build info: ${BUILD_INFO}"
fi

# Step 6: Check version info
VERSION_INFO=$(echo "${METRICS_RESPONSE}" | grep -oP 'dinero_version_info{version="\K[^"]+' || echo "")
if [[ -n "${VERSION_INFO}" ]]; then
    log "📦 Version: ${VERSION_INFO}"
fi

log "✅ All checks passed"
log ""

exit 0
